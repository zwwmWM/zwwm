#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace zwayland::wire {

inline constexpr std::uint32_t kServerIdStart = 0xff000000U;

// Dense array plus sparse spill so a hostile id cannot force a huge allocation.
template <typename T>
class ObjectMap {
 public:
  [[nodiscard]] T* find(std::uint32_t id) const {
    const auto& dense = side(id);
    const std::uint32_t index = slot(id);
    if (index < dense.size() && dense[index]) return dense[index].get();
    if (sparse_.empty()) return nullptr;
    const auto it = sparse_.find(id);
    return it == sparse_.end() ? nullptr : it->second.get();
  }

  [[nodiscard]] bool contains(std::uint32_t id) const { return find(id) != nullptr; }

  bool insert(std::uint32_t id, std::unique_ptr<T> object) {
    if (id == 0 || !object || contains(id)) return false;
    auto& dense = side(id);
    const std::uint32_t index = slot(id);
    if (index < dense.size()) {
      dense[index] = std::move(object);
    } else if (index - dense.size() <= kDenseSlack) {
      dense.resize(static_cast<std::size_t>(index) + 1);
      dense[index] = std::move(object);
    } else {
      sparse_.emplace(id, std::move(object));
    }
    ++count_;
    return true;
  }

  std::unique_ptr<T> take(std::uint32_t id) {
    auto& dense = side(id);
    const std::uint32_t index = slot(id);
    std::unique_ptr<T> result;
    if (index < dense.size() && dense[index]) {
      result = std::move(dense[index]);
      while (!dense.empty() && !dense.back()) dense.pop_back();
    } else if (auto it = sparse_.find(id); it != sparse_.end()) {
      result = std::move(it->second);
      sparse_.erase(it);
    }
    if (result) --count_;
    return result;
  }

  // Live ids in ascending order; a snapshot, so callers may mutate the map.
  [[nodiscard]] std::vector<std::uint32_t> ids() const {
    std::vector<std::uint32_t> result;
    result.reserve(count_);
    for (std::size_t i = 0; i < client_.size(); ++i)
      if (client_[i]) result.push_back(static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < server_.size(); ++i)
      if (server_[i]) result.push_back(kServerIdStart + static_cast<std::uint32_t>(i));
    for (const auto& [id, object] : sparse_) result.push_back(id);
    std::sort(result.begin(), result.end());
    return result;
  }

  [[nodiscard]] std::size_t size() const { return count_; }
  [[nodiscard]] std::size_t dense_size(bool server_side) const {
    return server_side ? server_.size() : client_.size();
  }

 private:
  static constexpr std::uint32_t kDenseSlack = 1024;
  [[nodiscard]] static std::uint32_t slot(std::uint32_t id) {
    return id < kServerIdStart ? id : id - kServerIdStart;
  }
  [[nodiscard]] std::vector<std::unique_ptr<T>>& side(std::uint32_t id) {
    return id < kServerIdStart ? client_ : server_;
  }
  [[nodiscard]] const std::vector<std::unique_ptr<T>>& side(std::uint32_t id) const {
    return id < kServerIdStart ? client_ : server_;
  }

  std::vector<std::unique_ptr<T>> client_;
  std::vector<std::unique_ptr<T>> server_;
  std::unordered_map<std::uint32_t, std::unique_ptr<T>> sparse_;
  std::size_t count_ = 0;
};

}  // namespace zwayland::wire
