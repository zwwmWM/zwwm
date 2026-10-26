#include "zwwm-vlk/image.hpp"

#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace zwwm::vlk {
namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string result_text(const char* operation, VkResult result) {
  return std::string(operation) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
}

bool same_file(int left, int right) {
  if (left == right) return true;
  struct stat a {}, b {};
  return fstat(left, &a) == 0 && fstat(right, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

std::uint32_t bytes_per_pixel(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      return 1;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return 4;
    default:
      return 0;
  }
}

void warn_sync_file_once(const char* operation) {
  static std::atomic_bool warned{false};
  if (!warned.exchange(true))
    std::fprintf(stderr, "zwwm: DMA-BUF %s is unavailable (%s); relying on driver implicit sync\n",
                 operation, std::strerror(errno));
}

constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};

}  // namespace

std::shared_ptr<Image> Image::create(std::shared_ptr<Device> device, std::uint32_t width, std::uint32_t height,
                                     VkFormat format, VkImageUsageFlags usage, std::string* error) {
  return allocate(std::move(device), width, height, 1, false, format, usage, error);
}

std::shared_ptr<Image> Image::create_array(std::shared_ptr<Device> device, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t layers, VkFormat format,
                                           VkImageUsageFlags usage, std::string* error) {
  return allocate(std::move(device), width, height, layers, true, format, usage, error);
}

std::shared_ptr<Image> Image::allocate(std::shared_ptr<Device> device, std::uint32_t width, std::uint32_t height,
                                       std::uint32_t layers, bool array, VkFormat format,
                                       VkImageUsageFlags usage, std::string* error) {
  if (device == nullptr || width == 0 || height == 0 || layers == 0) {
    (void)fail(error, "Vulkan images need a device and positive dimensions");
    return nullptr;
  }
  const auto limit = device->limits().maxImageDimension2D;
  if (width > limit || height > limit || layers > device->limits().maxImageArrayLayers) {
    (void)fail(error, "image exceeds the GPU texture size limit");
    return nullptr;
  }
  std::shared_ptr<Image> image(new Image(std::move(device)));
  image->format_ = format;
  image->width_ = width;
  image->height_ = height;
  image->layers_ = layers;
  image->array_ = array;
  image->usage_ = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  const VkDevice handle = image->device_->handle();
  const VkImageCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1},
      .mipLevels = 1,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = image->usage_,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (const VkResult result = vkCreateImage(handle, &info, nullptr, &image->image_); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkCreateImage", result));
    return nullptr;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(handle, image->image_, &requirements);
  const auto type = image->device_->memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  const auto any_type = type ? type : image->device_->memory_type(requirements.memoryTypeBits, 0);
  if (!any_type) {
    (void)fail(error, "no Vulkan memory type can hold the image");
    return nullptr;
  }
  const VkMemoryAllocateInfo allocate_info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = *any_type,
  };
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (const VkResult result = vkAllocateMemory(handle, &allocate_info, nullptr, &memory); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkAllocateMemory", result));
    return nullptr;
  }
  image->memory_.push_back(memory);
  if (const VkResult result = vkBindImageMemory(handle, image->image_, memory, 0); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkBindImageMemory", result));
    return nullptr;
  }
  if (!image->create_view(false, error)) return nullptr;
  return image;
}

std::shared_ptr<Image> Image::import_dmabuf(std::shared_ptr<Device> device, const DmabufAttributes& attributes,
                                            VkImageUsageFlags usage, std::string* error) {
  if (device == nullptr || !device->dmabuf_import()) {
    (void)fail(error, "the Vulkan device cannot import DMA-BUFs");
    return nullptr;
  }
  std::string validation;
  if (!attributes.valid(&validation)) {
    (void)fail(error, validation);
    return nullptr;
  }
  const DmabufFormat* format = device->dmabuf_format(attributes.format);
  if (format == nullptr) {
    char name[5]{};
    std::memcpy(name, &attributes.format, 4);
    (void)fail(error, std::string("DRM format ") + name + " has no Vulkan import path");
    return nullptr;
  }
  // Implicit-modifier buffers are linear on every driver zwwm can import from.
  std::uint64_t modifier = attributes.planes.front().modifier;
  if (modifier == DRM_FORMAT_MOD_INVALID) modifier = DRM_FORMAT_MOD_LINEAR;
  const DmabufModifier* layout = format->find(modifier);
  const bool wants_sample = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
  const bool wants_render = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
  if (layout == nullptr || (wants_sample && !layout->sampled) || (wants_render && !layout->render)) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "modifier 0x%016llx", static_cast<unsigned long long>(modifier));
    (void)fail(error, std::string(text) + " is not importable for this use");
    return nullptr;
  }
  const VkExtent2D limit = wants_render ? layout->render_extent : layout->sampled_extent;
  if (attributes.width > limit.width || attributes.height > limit.height) {
    (void)fail(error, "DMA-BUF exceeds the importable image size");
    return nullptr;
  }
  if (attributes.planes.size() < layout->planes) {
    (void)fail(error, "DMA-BUF provides fewer planes than its modifier requires");
    return nullptr;
  }
  const std::uint32_t plane_count = layout->planes;
  bool disjoint = false;
  for (std::uint32_t plane = 1; plane < plane_count; ++plane)
    if (!same_file(attributes.planes[0].fd, attributes.planes[plane].fd)) disjoint = true;
  if (disjoint && !layout->disjoint) {
    (void)fail(error, "DMA-BUF planes live in separate buffers but the modifier is not disjoint-capable");
    return nullptr;
  }

  std::shared_ptr<Image> image(new Image(std::move(device)));
  image->format_ = format->format;
  image->width_ = attributes.width;
  image->height_ = attributes.height;
  image->usage_ = usage;
  image->external_ = true;
  const VkDevice handle = image->device_->handle();

  std::vector<VkSubresourceLayout> planes(plane_count);
  for (std::uint32_t plane = 0; plane < plane_count; ++plane)
    planes[plane] = {.offset = attributes.planes[plane].offset, .rowPitch = attributes.planes[plane].stride};
  const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = plane_count,
      .pPlaneLayouts = planes.data(),
  };
  const VkExternalMemoryImageCreateInfo external{
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_modifier,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  const VkImageCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .flags = disjoint ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_DISJOINT_BIT) : 0U,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format->format,
      .extent = {attributes.width, attributes.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (const VkResult result = vkCreateImage(handle, &info, nullptr, &image->image_); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkCreateImage(DMA-BUF)", result));
    return nullptr;
  }

  const std::uint32_t memory_count = disjoint ? plane_count : 1U;
  std::vector<VkBindImageMemoryInfo> binds(memory_count);
  std::vector<VkBindImagePlaneMemoryInfo> plane_binds(memory_count);
  for (std::uint32_t index = 0; index < memory_count; ++index) {
    const int fd = attributes.planes[index].fd;
    VkMemoryFdPropertiesKHR fd_properties{.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (const VkResult result = image->device_->get_memory_fd_properties(
            handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_properties);
        result != VK_SUCCESS) {
      (void)fail(error, result_text("vkGetMemoryFdPropertiesKHR", result));
      return nullptr;
    }
    const auto aspect = static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << index);
    const VkImagePlaneMemoryRequirementsInfo plane_requirements{
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
        .planeAspect = aspect,
    };
    const VkImageMemoryRequirementsInfo2 requirements_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = disjoint ? &plane_requirements : nullptr,
        .image = image->image_,
    };
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetImageMemoryRequirements2(handle, &requirements_info, &requirements);
    const auto type = image->device_->memory_type(
        requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits, 0);
    if (!type) {
      (void)fail(error, "no Vulkan memory type can import the DMA-BUF");
      return nullptr;
    }
    const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0) {
      (void)fail(error, std::string("could not duplicate DMA-BUF fd: ") + std::strerror(errno));
      return nullptr;
    }
    const VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image->image_,
    };
    const VkImportMemoryFdInfoKHR import{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = disjoint ? nullptr : &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = duplicated,
    };
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = *type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (const VkResult result = vkAllocateMemory(handle, &allocate, nullptr, &memory); result != VK_SUCCESS) {
      (void)close(duplicated);
      (void)fail(error, result_text("vkAllocateMemory(DMA-BUF)", result));
      return nullptr;
    }
    image->memory_.push_back(memory);
    plane_binds[index] = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO, .planeAspect = aspect};
    binds[index] = {
        .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .pNext = disjoint ? &plane_binds[index] : nullptr,
        .image = image->image_,
        .memory = memory,
    };
  }
  if (const VkResult result = vkBindImageMemory2(handle, memory_count, binds.data()); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkBindImageMemory2(DMA-BUF)", result));
    return nullptr;
  }
  for (std::uint32_t plane = 0; plane < plane_count; ++plane) {
    const int fd = attributes.planes[plane].fd;
    const bool known = std::any_of(image->dmabuf_fds_.begin(), image->dmabuf_fds_.end(),
                                   [fd](int existing) { return same_file(existing, fd); });
    if (known) continue;
    const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated >= 0) image->dmabuf_fds_.push_back(duplicated);
  }
  // Color attachments require the identity swizzle.
  if (!image->create_view(!format->alpha && !wants_render, error)) return nullptr;
  return image;
}

Image::~Image() {
  const VkDevice handle = device_ != nullptr ? device_->handle() : VK_NULL_HANDLE;
  if (handle != VK_NULL_HANDLE) {
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(handle, view_, nullptr);
    if (image_ != VK_NULL_HANDLE) vkDestroyImage(handle, image_, nullptr);
    for (const auto memory : memory_) vkFreeMemory(handle, memory, nullptr);
  }
  for (const int fd : dmabuf_fds_) (void)close(fd);
}

bool Image::create_view(bool opaque, std::string* error) {
  const VkImageViewCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image_,
      .viewType = array_ ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
      .format = format_,
      .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     opaque ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY},
      .subresourceRange = kColorRange,
  };
  if (const VkResult result = vkCreateImageView(device_->handle(), &info, nullptr, &view_); result != VK_SUCCESS)
    return fail(error, result_text("vkCreateImageView", result));
  return true;
}

void Image::transition(VkCommandBuffer commands, VkImageLayout layout, bool discard) {
  VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
      .oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : layout_,
      .newLayout = layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image_,
      .subresourceRange = kColorRange,
  };
  if (external_ && !owned_) {
    // Foreign producers leave their contents in GENERAL.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = device_->foreign_queue_family();
    barrier.dstQueueFamilyIndex = device_->queue_family();
    owned_ = true;
  }
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(commands, &dependency);
  layout_ = layout;
  transitioned_ = true;
}

void Image::release(VkCommandBuffer commands) {
  if (!external_ || !owned_) return;
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
      .dstAccessMask = VK_ACCESS_2_NONE,
      .oldLayout = layout_,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = device_->queue_family(),
      .dstQueueFamilyIndex = device_->foreign_queue_family(),
      .image = image_,
      .subresourceRange = kColorRange,
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(commands, &dependency);
  layout_ = VK_IMAGE_LAYOUT_GENERAL;
  owned_ = false;
}

bool Image::upload(std::span<const Region> regions, std::string* error) {
  const std::uint32_t texel = bytes_per_pixel(format_);
  if (texel == 0 || external_) return fail(error, "image does not accept uploads");
  VkDeviceSize total = 0;
  bool whole = false;
  for (const auto& region : regions) {
    if (region.width == 0 || region.height == 0) continue;
    if (region.data == nullptr || region.x > width_ || region.y > height_ || region.width > width_ - region.x ||
        region.height > height_ - region.y || region.layer >= layers_ || region.row_bytes < region.width * texel)
      return fail(error, "upload rectangle exceeds the image");
    total = (total + 3U) / 4U * 4U + static_cast<VkDeviceSize>(region.width) * texel * region.height;
    whole = whole || (layers_ == 1 && region.x == 0 && region.y == 0 && region.width == width_ &&
                      region.height == height_);
  }
  if (total == 0) return true;
  auto staging = device_->staging(total, error);
  if (staging == nullptr) return false;
  std::vector<VkBufferImageCopy> copies;
  copies.reserve(regions.size());
  auto* destination = static_cast<std::byte*>(staging->mapping);
  VkDeviceSize offset = 0;
  for (const auto& region : regions) {
    if (region.width == 0 || region.height == 0) continue;
    // Buffer offsets must be multiples of the texel size and of 4.
    offset = (offset + 3U) / 4U * 4U;
    const VkDeviceSize row_bytes = static_cast<VkDeviceSize>(region.width) * texel;
    const auto* source = static_cast<const std::byte*>(region.data);
    for (std::uint32_t row = 0; row < region.height; ++row)
      std::memcpy(destination + offset + row * row_bytes, source + static_cast<std::size_t>(row) * region.row_bytes,
                  row_bytes);
    copies.push_back({
        .bufferOffset = offset,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, region.layer, 1},
        .imageOffset = {static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y), 0},
        .imageExtent = {region.width, region.height, 1},
    });
    offset += row_bytes * region.height;
  }
  Commands commands = device_->begin(error);
  if (!commands) return false;
  transition(commands.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, whole || layout_ == VK_IMAGE_LAYOUT_UNDEFINED);
  vkCmdCopyBufferToImage(commands.get(), staging->buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         static_cast<std::uint32_t>(copies.size()), copies.data());
  transition(commands.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  commands.keep(std::move(staging));
  commands.keep(shared_from_this());
  return device_->submit(commands, nullptr, false, error);
}

bool Image::upload(const std::uint32_t* pixels, std::uint32_t row_pixels, std::uint32_t x, std::uint32_t y,
                   std::uint32_t width, std::uint32_t height, std::string* error) {
  if (pixels == nullptr || width == 0 || height == 0) return true;
  if (bytes_per_pixel(format_) != 4) return fail(error, "image does not accept 32-bit uploads");
  const Region region{pixels, row_pixels * 4U, x, y, width, height, 0};
  return upload(std::span<const Region>(&region, 1), error);
}

bool Image::read(std::span<std::uint32_t> pixels, std::string* error) {
  if (bytes_per_pixel(format_) != 4) return fail(error, "image is not a 32-bit format");
  const std::size_t count = static_cast<std::size_t>(width_) * height_;
  if (pixels.size() < count) return fail(error, "readback destination is too small");
  auto staging = device_->staging(count * 4U, error);
  if (staging == nullptr) return false;
  Commands commands = device_->begin(error);
  if (!commands) return false;
  const VkImageLayout previous = layout_ == VK_IMAGE_LAYOUT_UNDEFINED || layout_ == VK_IMAGE_LAYOUT_GENERAL
                                     ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : layout_;
  transition(commands.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkBufferImageCopy region{
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {width_, height_, 1},
  };
  vkCmdCopyImageToBuffer(commands.get(), image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1, &region);
  if (external_) release(commands.get());
  else transition(commands.get(), previous);
  const VkBufferMemoryBarrier2 host{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = staging->buffer,
      .size = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers = &host,
  };
  vkCmdPipelineBarrier2(commands.get(), &dependency);
  commands.keep(staging);
  commands.keep(shared_from_this());
  if (!device_->submit(commands, nullptr, true, error)) return false;
  std::memcpy(pixels.data(), staging->mapping, count * 4U);
  return true;
}

int Image::export_sync_file(bool write) const {
  int merged = -1;
  for (const int fd : dmabuf_fds_) {
    dma_buf_export_sync_file request{.flags = static_cast<__u32>(write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ), .fd = -1};
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0) {
      warn_sync_file_once("sync_file export");
      if (merged >= 0) (void)close(merged);
      return -1;
    }
    if (merged < 0) {
      merged = request.fd;
      continue;
    }
    sync_merge_data merge{};
    std::strncpy(merge.name, "zwwm", sizeof(merge.name) - 1);
    merge.fd2 = request.fd;
    const int result = ioctl(merged, SYNC_IOC_MERGE, &merge);
    (void)close(request.fd);
    (void)close(merged);
    if (result != 0) return -1;
    merged = merge.fence;
  }
  return merged;
}

bool Image::import_sync_file(int fd, bool write) const {
  if (fd < 0 || dmabuf_fds_.empty()) return false;
  bool imported = true;
  for (const int dmabuf : dmabuf_fds_) {
    dma_buf_import_sync_file request{.flags = static_cast<__u32>(write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ), .fd = fd};
    if (ioctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request) != 0) {
      warn_sync_file_once("sync_file import");
      imported = false;
    }
  }
  return imported;
}

}  // namespace zwwm::vlk
