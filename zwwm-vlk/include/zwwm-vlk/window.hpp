#pragma once

#include "zwwm-vlk/image.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <zwayland/client/display.hpp>

namespace zwwm::vlk {

// Presents a render target on a parent Wayland surface through wl_shm copies.
class Window {
 public:
  [[nodiscard]] static std::unique_ptr<Window> create(std::shared_ptr<Device> device,
                                                      zwayland::client::Display& display,
                                                      zwayland::client::Proxy& surface,
                                                      zwayland::client::Proxy& shm,
                                                      std::uint32_t width, std::uint32_t height,
                                                      std::string* error = nullptr);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] const ImageRef& target() const { return target_; }
  [[nodiscard]] bool present();

  [[nodiscard]] const std::shared_ptr<Device>& device() const { return device_; }
  [[nodiscard]] std::span<const std::uint32_t> pixels() const;
  [[nodiscard]] const std::string& last_error() const { return last_error_; }

 private:
  struct Buffer;

  Window(std::shared_ptr<Device> device, zwayland::client::Display& display, zwayland::client::Proxy& surface,
         zwayland::client::Proxy& shm);

  [[nodiscard]] bool create_buffers(std::uint32_t width, std::uint32_t height);
  void release_buffers();
  bool fail(std::string message);

  std::shared_ptr<Device> device_;
  zwayland::client::Display& display_;
  zwayland::client::Proxy& surface_;
  zwayland::client::Proxy& shm_;
  ImageRef target_;
  std::vector<std::shared_ptr<Buffer>> buffers_;
  Buffer* presented_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::string last_error_;
};

}  // namespace zwwm::vlk
