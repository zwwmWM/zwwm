#include "zwwm-vlk/device.hpp"

#include <libdrm/drm_fourcc.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace zwwm::vlk {
namespace {

struct FormatEntry {
  std::uint32_t drm;
  VkFormat format;
  bool alpha;
};

// X variants sample alpha as one through the view swizzle.
constexpr std::array<FormatEntry, 14> kFormats{{
    {DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, true},
    {DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, false},
    {DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM, true},
    {DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM, false},
    {DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, true},
    {DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, false},
    {DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true},
    {DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false},
    {DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, true},
    {DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, false},
    {DRM_FORMAT_ABGR16161616, VK_FORMAT_R16G16B16A16_UNORM, true},
    {DRM_FORMAT_XBGR16161616, VK_FORMAT_R16G16B16A16_UNORM, false},
    {DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, false},
    {DRM_FORMAT_BGR565, VK_FORMAT_B5G6R5_UNORM_PACK16, false},
}};

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string result_text(const char* operation, VkResult result) {
  return std::string(operation) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
  return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool env_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  // Compositor vertex stages write every varying; shaders may read a subset.
  if (data != nullptr && data->pMessage != nullptr && std::strstr(data->pMessage, "but there is no corresponding Input") != nullptr)
    return VK_FALSE;
  const char* level = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
  std::fprintf(stderr, "zwwm: vulkan %s: %s\n", level, data != nullptr ? data->pMessage : "");
  return VK_FALSE;
}

int device_rank(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
  }
}

}  // namespace

const DmabufModifier* DmabufFormat::find(std::uint64_t modifier) const {
  const auto found = std::find_if(modifiers.begin(), modifiers.end(),
                                  [modifier](const DmabufModifier& item) { return item.modifier == modifier; });
  return found == modifiers.end() ? nullptr : &*found;
}

Commands::Commands(Commands&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      keep_(std::move(other.keep_)),
      waits_(std::move(other.waits_)) {}

Commands& Commands::operator=(Commands&& other) noexcept {
  if (this != &other) {
    abandon();
    device_ = std::exchange(other.device_, nullptr);
    buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
    keep_ = std::move(other.keep_);
    waits_ = std::move(other.waits_);
  }
  return *this;
}

Commands::~Commands() { abandon(); }

void Commands::abandon() {
  if (device_ == nullptr) return;
  if (buffer_ != VK_NULL_HANDLE) vkFreeCommandBuffers(device_->device_, device_->pool_, 1, &buffer_);
  for (const auto& [semaphore, stages] : waits_) {
    (void)stages;
    vkDestroySemaphore(device_->device_, semaphore, nullptr);
  }
  buffer_ = VK_NULL_HANDLE;
  waits_.clear();
  keep_.clear();
  device_ = nullptr;
}

void Commands::keep(std::shared_ptr<const void> resource) {
  if (resource != nullptr) keep_.push_back(std::move(resource));
}

bool Commands::wait_sync_file(int fd, VkPipelineStageFlags2 stages) {
  if (fd < 0) return true;
  if (device_ == nullptr || !device_->sync_file_import_) {
    (void)close(fd);
    return false;
  }
  const VkSemaphoreCreateInfo create{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore semaphore = VK_NULL_HANDLE;
  if (vkCreateSemaphore(device_->device_, &create, nullptr, &semaphore) != VK_SUCCESS) {
    (void)close(fd);
    return false;
  }
  const VkImportSemaphoreFdInfoKHR import{
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = semaphore,
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = fd,
  };
  // A successful import transfers fd ownership to the driver.
  if (device_->import_semaphore_fd(device_->device_, &import) != VK_SUCCESS) {
    (void)close(fd);
    vkDestroySemaphore(device_->device_, semaphore, nullptr);
    return false;
  }
  waits_.emplace_back(semaphore, stages);
  return true;
}

std::shared_ptr<Device> Device::create(const DeviceOptions& options, std::string* error) {
  std::shared_ptr<Device> device(new Device());
  if (!device->initialize(options, error)) return nullptr;
  return device;
}

Device::~Device() {
  if (device_ != VK_NULL_HANDLE) {
    (void)vkDeviceWaitIdle(device_);
    for (auto& pending : pending_) retire(pending);
    pending_.clear();
    for (auto* buffer : free_staging_) destroy_staging(buffer);
    free_staging_.clear();
    for (auto fence : free_fences_) vkDestroyFence(device_, fence, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (messenger_ != VK_NULL_HANDLE) {
    const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool Device::initialize(const DeviceOptions& options, std::string* error) {
  std::uint32_t loader_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS || loader_version < VK_API_VERSION_1_3)
    return fail(error, "the Vulkan loader does not support Vulkan 1.3");

  std::uint32_t layer_count = 0;
  (void)vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
  std::vector<VkLayerProperties> layers(layer_count);
  (void)vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
  std::uint32_t instance_extension_count = 0;
  (void)vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
  std::vector<VkExtensionProperties> instance_extensions(instance_extension_count);
  (void)vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions.data());

  constexpr const char* kValidation = "VK_LAYER_KHRONOS_validation";
  std::vector<const char*> enabled_layers, enabled_instance_extensions;
  const bool debug = env_enabled("ZWWM_VULKAN_VALIDATION");
  if (debug) {
    if (std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& layer) {
          return std::strcmp(layer.layerName, kValidation) == 0;
        }))
      enabled_layers.push_back(kValidation);
    else
      std::fputs("zwwm: ZWWM_VULKAN_VALIDATION is set but the validation layer is not installed\n", stderr);
    if (has_extension(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
      enabled_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  const VkApplicationInfo application{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "zwwm",
      .applicationVersion = 1,
      .pEngineName = "zwwm-vlk",
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_3,
  };
  const VkInstanceCreateInfo instance_info{
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
      .enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size()),
      .ppEnabledLayerNames = enabled_layers.data(),
      .enabledExtensionCount = static_cast<std::uint32_t>(enabled_instance_extensions.size()),
      .ppEnabledExtensionNames = enabled_instance_extensions.data(),
  };
  if (const VkResult result = vkCreateInstance(&instance_info, nullptr, &instance_); result != VK_SUCCESS)
    return fail(error, result_text("vkCreateInstance", result));
  if (!enabled_instance_extensions.empty()) {
    const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    const VkDebugUtilsMessengerCreateInfoEXT messenger{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message,
    };
    if (create_messenger != nullptr) (void)create_messenger(instance_, &messenger, nullptr, &messenger_);
  }

  std::uint32_t physical_count = 0;
  (void)vkEnumeratePhysicalDevices(instance_, &physical_count, nullptr);
  std::vector<VkPhysicalDevice> physicals(physical_count);
  (void)vkEnumeratePhysicalDevices(instance_, &physical_count, physicals.data());
  if (physicals.empty()) return fail(error, "no Vulkan physical devices are available");

  const char* requested_name = std::getenv("ZWWM_VULKAN_DEVICE");
  int best_score = -1;
  std::vector<VkExtensionProperties> best_extensions;
  std::string rejection = "no Vulkan 1.3 device with dynamic rendering and synchronization2";
  for (const auto candidate : physicals) {
    VkPhysicalDeviceDrmPropertiesEXT drm{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    std::uint32_t extension_count = 0;
    (void)vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    (void)vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, extensions.data());
    const bool has_drm = has_extension(extensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    if (has_drm) properties.pNext = &drm;
    vkGetPhysicalDeviceProperties2(candidate, &properties);
    if (properties.properties.apiVersion < VK_API_VERSION_1_3) continue;

    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                .pNext = &features13};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(candidate, &features);
    if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE ||
        features13.shaderDemoteToHelperInvocation != VK_TRUE) continue;
    if (features12.runtimeDescriptorArray != VK_TRUE || features12.descriptorBindingPartiallyBound != VK_TRUE ||
        features12.descriptorBindingVariableDescriptorCount != VK_TRUE ||
        features.features.shaderSampledImageArrayDynamicIndexing != VK_TRUE) {
      rejection = "no Vulkan device supports runtime sampled-image descriptor arrays";
      continue;
    }

    const bool dmabuf = has_extension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                        has_extension(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                        has_extension(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (options.require_dmabuf && !dmabuf) {
      rejection = "no Vulkan device supports DMA-BUF import with DRM format modifiers";
      continue;
    }
    if (options.drm_device) {
      const auto major_number = static_cast<std::int64_t>(major(*options.drm_device));
      const auto minor_number = static_cast<std::int64_t>(minor(*options.drm_device));
      const bool matches = has_drm &&
          ((drm.hasPrimary && drm.primaryMajor == major_number && drm.primaryMinor == minor_number) ||
           (drm.hasRender && drm.renderMajor == major_number && drm.renderMinor == minor_number));
      if (!matches) {
        rejection = "no Vulkan device drives the requested DRM node";
        continue;
      }
    }
    int score = device_rank(properties.properties.deviceType) * 10;
    if (requested_name != nullptr && *requested_name != '\0' &&
        std::string_view(properties.properties.deviceName).find(requested_name) != std::string_view::npos)
      score += 100;
    if (dmabuf) score += 5;
    // Equal candidates keep the first enumerated driver (duplicate ICDs).
    if (score <= best_score) continue;
    best_score = score;
    physical_ = candidate;
    best_extensions = std::move(extensions);
    name_ = properties.properties.deviceName;
    limits_ = properties.properties.limits;
    render_node_.reset();
    if (has_drm && drm.hasRender)
      render_node_ = makedev(static_cast<unsigned>(drm.renderMajor), static_cast<unsigned>(drm.renderMinor));
    else if (has_drm && drm.hasPrimary)
      render_node_ = makedev(static_cast<unsigned>(drm.primaryMajor), static_cast<unsigned>(drm.primaryMinor));
  }
  if (physical_ == VK_NULL_HANDLE) return fail(error, rejection);
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  std::uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
  const auto graphics = std::find_if(families.begin(), families.end(), [](const VkQueueFamilyProperties& family) {
    return (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
  });
  if (graphics == families.end()) return fail(error, "the Vulkan device has no graphics queue");
  queue_family_ = static_cast<std::uint32_t>(graphics - families.begin());

  std::vector<const char*> device_extensions;
  const auto enable = [&](const char* name) {
    if (!has_extension(best_extensions, name)) return false;
    device_extensions.push_back(name);
    return true;
  };
  const bool memory_fd = enable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
  const bool dma_buf = enable(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
  const bool modifiers = enable(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
  if (enable(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) foreign_queue_family_ = VK_QUEUE_FAMILY_FOREIGN_EXT;
  const bool semaphore_fd = enable(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
  dmabuf_import_ = memory_fd && dma_buf && modifiers;

  constexpr float priority = 1.0F;
  const VkDeviceQueueCreateInfo queue_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family_,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };
  VkPhysicalDeviceVulkan13Features enabled13{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      // SPIR-V 1.6 lowers GLSL discard to OpDemoteToHelperInvocation.
      .shaderDemoteToHelperInvocation = VK_TRUE,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
  };
  VkPhysicalDeviceVulkan12Features enabled12{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &enabled13,
      .descriptorBindingPartiallyBound = VK_TRUE,
      .descriptorBindingVariableDescriptorCount = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
  };
  const VkPhysicalDeviceFeatures2 enabled{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &enabled12,
      .features = {.shaderSampledImageArrayDynamicIndexing = VK_TRUE},
  };
  const VkDeviceCreateInfo device_info{
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &enabled,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size()),
      .ppEnabledExtensionNames = device_extensions.data(),
  };
  if (const VkResult result = vkCreateDevice(physical_, &device_info, nullptr, &device_); result != VK_SUCCESS)
    return fail(error, result_text("vkCreateDevice", result));
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  if (memory_fd)
    get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
  if (semaphore_fd) {
    get_semaphore_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
    import_semaphore_fd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
    const VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties semaphore_properties{.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical_, &semaphore_info, &semaphore_properties);
    sync_file_import_ = import_semaphore_fd != nullptr &&
        (semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    sync_file_export_ = get_semaphore_fd != nullptr &&
        (semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
  }
  if (dmabuf_import_ && get_memory_fd_properties == nullptr) dmabuf_import_ = false;

  const VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = queue_family_,
  };
  if (const VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &pool_); result != VK_SUCCESS)
    return fail(error, result_text("vkCreateCommandPool", result));
  if (dmabuf_import_) query_dmabuf_formats();
  return true;
}

void Device::query_dmabuf_formats() {
  for (const auto& entry : kFormats) {
    VkDrmFormatModifierPropertiesListEXT list{.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
    vkGetPhysicalDeviceFormatProperties2(physical_, entry.format, &properties);
    if (list.drmFormatModifierCount == 0) continue;
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(physical_, entry.format, &properties);

    DmabufFormat format{.drm_format = entry.drm, .format = entry.format, .alpha = entry.alpha};
    for (const auto& item : modifiers) {
      const auto supported = [&](VkImageUsageFlags usage, VkExtent2D* extent) {
        const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = item.drmFormatModifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VkPhysicalDeviceExternalImageFormatInfo external_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &modifier_info,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        const VkPhysicalDeviceImageFormatInfo2 image_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external_info,
            .format = entry.format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
        };
        VkExternalImageFormatProperties external{.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 image{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external};
        if (vkGetPhysicalDeviceImageFormatProperties2(physical_, &image_info, &image) != VK_SUCCESS) return false;
        if ((external.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) return false;
        *extent = {image.imageFormatProperties.maxExtent.width, image.imageFormatProperties.maxExtent.height};
        return true;
      };
      DmabufModifier modifier{.modifier = item.drmFormatModifier, .planes = item.drmFormatModifierPlaneCount};
      const auto features = item.drmFormatModifierTilingFeatures;
      modifier.disjoint = (features & VK_FORMAT_FEATURE_DISJOINT_BIT) != 0;
      if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
        modifier.sampled = supported(VK_IMAGE_USAGE_SAMPLED_BIT, &modifier.sampled_extent);
      if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0)
        modifier.render = supported(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &modifier.render_extent);
      if (modifier.sampled || modifier.render) format.modifiers.push_back(modifier);
    }
    if (!format.modifiers.empty()) dmabuf_formats_.push_back(std::move(format));
  }
}

const DmabufFormat* Device::dmabuf_format(std::uint32_t drm_format) const {
  const auto found = std::find_if(dmabuf_formats_.begin(), dmabuf_formats_.end(),
                                  [drm_format](const DmabufFormat& format) { return format.drm_format == drm_format; });
  return found == dmabuf_formats_.end() ? nullptr : &*found;
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> Device::sampled_dmabuf_formats() const {
  std::vector<std::pair<std::uint32_t, std::uint64_t>> result;
  for (const auto& format : dmabuf_formats_)
    for (const auto& modifier : format.modifiers)
      if (modifier.sampled) result.emplace_back(format.drm_format, modifier.modifier);
  return result;
}

std::vector<std::uint64_t> Device::render_modifiers(std::uint32_t drm_format) const {
  std::vector<std::uint64_t> result;
  if (const auto* format = dmabuf_format(drm_format))
    for (const auto& modifier : format->modifiers)
      if (modifier.render) result.push_back(modifier.modifier);
  return result;
}

std::optional<std::uint32_t> Device::memory_type(std::uint32_t allowed, VkMemoryPropertyFlags required,
                                                 VkMemoryPropertyFlags preferred) const {
  std::optional<std::uint32_t> fallback;
  for (std::uint32_t index = 0; index < memory_.memoryTypeCount; ++index) {
    if ((allowed & (1U << index)) == 0) continue;
    const auto flags = memory_.memoryTypes[index].propertyFlags;
    if ((flags & required) != required) continue;
    if ((flags & preferred) == preferred) return index;
    if (!fallback) fallback = index;
  }
  return fallback;
}

VkFence Device::acquire_fence() {
  if (!free_fences_.empty()) {
    const VkFence fence = free_fences_.back();
    free_fences_.pop_back();
    if (vkResetFences(device_, 1, &fence) == VK_SUCCESS) return fence;
    vkDestroyFence(device_, fence, nullptr);
  }
  const VkFenceCreateInfo info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(device_, &info, nullptr, &fence) != VK_SUCCESS) return VK_NULL_HANDLE;
  return fence;
}

void Device::retire(Pending& pending) {
  if (pending.buffer != VK_NULL_HANDLE) vkFreeCommandBuffers(device_, pool_, 1, &pending.buffer);
  for (const auto semaphore : pending.semaphores) vkDestroySemaphore(device_, semaphore, nullptr);
  if (pending.fence != VK_NULL_HANDLE) free_fences_.push_back(pending.fence);
  pending.buffer = VK_NULL_HANDLE;
  pending.fence = VK_NULL_HANDLE;
  pending.semaphores.clear();
  auto keep = std::move(pending.keep);
  keep.clear();
}

void Device::collect() {
  // Retired keep-alives may hold the last external references to this device.
  const auto self = weak_from_this().lock();
  std::vector<Pending> finished;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (vkGetFenceStatus(device_, it->fence) == VK_SUCCESS) {
      finished.push_back(std::move(*it));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& pending : finished) retire(pending);
}

void Device::wait_idle() {
  (void)vkQueueWaitIdle(queue_);
  collect();
}

Commands Device::begin(std::string* error) {
  collect();
  Commands commands;
  const VkCommandBufferAllocateInfo allocate{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool_,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  if (const VkResult result = vkAllocateCommandBuffers(device_, &allocate, &commands.buffer_); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkAllocateCommandBuffers", result));
    commands.buffer_ = VK_NULL_HANDLE;
    return commands;
  }
  commands.device_ = this;
  const VkCommandBufferBeginInfo begin{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (const VkResult result = vkBeginCommandBuffer(commands.buffer_, &begin); result != VK_SUCCESS) {
    (void)fail(error, result_text("vkBeginCommandBuffer", result));
    commands.abandon();
  }
  return commands;
}

bool Device::submit(Commands& commands, int* sync_file, bool wait, std::string* error) {
  if (sync_file != nullptr) *sync_file = -1;
  if (!commands || commands.device_ != this) return fail(error, "invalid Vulkan command buffer");
  if (const VkResult result = vkEndCommandBuffer(commands.buffer_); result != VK_SUCCESS) {
    commands.abandon();
    return fail(error, result_text("vkEndCommandBuffer", result));
  }
  Pending pending;
  pending.fence = acquire_fence();
  if (pending.fence == VK_NULL_HANDLE) {
    commands.abandon();
    return fail(error, "could not allocate a Vulkan fence");
  }
  VkSemaphore signal = VK_NULL_HANDLE;
  if (sync_file != nullptr && sync_file_export_) {
    const VkExportSemaphoreCreateInfo export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export_info};
    if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &signal) != VK_SUCCESS) signal = VK_NULL_HANDLE;
  }

  std::vector<VkSemaphoreSubmitInfo> waits;
  waits.reserve(commands.waits_.size());
  for (const auto& [semaphore, stages] : commands.waits_) {
    waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = semaphore, .stageMask = stages});
    pending.semaphores.push_back(semaphore);
  }
  commands.waits_.clear();
  const VkCommandBufferSubmitInfo buffer_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = commands.buffer_,
  };
  const VkSemaphoreSubmitInfo signal_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = signal,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkSubmitInfo2 submit{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
      .pWaitSemaphoreInfos = waits.data(),
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &buffer_info,
      .signalSemaphoreInfoCount = signal == VK_NULL_HANDLE ? 0U : 1U,
      .pSignalSemaphoreInfos = &signal_info,
  };
  pending.buffer = std::exchange(commands.buffer_, VK_NULL_HANDLE);
  pending.keep = std::move(commands.keep_);
  commands.device_ = nullptr;
  if (signal != VK_NULL_HANDLE) pending.semaphores.push_back(signal);
  if (const VkResult result = vkQueueSubmit2(queue_, 1, &submit, pending.fence); result != VK_SUCCESS) {
    retire(pending);
    return fail(error, result_text("vkQueueSubmit2", result));
  }
  if (signal != VK_NULL_HANDLE) {
    const VkSemaphoreGetFdInfoKHR get{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = signal,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int fd = -1;
    if (get_semaphore_fd(device_, &get, &fd) == VK_SUCCESS) *sync_file = fd;
  }
  const VkFence fence = pending.fence;
  pending_.push_back(std::move(pending));
  if (wait || (sync_file != nullptr && *sync_file < 0)) {
    if (const VkResult result = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX); result != VK_SUCCESS)
      return fail(error, result_text("vkWaitForFences", result));
    collect();
  }
  return true;
}

std::shared_ptr<StagingBuffer> Device::staging(VkDeviceSize size, std::string* error) {
  StagingBuffer* buffer = nullptr;
  {
    std::lock_guard lock(staging_mutex_);
    auto best = free_staging_.end();
    for (auto it = free_staging_.begin(); it != free_staging_.end(); ++it)
      if ((*it)->size >= size && (*it)->size <= size * 4 + 65536 &&
          (best == free_staging_.end() || (*it)->size < (*best)->size)) best = it;
    if (best != free_staging_.end()) {
      buffer = *best;
      free_staging_.erase(best);
    }
  }
  if (buffer == nullptr) {
    auto created = std::make_unique<StagingBuffer>();
    created->size = std::max<VkDeviceSize>(size, 4096);
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = created->size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(device_, &info, nullptr, &created->buffer) != VK_SUCCESS) {
      (void)fail(error, "could not create a Vulkan staging buffer");
      return nullptr;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, created->buffer, &requirements);
    const auto type = memory_type(requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type.value_or(0),
    };
    if (!type || vkAllocateMemory(device_, &allocate, nullptr, &created->memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, created->buffer, created->memory, 0) != VK_SUCCESS ||
        vkMapMemory(device_, created->memory, 0, VK_WHOLE_SIZE, 0, &created->mapping) != VK_SUCCESS) {
      destroy_staging(created.release());
      (void)fail(error, "could not allocate host-visible Vulkan staging memory");
      return nullptr;
    }
    buffer = created.release();
  }
  // Submissions never outlive the device that retires them.
  return std::shared_ptr<StagingBuffer>(buffer, [this](StagingBuffer* released) {
    std::lock_guard lock(staging_mutex_);
    if (free_staging_.size() < 8) free_staging_.push_back(released);
    else destroy_staging(released);
  });
}

void Device::destroy_staging(StagingBuffer* buffer) {
  if (buffer == nullptr) return;
  if (buffer->mapping != nullptr) vkUnmapMemory(device_, buffer->memory);
  if (buffer->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer->buffer, nullptr);
  if (buffer->memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer->memory, nullptr);
  delete buffer;
}

}  // namespace zwwm::vlk
