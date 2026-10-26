#pragma once

#include <vulkan/vulkan.h>

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zwwm::vlk {

class Device;

struct DeviceOptions {
  std::optional<dev_t> drm_device;
  bool require_dmabuf = false;
};

struct DmabufModifier {
  std::uint64_t modifier = 0;
  std::uint32_t planes = 1;
  bool sampled = false;
  bool render = false;
  bool disjoint = false;
  VkExtent2D sampled_extent{};
  VkExtent2D render_extent{};
};

struct DmabufFormat {
  std::uint32_t drm_format = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool alpha = true;
  std::vector<DmabufModifier> modifiers;

  [[nodiscard]] const DmabufModifier* find(std::uint64_t modifier) const;
};

struct StagingBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapping = nullptr;
  VkDeviceSize size = 0;
};

// Resources passed to keep() live until the GPU retires the submission.
class Commands {
 public:
  Commands() = default;
  Commands(const Commands&) = delete;
  Commands& operator=(const Commands&) = delete;
  Commands(Commands&& other) noexcept;
  Commands& operator=(Commands&& other) noexcept;
  ~Commands();

  [[nodiscard]] VkCommandBuffer get() const { return buffer_; }
  [[nodiscard]] explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }
  void keep(std::shared_ptr<const void> resource);
  // Waits on a sync_file before `stages`; consumes `fd` even on failure.
  bool wait_sync_file(int fd, VkPipelineStageFlags2 stages);

 private:
  friend class Device;
  void abandon();

  Device* device_ = nullptr;
  VkCommandBuffer buffer_ = VK_NULL_HANDLE;
  std::vector<std::shared_ptr<const void>> keep_;
  std::vector<std::pair<VkSemaphore, VkPipelineStageFlags2>> waits_;
};

class Device : public std::enable_shared_from_this<Device> {
 public:
  [[nodiscard]] static std::shared_ptr<Device> create(const DeviceOptions& options, std::string* error);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  [[nodiscard]] VkInstance instance() const { return instance_; }
  [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
  [[nodiscard]] VkDevice handle() const { return device_; }
  [[nodiscard]] VkQueue queue() const { return queue_; }
  [[nodiscard]] std::uint32_t queue_family() const { return queue_family_; }
  [[nodiscard]] std::uint32_t foreign_queue_family() const { return foreign_queue_family_; }
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const VkPhysicalDeviceLimits& limits() const { return limits_; }
  [[nodiscard]] std::optional<dev_t> render_node() const { return render_node_; }

  [[nodiscard]] bool dmabuf_import() const { return dmabuf_import_; }
  [[nodiscard]] bool sync_file_import() const { return sync_file_import_; }
  [[nodiscard]] bool sync_file_export() const { return sync_file_export_; }
  [[nodiscard]] const std::vector<DmabufFormat>& dmabuf_formats() const { return dmabuf_formats_; }
  [[nodiscard]] const DmabufFormat* dmabuf_format(std::uint32_t drm_format) const;
  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>> sampled_dmabuf_formats() const;
  [[nodiscard]] std::vector<std::uint64_t> render_modifiers(std::uint32_t drm_format) const;

  [[nodiscard]] std::optional<std::uint32_t> memory_type(std::uint32_t allowed,
                                                         VkMemoryPropertyFlags required,
                                                         VkMemoryPropertyFlags preferred = 0) const;

  [[nodiscard]] Commands begin(std::string* error = nullptr);
  // `sync_file` receives a completion fence, or -1 after a blocking wait.
  [[nodiscard]] bool submit(Commands& commands, int* sync_file = nullptr, bool wait = false,
                            std::string* error = nullptr);
  void collect();
  void wait_idle();

  [[nodiscard]] std::shared_ptr<StagingBuffer> staging(VkDeviceSize size, std::string* error = nullptr);

  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
  PFN_vkGetSemaphoreFdKHR get_semaphore_fd = nullptr;
  PFN_vkImportSemaphoreFdKHR import_semaphore_fd = nullptr;

 private:
  friend class Commands;
  struct Pending {
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<const void>> keep;
    std::vector<VkSemaphore> semaphores;
  };

  Device() = default;
  [[nodiscard]] bool initialize(const DeviceOptions& options, std::string* error);
  void query_dmabuf_formats();
  [[nodiscard]] VkFence acquire_fence();
  void retire(Pending& pending);
  void destroy_staging(StagingBuffer* buffer);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  std::uint32_t queue_family_ = 0;
  std::uint32_t foreign_queue_family_ = VK_QUEUE_FAMILY_EXTERNAL;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkPhysicalDeviceLimits limits_{};
  std::string name_;
  std::optional<dev_t> render_node_;
  bool dmabuf_import_ = false;
  bool sync_file_import_ = false;
  bool sync_file_export_ = false;
  std::vector<DmabufFormat> dmabuf_formats_;
  std::vector<Pending> pending_;
  std::vector<VkFence> free_fences_;
  std::mutex staging_mutex_;
  std::vector<StagingBuffer*> free_staging_;
};

}  // namespace zwwm::vlk
