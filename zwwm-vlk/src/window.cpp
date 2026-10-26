#include "zwwm-vlk/window.hpp"

#include <wayland-zwayland-client.h>
#include <zwayland/wire/message.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <limits>
#include <utility>

namespace zwwm::vlk {
namespace protocol = zwayland::generated;

namespace {

int create_anonymous_file() {
#ifdef SYS_memfd_create
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "zwwm-vlk", 0x0001U));
  if (fd >= 0) return fd;
#endif
  char path[] = "/tmp/zwwm-vlk-XXXXXX";
  const int fallback_fd = mkostemp(path, O_CLOEXEC);
  if (fallback_fd < 0) return -1;
  (void)unlink(path);
  return fallback_fd;
}

}  // namespace

struct Window::Buffer {
  zwayland::client::Display* display = nullptr;
  zwayland::client::Proxy* proxy = nullptr;
  zwayland::wire::FileDescriptor fd;
  void* mapping = nullptr;
  std::size_t size = 0;
  bool busy = false;
  bool retired = false;

  ~Buffer() {
    if (mapping != nullptr) (void)munmap(mapping, size);
  }

  struct Observer {
    std::shared_ptr<Buffer> buffer;
    void release(zwayland::client::Proxy&) const {
      buffer->busy = false;
      if (!buffer->retired || buffer->proxy == nullptr) return;
      const std::uint32_t id = buffer->proxy->id;
      buffer->proxy = nullptr;
      protocol::wl_buffer_destroy(*buffer->display, id);
    }
  };
};

Window::Window(std::shared_ptr<Device> device, zwayland::client::Display& display,
               zwayland::client::Proxy& surface, zwayland::client::Proxy& shm)
    : device_(std::move(device)), display_(display), surface_(surface), shm_(shm) {}

Window::~Window() {
  release_buffers();
  target_.reset();
  if (device_ != nullptr) device_->wait_idle();
}

std::unique_ptr<Window> Window::create(std::shared_ptr<Device> device, zwayland::client::Display& display,
                                       zwayland::client::Proxy& surface, zwayland::client::Proxy& shm,
                                       std::uint32_t width, std::uint32_t height, std::string* error) {
  if (device == nullptr) {
    if (error != nullptr) *error = "Vulkan device is unavailable";
    return nullptr;
  }
  auto window = std::unique_ptr<Window>(new Window(std::move(device), display, surface, shm));
  if (!window->resize(width, height)) {
    if (error != nullptr) *error = window->last_error();
    return nullptr;
  }
  return window;
}

bool Window::resize(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return fail("window dimensions must be positive");
  if (width == width_ && height == height_ && target_ != nullptr) return true;
  release_buffers();
  std::string error;
  target_ = Image::create(device_, width, height, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &error);
  if (target_ == nullptr) return fail("could not create the nested render target: " + error);
  if (!create_buffers(width, height)) return false;
  width_ = width;
  height_ = height;
  return true;
}

bool Window::create_buffers(std::uint32_t width, std::uint32_t height) {
  constexpr std::size_t count = 3;
  const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * 4U;
  const std::uint64_t size64 = stride64 * height;
  if (stride64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
      size64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return fail("window buffer exceeds wl_shm limits");
  const auto stride = static_cast<std::int32_t>(stride64);
  const auto size = static_cast<std::size_t>(size64);

  buffers_.clear();
  buffers_.reserve(count);
  for (std::size_t index = 0; index != count; ++index) {
    auto buffer = std::make_shared<Buffer>();
    buffer->display = &display_;
    buffer->size = size;
    buffer->fd = zwayland::wire::FileDescriptor(create_anonymous_file());
    if (buffer->fd.get() < 0 || ftruncate(buffer->fd.get(), static_cast<off_t>(size)) < 0)
      return fail("could not allocate wl_shm presentation buffer");
    buffer->mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd.get(), 0);
    if (buffer->mapping == MAP_FAILED) {
      buffer->mapping = nullptr;
      return fail("could not map wl_shm presentation buffer");
    }
    const std::uint32_t pool_id = protocol::wl_shm_create_pool(
        display_, shm_.id, buffer->fd.get(), static_cast<std::int32_t>(size));
    auto* pool = display_.find_proxy(pool_id);
    if (pool == nullptr) return fail("could not create wl_shm_pool proxy");
    const std::uint32_t buffer_id = protocol::wl_shm_pool_create_buffer(
        display_, pool->id, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), stride,
        protocol::WL_SHM_FORMAT_XRGB8888);
    buffer->proxy = display_.find_proxy(buffer_id);
    protocol::wl_shm_pool_destroy(display_, pool->id);
    if (buffer->proxy == nullptr) return fail("could not create wl_buffer proxy");
    buffer->proxy->set_observer(protocol::wl_buffer_observer(Buffer::Observer{buffer}));
    buffers_.push_back(std::move(buffer));
  }
  if (!display_.flush()) return fail("could not flush wl_shm buffer creation");
  return true;
}

bool Window::present() {
  Buffer* buffer = nullptr;
  for (const auto& candidate : buffers_) {
    if (!candidate->busy) {
      buffer = candidate.get();
      break;
    }
  }
  if (buffer == nullptr) return fail("all presentation buffers are busy");
  std::string error;
  const std::size_t pixel_count = static_cast<std::size_t>(width_) * height_;
  auto* pixels = static_cast<std::uint32_t*>(buffer->mapping);
  if (!target_->read({pixels, pixel_count}, &error)) return fail("could not read the nested frame: " + error);

  protocol::wl_surface_attach(display_, surface_.id, buffer->proxy, 0, 0);
  if (surface_.version >= 4)
    protocol::wl_surface_damage_buffer(display_, surface_.id, 0, 0, std::numeric_limits<std::int32_t>::max(),
                                       std::numeric_limits<std::int32_t>::max());
  else
    protocol::wl_surface_damage(display_, surface_.id, 0, 0, std::numeric_limits<std::int32_t>::max(),
                                std::numeric_limits<std::int32_t>::max());
  protocol::wl_surface_commit(display_, surface_.id);
  buffer->busy = true;
  presented_ = buffer;
  if (!display_.flush()) return fail("could not flush presented buffer");
  last_error_.clear();
  return true;
}

std::span<const std::uint32_t> Window::pixels() const {
  if (presented_ == nullptr || presented_->mapping == nullptr) return {};
  return {static_cast<const std::uint32_t*>(presented_->mapping), static_cast<std::size_t>(width_) * height_};
}

void Window::release_buffers() {
  presented_ = nullptr;
  for (const auto& buffer : buffers_) {
    if (buffer->proxy == nullptr) continue;
    if (buffer->busy) {
      buffer->retired = true;
      continue;
    }
    const std::uint32_t id = buffer->proxy->id;
    buffer->proxy = nullptr;
    protocol::wl_buffer_destroy(display_, id);
  }
  buffers_.clear();
}

bool Window::fail(std::string message) {
  last_error_ = std::move(message);
  return false;
}

}  // namespace zwwm::vlk
