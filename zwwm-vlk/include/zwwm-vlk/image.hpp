#pragma once

#include "zwwm-vlk/device.hpp"
#include "zwwm-vlk/dmabuf.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zwwm::vlk {

class Image : public std::enable_shared_from_this<Image> {
 public:
  [[nodiscard]] static std::shared_ptr<Image> create(std::shared_ptr<Device> device, std::uint32_t width,
                                                     std::uint32_t height, VkFormat format,
                                                     VkImageUsageFlags usage, std::string* error = nullptr);
  [[nodiscard]] static std::shared_ptr<Image> create_array(std::shared_ptr<Device> device, std::uint32_t width,
                                                           std::uint32_t height, std::uint32_t layers,
                                                           VkFormat format, VkImageUsageFlags usage,
                                                           std::string* error = nullptr);
  // Duplicates the borrowed plane FDs; the caller keeps its own.
  [[nodiscard]] static std::shared_ptr<Image> import_dmabuf(std::shared_ptr<Device> device,
                                                            const DmabufAttributes& attributes,
                                                            VkImageUsageFlags usage,
                                                            std::string* error = nullptr);
  ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  [[nodiscard]] const std::shared_ptr<Device>& device() const { return device_; }
  [[nodiscard]] VkImage handle() const { return image_; }
  [[nodiscard]] VkImageView view() const { return view_; }
  [[nodiscard]] VkFormat format() const { return format_; }
  [[nodiscard]] std::uint32_t width() const { return width_; }
  [[nodiscard]] std::uint32_t height() const { return height_; }
  [[nodiscard]] std::uint32_t layers() const { return layers_; }
  [[nodiscard]] VkImageUsageFlags usage() const { return usage_; }
  [[nodiscard]] bool external() const { return external_; }
  [[nodiscard]] VkImageLayout layout() const { return layout_; }

  // External images are acquired from the foreign queue family first.
  void transition(VkCommandBuffer commands, VkImageLayout layout, bool discard = false);
  void release(VkCommandBuffer commands);
  [[nodiscard]] bool acquired() const { return !external_ || owned_; }

  struct Region {
    const void* data = nullptr;
    std::uint32_t row_bytes = 0;
    std::uint32_t x = 0, y = 0, width = 0, height = 0, layer = 0;
  };
  [[nodiscard]] bool upload(std::span<const Region> regions, std::string* error = nullptr);
  [[nodiscard]] bool upload(const std::uint32_t* pixels, std::uint32_t row_pixels, std::uint32_t x,
                            std::uint32_t y, std::uint32_t width, std::uint32_t height,
                            std::string* error = nullptr);
  [[nodiscard]] bool read(std::span<std::uint32_t> pixels, std::string* error = nullptr);

  // DMA-BUF implicit sync; export returns -1 without kernel support.
  [[nodiscard]] int export_sync_file(bool write) const;
  bool import_sync_file(int fd, bool write) const;
  [[nodiscard]] bool has_dmabuf() const { return !dmabuf_fds_.empty(); }

 private:
  explicit Image(std::shared_ptr<Device> device) : device_(std::move(device)) {}
  [[nodiscard]] static std::shared_ptr<Image> allocate(std::shared_ptr<Device> device, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t layers, bool array,
                                                       VkFormat format, VkImageUsageFlags usage, std::string* error);
  [[nodiscard]] bool create_view(bool opaque, std::string* error);

  std::shared_ptr<Device> device_;
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> memory_;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage_ = 0;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t layers_ = 1;
  bool array_ = false;
  VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  bool external_ = false;
  bool owned_ = false;
  bool transitioned_ = false;
  std::vector<int> dmabuf_fds_;
};

using ImageRef = std::shared_ptr<Image>;

}  // namespace zwwm::vlk
