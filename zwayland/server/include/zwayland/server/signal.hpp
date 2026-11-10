#pragma once

#include <cstddef>
#include <algorithm>
#include <vector>
#include <utility>

namespace zwayland::server {

// Modeled on wl_listener.
struct Listener {
  void (*notify)(Listener* self, void* data) = nullptr;
};

// Listeners must outlive the signal; removals during emit leave tombstones.
class Signal {
 public:
  void add(Listener& l) {
    if (std::find(listeners_.begin(), listeners_.end(), &l) == listeners_.end())
      listeners_.push_back(&l);
  }

  void remove(Listener& l) {
    const auto it = std::find(listeners_.begin(), listeners_.end(), &l);
    if (it == listeners_.end()) return;
    if (emitting_ > 0) {
      *it = nullptr;
      tombstones_ = true;
    } else {
      listeners_.erase(it);
    }
  }

  // Listeners added during emission are not notified until the next emit.
  void emit(void* data) {
    const std::size_t end = listeners_.size();
    ++emitting_;
    for (std::size_t i = 0; i < end && i < listeners_.size(); ++i) {
      Listener* l = listeners_[i];
      if (l != nullptr && l->notify != nullptr) l->notify(l, data);
    }
    if (--emitting_ == 0 && tombstones_) {
      std::erase(listeners_, nullptr);
      tombstones_ = false;
    }
  }

  void emit_final(void* data) {
    auto snapshot = std::move(listeners_);
    listeners_.clear();
    tombstones_ = false;
    for (Listener* listener : snapshot) {
      if (listener != nullptr && listener->notify != nullptr)
        listener->notify(listener, data);
    }
  }

 private:
  std::vector<Listener*> listeners_;
  std::size_t emitting_ = 0;
  bool tombstones_ = false;
};

}  // namespace zwayland::server
