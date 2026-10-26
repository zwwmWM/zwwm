#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace zterm {

class ConfigWatch {
 public:
  ConfigWatch();
  ~ConfigWatch();
  ConfigWatch(const ConfigWatch&) = delete;
  ConfigWatch& operator=(const ConfigWatch&) = delete;
  void watch(const std::vector<std::string>& sources);
  void consume();
  bool ready();
  int timeout() const;
  int fd() const { return fd_; }
 private:
  int fd_ = -1;
  std::vector<std::filesystem::path> files_;
  std::map<int,std::filesystem::path> directories_;
  bool pending_ = false;
  std::chrono::steady_clock::time_point deadline_{};
};

}  // namespace zterm
