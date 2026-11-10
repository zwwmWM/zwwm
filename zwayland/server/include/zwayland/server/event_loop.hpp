#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zwayland::server {

inline constexpr int kEventReadable = 0x001;
inline constexpr int kEventWritable = 0x004;
inline constexpr int kEventError = 0x008;
inline constexpr int kEventHangup = 0x010;

// fd sources use the fd as handle; other sources a disjoint range.
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // fd source: `cb` returns false to have the source removed. Returns the fd.
  int add_fd(int fd, int mask, std::function<bool(int fd, int mask)> cb);
  void update_fd(int fd, int mask);

  // Runs `cb` once on the next iteration, in FIFO order. Wakes a blocked loop.
  int add_idle(std::function<void()> cb);

  // One-shot timer firing `ms` from now. 0 leaves it disarmed.
  int add_timer(std::uint32_t ms, std::function<void()> cb);
  // Persistent, initially disarmed timer re-armed with update_timer().
  int add_timer(std::function<void()> cb);
  // 0 disarms, matching timerfd semantics.
  void update_timer(int handle, std::uint32_t ms);

  int add_signal(int signal_number, std::function<bool(int)> cb);

  // Runs right before the loop sleeps, after idles; used to batch flushes.
  int add_prepare(std::function<void()> cb);

  // Removes a source by its handle.
  void remove(int handle);

  // One iteration; timeout_ms -1 blocks. False when the loop failed.
  bool dispatch(int timeout_ms);
  void run();
  void stop();

 private:
  struct FdSource {
    int fd;
    int mask;
    std::function<bool(int, int)> cb;
  };
  struct Timer {
    std::shared_ptr<std::function<void()>> cb;
    std::uint64_t deadline = 0;  // CLOCK_MONOTONIC ns; 0 == disarmed
    bool one_shot = false;
  };
  struct Idle {
    int handle;
    std::function<void()> cb;
  };

  void wake();
  void remove_fd_source(int fd);
  void disarm(int handle, Timer& timer);
  void arm_timer();
  void add_fd_filters(int fd, int old_mask, int new_mask);
  void dispatch_fd(int fd, int mask, const std::shared_ptr<FdSource>& source);
  void run_idles();
  void run_expired_timers();

  int poll_fd_ = -1;
  int wake_fd_ = -1;
  int timer_fd_ = -1;
  std::atomic<bool> running_{false};

  std::unordered_map<int, std::shared_ptr<FdSource>> fds_;
  std::unordered_set<int> owned_fds_;
  std::unordered_map<int, std::shared_ptr<FdSource>> signals_;
  std::unordered_map<int, Timer> timers_;
  std::set<std::pair<std::uint64_t, int>> deadlines_;
  std::uint64_t armed_deadline_ = 0;
  std::vector<Idle> idles_;
  std::vector<Idle> running_idles_;
  std::vector<std::pair<int, std::shared_ptr<std::function<void()>>>> prepares_;

  int next_handle_ = 0x10000000;
};

}  // namespace zwayland::server
