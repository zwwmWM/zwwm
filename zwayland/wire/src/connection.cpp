#include "zwayland/wire/connection.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace zwayland::wire {

namespace {

constexpr std::size_t kRecvChunk = 16384;
constexpr std::size_t kMaxControlBytes = 4096;
constexpr std::uint32_t kMaxMessageSize = 4096 * 16;
constexpr std::size_t kMaxBufferedInput = 16 * 1024 * 1024;
constexpr std::size_t kMaxFdsPerSend = 28;
constexpr std::size_t kIdleBufferKeep = 64 * 1024;

std::uint32_t load_u32(const std::byte* bytes) {
  std::uint32_t value;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

}  // namespace

// ---- ByteBuffer -------------------------------------------------------------

std::span<std::byte> ByteBuffer::prepare(std::size_t bytes) {
  if (storage_.size() - tail_ < bytes && head_ > 0) {
    const std::size_t live = tail_ - head_;
    if (live > 0) std::memmove(storage_.data(), storage_.data() + head_, live);
    head_ = 0;
    tail_ = live;
  }
  if (storage_.size() - tail_ < bytes)
    storage_.resize(std::max({storage_.size() * 2, tail_ + bytes, std::size_t{4096}}));
  return {storage_.data() + tail_, storage_.size() - tail_};
}

void ByteBuffer::append(std::span<const std::byte> bytes) {
  if (bytes.empty()) return;
  std::memcpy(prepare(bytes.size()).data(), bytes.data(), bytes.size());
  commit(bytes.size());
}

void ByteBuffer::drop(std::size_t bytes) {
  head_ += std::min(bytes, tail_ - head_);
  if (head_ == tail_) head_ = tail_ = 0;
}

void ByteBuffer::trim(std::size_t keep) {
  if (!empty() || storage_.size() <= keep) return;
  storage_.resize(keep);
  storage_.shrink_to_fit();
}

void ByteBuffer::swap(ByteBuffer& other) noexcept {
  storage_.swap(other.storage_);
  std::swap(head_, other.head_);
  std::swap(tail_, other.tail_);
}

// ---- Connection -------------------------------------------------------------

Connection::Connection(int fd) : fd_(fd) {}

void Connection::link(Connection& peer) {
  loopback_ = true;
  peer_ = &peer;
  if (!link_alive_) link_alive_ = std::make_shared<std::atomic<bool>>(true);
  peer.link_alive_ = link_alive_;
}

Connection::~Connection() {
  close_output_fds();
  // FDs not yet delivered to a handler are still ours to close.
  for (std::size_t i = input_fd_cursor_; i < input_fds_.size(); ++i) close(input_fds_[i]);
  if (link_alive_) link_alive_->store(false);
  if (fd_ >= 0) close(fd_);
}

void Connection::close_output_fds() {
  for (int fd : output_fds_) close(fd);
  output_fds_.clear();
}

void Connection::notify_data_ready() {
  if (!data_ready_) return;
  if (notifying_data_ready_) {
    data_ready_pending_ = true;
    return;
  }
  notifying_data_ready_ = true;
  do {
    data_ready_pending_ = false;
    data_ready_();
  } while (data_ready_pending_);
  notifying_data_ready_ = false;
}

void Connection::unstage() {
  if (pinned_ || staged_.empty()) return;
  if (input_.empty()) {
    input_.swap(staged_);
  } else {
    input_.append(staged_.readable());
    staged_.drop(staged_.size());
  }
}

bool Connection::read_available() {
  if (loopback_) return link_alive_ && link_alive_->load() && has_input();

  bool got_any = false;
  for (;;) {
    ByteBuffer& target = input_target();
    const std::span<std::byte> space = target.prepare(kRecvChunk);
    alignas(struct cmsghdr) std::byte control[kMaxControlBytes];

    struct iovec iov {
      space.data(), space.size()
    };
    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    const ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n < 0) {
      failed_ = true;
      break;
    }
    if (n == 0) {
      peer_closed_ = true;
      break;
    }
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
      const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const auto* data = reinterpret_cast<const unsigned char*>(CMSG_DATA(cmsg));
      for (std::size_t i = 0; i < count; ++i) {
        int fd;
        std::memcpy(&fd, data + i * sizeof(int), sizeof(int));
        input_fds_.push_back(fd);
      }
    }
    if ((msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
      failed_ = true;
      break;
    }
    target.commit(static_cast<std::size_t>(n));
    got_any = true;
    if (input_.size() + staged_.size() > kMaxBufferedInput) {
      failed_ = true;
      break;
    }
    // A short read means the socket is drained; skip the EAGAIN syscall.
    if (static_cast<std::size_t>(n) < space.size()) break;
  }
  return got_any;
}

void Connection::accept_loopback(ByteBuffer& bytes, std::vector<int>& fds) {
  ByteBuffer& target = input_target();
  if (target.empty()) {
    target.swap(bytes);  // zero-copy hand-off of the whole batch
  } else {
    target.append(bytes.readable());
    bytes.drop(bytes.size());
  }
  // FDs are not closed: ownership transfers within the process.
  input_fds_.insert(input_fds_.end(), fds.begin(), fds.end());
  fds.clear();
}

bool Connection::flush() {
  if (loopback_) {
    if (!link_alive_ || !link_alive_->load() || peer_ == nullptr) {
      failed_ = true;
      return false;
    }
    if (!has_output()) return true;
    peer_->accept_loopback(output_, output_fds_);
    peer_->notify_data_ready();
    return true;
  }

  while (!output_.empty()) {
    const std::span<const std::byte> data = output_.readable();
    struct iovec iov {
      const_cast<std::byte*>(data.data()), data.size()
    };
    // SCM_RIGHTS needs a payload byte; keep bytes back while more FD batches remain.
    if (output_fds_.size() > kMaxFdsPerSend) iov.iov_len = 1;

    alignas(struct cmsghdr) std::byte control[CMSG_SPACE(kMaxFdsPerSend * sizeof(int))]{};
    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    const std::size_t fd_count = std::min(output_fds_.size(), kMaxFdsPerSend);
    if (fd_count > 0) {
      msg.msg_control = control;
      msg.msg_controllen = CMSG_SPACE(fd_count * sizeof(int));
      struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
      std::memcpy(CMSG_DATA(cmsg), output_fds_.data(), fd_count * sizeof(int));
    }

    ssize_t n;
    do {
      n = sendmsg(fd_, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    if (n <= 0) {
      failed_ = true;
      return false;
    }

    // SCM_RIGHTS duplicated the descriptors into the receiver.
    for (std::size_t i = 0; i < fd_count; ++i) close(output_fds_[i]);
    output_fds_.erase(output_fds_.begin(), output_fds_.begin() + static_cast<std::ptrdiff_t>(fd_count));
    output_.drop(static_cast<std::size_t>(n));
  }
  output_.trim(kIdleBufferKeep);
  return true;
}

bool Connection::queue(std::span<const std::byte> bytes, std::span<const int> fds) {
  if (failed_ || (max_output_ != 0 && output_.size() + bytes.size() > max_output_)) {
    failed_ = true;
    for (int fd : fds) close(fd);
    return false;
  }
  try {
    output_fds_.reserve(output_fds_.size() + fds.size());
    output_.append(bytes);
  } catch (...) {
    for (int fd : fds) close(fd);
    throw;
  }
  output_fds_.insert(output_fds_.end(), fds.begin(), fds.end());
  return true;
}

void Connection::queue(std::vector<std::byte> bytes, std::vector<int> fds) {
  (void)queue(std::span<const std::byte>(bytes), std::span<const int>(fds));
}

std::optional<Message> Connection::next_message() {
  unstage();
  const std::span<const std::byte> avail = input_.readable();
  if (avail.size() < 8) return std::nullopt;

  const std::uint32_t header = load_u32(avail.data() + 4);
  const std::uint32_t size = header >> 16;
  if (size < 8 || size > kMaxMessageSize || size % 4 != 0) {
    failed_ = true;
    return std::nullopt;
  }
  if (avail.size() < size) return std::nullopt;

  Message message;
  message.object_id = load_u32(avail.data());
  message.opcode = header & 0xffff;
  message.size = size;
  message.body = avail.subspan(8, size - 8);
  message.fds = std::span<const int>(input_fds_).subspan(input_fd_cursor_);
  pinned_ = true;
  return message;
}

void Connection::consume(std::size_t size, std::size_t fds_consumed) {
  input_.drop(size);
  input_fd_cursor_ = std::min(input_fd_cursor_ + fds_consumed, input_fds_.size());
  if (input_fd_cursor_ == input_fds_.size()) {
    input_fds_.clear();
    input_fd_cursor_ = 0;
  }
  pinned_ = false;
  unstage();
  if (input_.empty()) input_.trim(kIdleBufferKeep);
}

}  // namespace zwayland::wire
