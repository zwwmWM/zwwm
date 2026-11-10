#include "zwayland/server/event_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#endif

namespace zwayland::server {

namespace {

constexpr int kMaxEvents = 64;

std::uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

#if defined(__FreeBSD__)
constexpr std::uintptr_t kWakeIdent = 0;
constexpr std::uintptr_t kTimerIdent = 1;

bool kevent_change(int kq, std::uintptr_t ident, short filter, unsigned short flags,
                   unsigned int fflags = 0, std::intptr_t data = 0) {
  struct kevent change;
  EV_SET(&change, ident, filter, flags, fflags, data, nullptr);
  return kevent(kq, &change, 1, nullptr, 0, nullptr) == 0;
}
#else
static_assert(kEventReadable == EPOLLIN && kEventWritable == EPOLLOUT &&
              kEventError == EPOLLERR && kEventHangup == EPOLLHUP);

void watch(int epoll_fd, int fd) {
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    throw std::runtime_error("epoll add failed");
}
#endif

}  // namespace

#if defined(__FreeBSD__)
EventLoop::EventLoop() {
  poll_fd_ = kqueue();
  if (poll_fd_ < 0) throw std::runtime_error("kqueue failed");
  if (fcntl(poll_fd_, F_SETFD, FD_CLOEXEC) < 0 ||
      !kevent_change(poll_fd_, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR)) {
    close(poll_fd_);
    throw std::runtime_error("kqueue setup failed");
  }
}

EventLoop::~EventLoop() {
  for (int fd : owned_fds_) close(fd);
  for (const auto& [handle, signal_number] : signals_)
    kevent_change(poll_fd_, static_cast<std::uintptr_t>(signal_number), EVFILT_SIGNAL, EV_DELETE);
  close(poll_fd_);
}

void EventLoop::wake() {
  kevent_change(poll_fd_, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER);
}

void EventLoop::add_fd_filters(int fd, int old_mask, int new_mask) {
  const auto ident = static_cast<std::uintptr_t>(fd);
  for (const auto [bit, filter] : {std::pair{kEventReadable, EVFILT_READ},
                                   std::pair{kEventWritable, EVFILT_WRITE}}) {
    const bool had = (old_mask & bit) != 0;
    const bool want = (new_mask & bit) != 0;
    if (want && !had && !kevent_change(poll_fd_, ident, static_cast<short>(filter), EV_ADD))
      throw std::runtime_error("kevent add failed");
    if (had && !want) kevent_change(poll_fd_, ident, static_cast<short>(filter), EV_DELETE);
  }
}
#else
EventLoop::EventLoop() {
  poll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (poll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  try {
    if (wake_fd_ < 0 || timer_fd_ < 0) throw std::runtime_error("loop fd creation failed");
    watch(poll_fd_, wake_fd_);
    watch(poll_fd_, timer_fd_);
  } catch (...) {
    if (wake_fd_ >= 0) close(wake_fd_);
    if (timer_fd_ >= 0) close(timer_fd_);
    close(poll_fd_);
    throw;
  }
}

EventLoop::~EventLoop() {
  for (int fd : owned_fds_) close(fd);
  close(timer_fd_);
  close(wake_fd_);
  close(poll_fd_);
}

void EventLoop::wake() {
  std::uint64_t one = 1;
  [[maybe_unused]] ssize_t n = write(wake_fd_, &one, sizeof(one));
}

void EventLoop::add_fd_filters(int fd, int old_mask, int new_mask) {
  epoll_event ev{};
  ev.events = static_cast<std::uint32_t>(new_mask);
  ev.data.fd = fd;
  if (epoll_ctl(poll_fd_, old_mask < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &ev) < 0)
    throw std::runtime_error("epoll_ctl failed");
}
#endif

int EventLoop::add_fd(int fd, int mask, std::function<bool(int, int)> cb) {
  if (fds_.contains(fd)) throw std::runtime_error("event source already exists");
#if defined(__FreeBSD__)
  try {
    add_fd_filters(fd, 0, mask);
  } catch (...) {
    add_fd_filters(fd, mask, 0);
    throw;
  }
#else
  add_fd_filters(fd, -1, mask);
#endif
  fds_[fd] = std::make_shared<FdSource>(FdSource{fd, mask, std::move(cb)});
  return fd;
}

void EventLoop::update_fd(int fd, int mask) {
  auto it = fds_.find(fd);
  if (it == fds_.end() || it->second->mask == mask) return;
  add_fd_filters(fd, it->second->mask, mask);
  it->second->mask = mask;
}

int EventLoop::add_idle(std::function<void()> cb) {
  const int handle = next_handle_++;
  idles_.push_back({handle, std::move(cb)});
  // A blocked wait only exists outside dispatch; inside it the next wait polls.
  if (idles_.size() == 1) wake();
  return handle;
}

int EventLoop::add_timer(std::uint32_t ms, std::function<void()> cb) {
  const int handle = add_timer(std::move(cb));
  timers_.at(handle).one_shot = true;
  update_timer(handle, ms);
  return handle;
}

int EventLoop::add_timer(std::function<void()> cb) {
  const int handle = next_handle_++;
  timers_[handle] = Timer{std::make_shared<std::function<void()>>(std::move(cb)), 0, false};
  return handle;
}

void EventLoop::disarm(int handle, Timer& timer) {
  if (timer.deadline != 0) deadlines_.erase({timer.deadline, handle});
  timer.deadline = 0;
}

void EventLoop::update_timer(int handle, std::uint32_t ms) {
  const auto found = timers_.find(handle);
  if (found == timers_.end()) return;
  disarm(handle, found->second);
  if (ms == 0) return;
  found->second.deadline = now_ns() + static_cast<std::uint64_t>(ms) * 1000000ULL;
  // The kernel timer is re-armed once before the next wait, not per update.
  deadlines_.insert({found->second.deadline, handle});
}

void EventLoop::arm_timer() {
  const std::uint64_t next = deadlines_.empty() ? 0 : deadlines_.begin()->first;
  if (next == armed_deadline_) return;
#if defined(__FreeBSD__)
  if (next == 0) {
    kevent_change(poll_fd_, kTimerIdent, EVFILT_TIMER, EV_DELETE);
  } else {
    const std::uint64_t now = now_ns();
    const auto delay = static_cast<std::intptr_t>(next > now ? next - now : 1);
    if (!kevent_change(poll_fd_, kTimerIdent, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS,
                       delay))
      throw std::runtime_error("kevent timer failed");
  }
#else
  itimerspec spec{};
  spec.it_value.tv_sec = static_cast<std::time_t>(next / 1000000000ULL);
  spec.it_value.tv_nsec = static_cast<long>(next % 1000000000ULL);
  if (timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr) < 0)
    throw std::runtime_error("timerfd_settime failed");
#endif
  armed_deadline_ = next;
}

int EventLoop::add_signal(int signal_number, std::function<bool(int)> cb) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, signal_number);
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0)
    throw std::runtime_error("sigprocmask failed");
#if defined(__FreeBSD__)
  if (!kevent_change(poll_fd_, static_cast<std::uintptr_t>(signal_number), EVFILT_SIGNAL, EV_ADD))
    throw std::runtime_error("kevent signal failed");
  const int handle = next_handle_++;
  signals_[handle] = std::make_shared<FdSource>(FdSource{
      signal_number, 0, [mask, signal_number, cb = std::move(cb)](int, int) {
        const timespec zero{};
        while (sigtimedwait(&mask, nullptr, &zero) == signal_number)
          if (!cb(signal_number)) return false;
        return true;
      }});
  return handle;
#else
  const int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0) throw std::runtime_error("signalfd failed");
  try {
    add_fd(fd, EPOLLIN, [signal_number, cb = std::move(cb)](int source_fd, int) {
      signalfd_siginfo info{};
      while (read(source_fd, &info, sizeof(info)) == sizeof(info))
        if (!cb(signal_number)) return false;
      return true;
    });
  } catch (...) {
    close(fd);
    throw;
  }
  owned_fds_.insert(fd);
  return fd;
#endif
}

int EventLoop::add_prepare(std::function<void()> cb) {
  const int handle = next_handle_++;
  prepares_.emplace_back(handle, std::make_shared<std::function<void()>>(std::move(cb)));
  return handle;
}

void EventLoop::remove(int handle) {
  if (fds_.contains(handle)) {
    remove_fd_source(handle);
    return;
  }
  if (auto it = timers_.find(handle); it != timers_.end()) {
    disarm(handle, it->second);
    timers_.erase(it);
    return;
  }
  if (auto it = signals_.find(handle); it != signals_.end()) {
    const int signal_number = it->second->fd;
    signals_.erase(it);
#if defined(__FreeBSD__)
    const bool shared = std::any_of(signals_.begin(), signals_.end(), [&](const auto& entry) {
      return entry.second->fd == signal_number;
    });
    if (!shared)
      kevent_change(poll_fd_, static_cast<std::uintptr_t>(signal_number), EVFILT_SIGNAL, EV_DELETE);
#else
    (void)signal_number;
#endif
    return;
  }
  for (auto* list : {&idles_, &running_idles_})
    for (auto& idle : *list)
      if (idle.handle == handle) {
        idle.cb = nullptr;
        return;
      }
  std::erase_if(prepares_, [handle](const auto& entry) { return entry.first == handle; });
}

void EventLoop::remove_fd_source(int fd) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) return;
#if defined(__FreeBSD__)
  add_fd_filters(fd, it->second->mask, 0);
#else
  epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
  fds_.erase(it);
  if (owned_fds_.erase(fd) != 0) close(fd);
}

void EventLoop::run_idles() {
  // Idles queued by idles run on the next iteration, so this cannot spin forever.
  running_idles_.swap(idles_);
  for (std::size_t i = 0; i < running_idles_.size(); ++i) {
    auto cb = std::move(running_idles_[i].cb);
    running_idles_[i].cb = nullptr;
    if (cb) cb();
  }
  running_idles_.clear();
}

void EventLoop::run_expired_timers() {
#if !defined(__FreeBSD__)
  std::uint64_t expirations = 0;
  while (read(timer_fd_, &expirations, sizeof(expirations)) > 0) {
  }
#endif
  armed_deadline_ = 0;
  const std::uint64_t now = now_ns();
  while (!deadlines_.empty() && deadlines_.begin()->first <= now) {
    const int handle = deadlines_.begin()->second;
    deadlines_.erase(deadlines_.begin());
    auto it = timers_.find(handle);
    if (it == timers_.end()) continue;
    it->second.deadline = 0;
    const auto cb = it->second.cb;
    if (it->second.one_shot) timers_.erase(it);
    (*cb)();
  }
}

void EventLoop::dispatch_fd(int fd, int mask, const std::shared_ptr<FdSource>& source) {
  auto it = fds_.find(fd);
  if (it == fds_.end() || it->second != source) return;
  if (!source->cb(fd, mask)) {
    it = fds_.find(fd);
    if (it != fds_.end() && it->second == source) remove_fd_source(fd);
  }
}

bool EventLoop::dispatch(int timeout_ms) {
  if (!idles_.empty()) run_idles();
  for (std::size_t i = 0; i < prepares_.size(); ++i) {
    const auto cb = prepares_[i].second;
    (*cb)();
  }
  arm_timer();
  const int wait_ms = idles_.empty() ? timeout_ms : 0;

#if defined(__FreeBSD__)
  struct kevent events[kMaxEvents];
  timespec timeout{wait_ms / 1000, static_cast<long>(wait_ms % 1000) * 1000000L};
  const int n = kevent(poll_fd_, nullptr, 0, events, kMaxEvents, wait_ms < 0 ? nullptr : &timeout);
  if (n < 0) return errno == EINTR;

  struct Ready {
    int fd;
    int mask;
    std::shared_ptr<FdSource> source;
  };
  Ready ready[kMaxEvents];
  int ready_count = 0;
  std::vector<std::pair<int, std::shared_ptr<FdSource>>> signals;
  bool timer_fired = false;
  for (int i = 0; i < n; ++i) {
    const struct kevent& event = events[i];
    if (event.filter == EVFILT_USER) continue;
    if (event.filter == EVFILT_TIMER) {
      timer_fired = true;
      continue;
    }
    if (event.filter == EVFILT_SIGNAL) {
      for (const auto& [handle, source] : signals_)
        if (source->fd == static_cast<int>(event.ident)) signals.emplace_back(handle, source);
      continue;
    }
    const int fd = static_cast<int>(event.ident);
    int mask = event.filter == EVFILT_WRITE ? kEventWritable : kEventReadable;
    if ((event.flags & EV_EOF) != 0) mask |= kEventHangup;
    if ((event.flags & EV_ERROR) != 0) mask |= kEventError;
    Ready* entry = std::find_if(ready, ready + ready_count, [fd](const Ready& r) { return r.fd == fd; });
    if (entry == ready + ready_count) {
      const auto it = fds_.find(fd);
      if (it == fds_.end()) continue;
      ready[ready_count++] = {fd, 0, it->second};
    }
    entry->mask |= mask;
  }

  if (timer_fired) run_expired_timers();
  for (int i = 0; i < ready_count; ++i) dispatch_fd(ready[i].fd, ready[i].mask, ready[i].source);
  for (const auto& [handle, source] : signals) {
    const auto it = signals_.find(handle);
    if (it == signals_.end() || it->second != source) continue;
    if (!source->cb(source->fd, 0)) remove(handle);
  }
#else
  epoll_event events[kMaxEvents];
  const int n = epoll_wait(poll_fd_, events, kMaxEvents, wait_ms);
  if (n < 0) return errno == EINTR;

  // A callback may close an fd that another source reuses within this batch.
  std::shared_ptr<FdSource> ready[kMaxEvents];
  for (int i = 0; i < n; ++i)
    if (auto it = fds_.find(events[i].data.fd); it != fds_.end()) ready[i] = it->second;

  for (int i = 0; i < n; ++i) {
    const int fd = events[i].data.fd;
    if (fd == wake_fd_) {
      std::uint64_t v = 0;
      while (read(wake_fd_, &v, sizeof(v)) > 0) {
      }
      continue;
    }
    if (fd == timer_fd_) {
      run_expired_timers();
      continue;
    }
    if (ready[i]) dispatch_fd(fd, static_cast<int>(events[i].events), ready[i]);
  }
#endif
  return true;
}

void EventLoop::run() {
  running_.store(true);
  while (running_.load() && dispatch(-1)) {
  }
}

void EventLoop::stop() {
  running_.store(false);
  wake();
}

}  // namespace zwayland::server
