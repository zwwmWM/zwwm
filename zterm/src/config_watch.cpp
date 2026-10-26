#include "config_watch.hpp"

#include <sys/inotify.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

namespace zterm {

ConfigWatch::ConfigWatch() : fd_(inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
  if (fd_ < 0) std::fprintf(stderr,"zterm: config watch unavailable: %s\n",std::strerror(errno));
}

ConfigWatch::~ConfigWatch() { if (fd_ >= 0) close(fd_); }

void ConfigWatch::watch(const std::vector<std::string>& sources) {
  pending_ = false; files_.clear();
  if (fd_ < 0) return;
  std::set<std::filesystem::path> wanted;
  std::set<std::filesystem::path> seen;
  std::vector<std::string> targets = sources;
  const auto parent_watch = [&](const std::filesystem::path& file) {
    std::error_code error;
    auto parent = file.parent_path();
    while (!parent.empty() && !std::filesystem::is_directory(parent,error)) {
      const auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
    if (!parent.empty()) wanted.insert(parent);
  };
  for (size_t index = 0; index < targets.size() && index < 1024; ++index) {
    const auto source = targets[index];
    if (source.empty()) continue;
    std::error_code error;
    const auto file = std::filesystem::absolute(source,error).lexically_normal();
    if (error || !seen.insert(file).second) continue;
    files_.push_back(file);
    parent_watch(file);
    if (std::filesystem::is_symlink(file,error)) {
      auto target = std::filesystem::read_symlink(file,error);
      if (!error) targets.push_back((target.is_absolute() ? target : file.parent_path()/target).lexically_normal().string());
    }
    for (auto parent = file.parent_path(); !parent.empty();) {
      if (std::filesystem::is_symlink(parent,error)) { files_.push_back(parent); parent_watch(parent); }
      const auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
  }
  for (auto it = directories_.begin(); it != directories_.end();) {
    if (wanted.contains(it->second)) { wanted.erase(it->second); ++it; }
    else { (void)inotify_rm_watch(fd_,it->first); it = directories_.erase(it); }
  }
  for (const auto& directory : wanted) {
    const int id = inotify_add_watch(fd_,directory.c_str(),IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_MOVED_FROM |
      IN_CREATE | IN_DELETE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
    if (id >= 0) directories_[id] = directory;
  }
}

void ConfigWatch::consume() {
  alignas(inotify_event) std::array<char,16384> buffer{};
  bool changed = false, rearm = false;
  for (;;) {
    const auto size = read(fd_,buffer.data(),buffer.size());
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    for (size_t offset = 0; offset < static_cast<size_t>(size);) {
      const auto& event = *reinterpret_cast<const inotify_event*>(buffer.data()+offset);
      offset += sizeof(event)+event.len;
      if (event.mask & IN_Q_OVERFLOW) { changed = rearm = true; continue; }
      const auto found = directories_.find(event.wd);
      if (found == directories_.end()) continue;
      const auto path = event.len ? found->second / event.name : found->second;
      if (event.mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) { directories_.erase(found); rearm = true; }
      for (const auto& file : files_)
        if (file == path || file.string().starts_with(path.string()+"/")) { changed = true; rearm = true; break; }
    }
  }
  if (rearm) {
    std::vector<std::string> paths; for (const auto& file : files_) paths.push_back(file.string());
    watch(paths);
  }
  if (changed) { pending_ = true; deadline_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(100); }
}

bool ConfigWatch::ready() {
  if (!pending_ || std::chrono::steady_clock::now() < deadline_) return false;
  pending_ = false; return true;
}

int ConfigWatch::timeout() const {
  if (!pending_) return -1;
  return std::max(0,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-std::chrono::steady_clock::now()).count())+1);
}

}  // namespace zterm
