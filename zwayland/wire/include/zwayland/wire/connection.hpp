#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "zwayland/wire/message.hpp"

namespace zwayland::wire {

// Compacts instead of chaining so every message is contiguous.
class ByteBuffer {
 public:
  [[nodiscard]] std::span<const std::byte> readable() const {
    return {storage_.data() + head_, tail_ - head_};
  }
  [[nodiscard]] std::size_t size() const { return tail_ - head_; }
  [[nodiscard]] bool empty() const { return head_ == tail_; }
  [[nodiscard]] std::size_t capacity() const { return storage_.size(); }

  // Returns at least `bytes` of writable space at the tail.
  std::span<std::byte> prepare(std::size_t bytes);
  void commit(std::size_t bytes) { tail_ += bytes; }
  void append(std::span<const std::byte> bytes);
  void drop(std::size_t bytes);
  // Releases memory left over from a burst once the buffer drains.
  void trim(std::size_t keep);
  void swap(ByteBuffer& other) noexcept;

 private:
  std::vector<std::byte> storage_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};

// Wayland transport with explicit FD ownership and zero-copy loopback.
class Connection {
 public:
  explicit Connection(int fd);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  [[nodiscard]] int fd() const { return fd_; }

  // Drains available input; false when nothing new arrived.
  [[nodiscard]] bool read_available();

  // Sends as much queued output as the socket takes. False on error.
  [[nodiscard]] bool flush();

  // Copies bytes and takes ownership of FDs. False if the backlog cap is hit.
  bool queue(std::span<const std::byte> bytes, std::span<const int> fds);
  void queue(std::vector<std::byte> bytes, std::vector<int> fds);

  // Pinned until consume(); input arriving meanwhile is staged.
  [[nodiscard]] std::optional<Message> next_message();

  // Consumes the pinned message; delivered FDs belong to the caller.
  void consume(std::size_t size, std::size_t fds_consumed);

  // Both loopback endpoints must link to each other.
  void link(Connection& peer);
  void set_data_ready(std::function<void()> ready) { data_ready_ = std::move(ready); }
  // 0 disables the cap. Exceeding it fails the connection instead of growing.
  void set_max_output(std::size_t bytes) { max_output_ = bytes; }

  [[nodiscard]] bool has_input() const { return !input_.empty() || !staged_.empty(); }
  [[nodiscard]] bool has_output() const { return !output_.empty() || !output_fds_.empty(); }
  [[nodiscard]] std::size_t pending_output() const { return output_.size(); }
  [[nodiscard]] bool loopback() const { return loopback_; }
  [[nodiscard]] bool healthy() const {
    return !failed_ && (!loopback_ || (link_alive_ && link_alive_->load()));
  }
  [[nodiscard]] bool peer_closed() const { return peer_closed_; }

 private:
  [[nodiscard]] ByteBuffer& input_target() { return pinned_ ? staged_ : input_; }
  void unstage();
  void close_output_fds();
  void notify_data_ready();
  void accept_loopback(ByteBuffer& bytes, std::vector<int>& fds);

  int fd_ = -1;
  bool loopback_ = false;
  Connection* peer_ = nullptr;
  // Avoids dereferencing a destroyed loopback peer.
  std::shared_ptr<std::atomic<bool>> link_alive_;
  std::function<void()> data_ready_;
  bool notifying_data_ready_ = false;
  bool data_ready_pending_ = false;
  bool peer_closed_ = false;
  bool failed_ = false;
  bool pinned_ = false;
  std::size_t max_output_ = 0;

  ByteBuffer input_;
  ByteBuffer staged_;  // input received while a message is pinned
  ByteBuffer output_;
  std::vector<int> input_fds_;
  std::size_t input_fd_cursor_ = 0;
  std::vector<int> output_fds_;
};

}  // namespace zwayland::wire
