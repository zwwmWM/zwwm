#include "zwwm/renderer/vulkan.hpp"
#include "shader_compiler.hpp"
#include "shaders.hpp"

#include <zwwm-vlk/device.hpp>
#include <zwwm-vlk/image.hpp>

#include <png.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#ifndef ZWWM_DATA_DIR
#define ZWWM_DATA_DIR "/usr/share/zwwm"
#endif

namespace zwwm::renderer {
namespace {

namespace gl = detail::gl;

constexpr VkFormat kTextureFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkDeviceSize kUniformChunk = 1U << 20U;
constexpr std::uint32_t kPoolSets = 256;
constexpr std::uint32_t kMaximumTextures = 4096;

constexpr std::uint32_t kHasTexture = 1U;
constexpr std::uint32_t kHasTextureRect = 2U;
constexpr std::uint32_t kBackgroundOnly = 4U;
constexpr std::uint32_t kHasClip = 8U;
constexpr std::uint32_t kStrictClip = 16U;

constexpr int kKawaseDownsample = 0;
constexpr int kKawaseUpsample = 1;
constexpr int kKawaseFinalize = 2;

// std140 mirror of the ZwwmDraw block in zwwm.glsl.
struct DrawData {
  std::array<float, 4> draw_rect{};
  std::array<float, 4> toplevel_rect{};
  std::array<float, 4> texture_rect{};
  std::array<float, 4> clip_rect{};
  std::array<float, 4> source_uv{0.0F, 0.0F, 1.0F, 1.0F};
  std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 4> input_to_clip_0{1.0F, 0.0F, 0.0F, 0.0F};
  std::array<float, 4> input_to_clip_1{0.0F, 1.0F, 0.0F, 0.0F};
  std::array<float, 4> glass_tint{};
  std::array<float, 2> output_size{};
  float opacity = 1.0F;
  float time = 0.0F;
  float corner_radius = 0.0F;
  float border_width = 0.0F;
  float clip_radius = 0.0F;
  float texture_corner_radius = 0.0F;
  float mask_threshold = 0.0F;
  std::uint32_t flags = 0;
  std::int32_t texture_transform = 0;
  std::uint32_t state = 0;
  std::int32_t window_texture = 0;
  std::int32_t backdrop_texture = 0;
  std::int32_t blurred_backdrop_texture = 0;
  std::int32_t background_texture = 0;
  float glass_refraction = 0.0F;
  float glass_dispersion = 0.0F;
  float glass_frost = 0.0F;
  float glass_ior = 1.5F;
  float glass_thickness = 12.0F;
  float glass_roughness = 0.18F;
  std::array<float, 2> texel_size{};
  float sample_offset = 0.0F;
  float brightness = 1.0F;
  float contrast = 1.0F;
  float saturation = 1.0F;
  float noise = 0.0F;
  float reserved = 0.0F;
};
static_assert(offsetof(DrawData, output_size) == 144);
static_assert(offsetof(DrawData, window_texture) == 192);
static_assert(offsetof(DrawData, texel_size) == 232);
static_assert(sizeof(DrawData) == 264);
// std140 rounds the block to its 16-byte base alignment.
constexpr VkDeviceSize kDrawSize = 272;

std::array<float, 4> rect_of(const FloatRect& rect) {
  return {static_cast<float>(rect.origin.x), static_cast<float>(rect.origin.y), static_cast<float>(rect.size.width),
          static_cast<float>(rect.size.height)};
}

std::string default_wallpaper_path() {
  for (const auto& path : {
           std::filesystem::path{"/etc/xdg/zwwm/background/background.png"},
           std::filesystem::path{ZWWM_DATA_DIR} / "background/background.png",
           std::filesystem::path{ZWWM_SOURCE_DATA_DIR} / "background.png",
           std::filesystem::path{"zwwm/data/background.png"}}) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path.string();
  }
  return "/etc/xdg/zwwm/background/background.png";
}

struct RenderDraw : DrawCall {
  FloatRect bounds, toplevel_bounds;
  std::optional<FloatRect> clip_bounds, texture_bounds;
  explicit RenderDraw(const DrawCall& draw) : DrawCall(draw), bounds(draw.bounds),
      toplevel_bounds(draw.toplevel_bounds) {
    if (draw.clip_bounds) clip_bounds = FloatRect(*draw.clip_bounds);
    if (draw.texture_bounds) texture_bounds = FloatRect(*draw.texture_bounds);
    if (draw.geometry) {
      bounds = draw.geometry->bounds;
      toplevel_bounds = draw.geometry->toplevel;
      if (draw.geometry->clip) clip_bounds = draw.geometry->clip;
      if (draw.geometry->texture) texture_bounds = draw.geometry->texture;
    }
  }
};

float rounded_rectangle_distance(float x, float y, float width, float height, float radius) {
  const float effective_radius = std::clamp(radius, 0.0F, 0.5F * std::min(width, height));
  const float half_width = width * 0.5F;
  const float half_height = height * 0.5F;
  const float corner_x = std::abs(x - half_width) - half_width + effective_radius;
  const float corner_y = std::abs(y - half_height) - half_height + effective_radius;
  return std::hypot(std::max(corner_x, 0.0F), std::max(corner_y, 0.0F)) +
         std::min(std::max(corner_x, corner_y), 0.0F) - effective_radius;
}

std::uint32_t float_components(std::uint32_t type) {
  switch (type) {
    case gl::kFloat: return 1;
    case gl::kFloatVec2: return 2;
    case gl::kFloatVec3: return 3;
    case gl::kFloatVec4: return 4;
    default: return 0;
  }
}

std::uint32_t int_components(std::uint32_t type) {
  switch (type) {
    case gl::kInt: return 1;
    case gl::kIntVec2: return 2;
    case gl::kIntVec3: return 3;
    case gl::kIntVec4: return 4;
    default: return 0;
  }
}

bool compatible_value(const detail::ConfigMember& member, const ShaderValue& value) {
  const auto type = member.type;
  if (std::holds_alternative<bool>(value)) return type == gl::kBool;
  if (std::holds_alternative<std::int32_t>(value))
    return type == gl::kInt || type == gl::kUnsignedInt || type == gl::kFloat;
  if (std::holds_alternative<std::array<float, 4>>(value)) return type == gl::kFloatVec4;
  if (const auto* colors = std::get_if<std::vector<std::array<float, 4>>>(&value))
    return type == gl::kFloatVec4 && member.array_size >= colors->size();
  const auto* components = std::get_if<std::vector<std::int32_t>>(&value);
  if (components == nullptr) return false;
  const auto count = static_cast<std::uint32_t>(components->size());
  return count >= 2 && count <= 4 && (float_components(type) == count || int_components(type) == count);
}

bool pipeline_shader_value(ShaderRole role, std::string_view name) {
  if (role == ShaderRole::border) return name == "width" || name == "radius";
  if (role == ShaderRole::shadow) return name == "size" || name == "offset_x" || name == "offset_y";
  if (role == ShaderRole::window)
    return name == "blur_radius" || name == "blur_passes" || name == "blur_brightness" ||
           name == "blur_contrast" || name == "blur_saturation" || name == "blur_noise";
  return false;
}

bool encode_config(const detail::ConfigBlock& block, const std::vector<std::pair<std::string, ShaderValue>>& values,
                   ShaderRole role, std::vector<std::byte>* bytes, std::string* error) {
  bytes->assign(block.size, std::byte{0});
  const auto find = [&](std::string_view name) -> const detail::ConfigMember* {
    const auto found = std::find_if(block.members.begin(), block.members.end(),
                                    [name](const auto& member) { return member.name == name; });
    return found == block.members.end() ? nullptr : &*found;
  };
  const auto put = [&](const detail::ConfigMember& member, std::uint32_t element, const void* data, std::size_t size) {
    const std::size_t offset = member.offset + static_cast<std::size_t>(element) * member.array_stride;
    if (offset + size <= bytes->size()) std::memcpy(bytes->data() + offset, data, size);
  };
  for (const auto& [name, value] : values) {
    const auto* member = find(name);
    if (member == nullptr && pipeline_shader_value(role, name)) continue;
    if (member == nullptr || !compatible_value(*member, value)) {
      if (error != nullptr) *error = "config block has no compatible member '" + name + "'";
      return false;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
      const std::uint32_t truth = *boolean ? 1U : 0U;
      put(*member, 0, &truth, 4);
    } else if (const auto* integer = std::get_if<std::int32_t>(&value)) {
      if (member->type == gl::kFloat) {
        const auto converted = static_cast<float>(*integer);
        put(*member, 0, &converted, 4);
      } else {
        put(*member, 0, integer, 4);
      }
    } else if (const auto* color = std::get_if<std::array<float, 4>>(&value)) {
      put(*member, 0, color->data(), 16);
    } else if (const auto* components = std::get_if<std::vector<std::int32_t>>(&value)) {
      if (float_components(member->type) != 0) {
        std::array<float, 4> converted{};
        for (std::size_t index = 0; index < components->size(); ++index)
          converted[index] = static_cast<float>((*components)[index]);
        put(*member, 0, converted.data(), components->size() * 4);
      } else {
        put(*member, 0, components->data(), components->size() * 4);
      }
    } else if (const auto* colors = std::get_if<std::vector<std::array<float, 4>>>(&value)) {
      for (std::size_t index = 0; index < colors->size(); ++index)
        put(*member, static_cast<std::uint32_t>(index), (*colors)[index].data(), 16);
      if (const auto* count = find(name + "_count"); count != nullptr && int_components(count->type) == 1) {
        const auto size = static_cast<std::int32_t>(colors->size());
        put(*count, 0, &size, 4);
      }
    }
  }
  return true;
}

bool load_png(const std::string& path, std::vector<std::uint32_t>* pixels, std::uint32_t* width,
              std::uint32_t* height, std::uint32_t maximum, std::string& error) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_file(&image, path.c_str()) == 0) {
    error = "could not read background image " + path + ": " + image.message;
    return false;
  }
  image.format = PNG_FORMAT_BGRA;
  if (image.width == 0 || image.height == 0 || image.width > maximum || image.height > maximum) {
    png_image_free(&image);
    error = "background image exceeds the GPU texture size limit";
    return false;
  }
  pixels->resize(static_cast<std::size_t>(image.width) * image.height);
  if (png_image_finish_read(&image, nullptr, pixels->data(), 0, nullptr) == 0) {
    error = "could not decode background image " + path + ": " + image.message;
    png_image_free(&image);
    return false;
  }
  *width = image.width;
  *height = image.height;
  png_image_free(&image);
  return true;
}

struct Program {
  VkDevice device = VK_NULL_HANDLE;
  VkShaderModule vertex = VK_NULL_HANDLE;
  VkShaderModule fragment = VK_NULL_HANDLE;
  ShaderRole role = ShaderRole::builtin;
  VkBuffer config_buffer = VK_NULL_HANDLE;
  VkDeviceMemory config_memory = VK_NULL_HANDLE;
  VkDescriptorPool config_pool = VK_NULL_HANDLE;
  VkDescriptorSet config_set = VK_NULL_HANDLE;
  std::map<std::tuple<VkFormat, bool, int>, VkPipeline> pipelines;

  Program() = default;
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  ~Program() {
    if (device == VK_NULL_HANDLE) return;
    for (const auto& [key, pipeline] : pipelines) {
      (void)key;
      vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (config_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, config_pool, nullptr);
    if (config_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, config_buffer, nullptr);
    if (config_memory != VK_NULL_HANDLE) vkFreeMemory(device, config_memory, nullptr);
    if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragment, nullptr);
    if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertex, nullptr);
  }
};
using ProgramRef = std::shared_ptr<Program>;

// Recycled once the renderer holds the only reference again.
struct Arena {
  struct Chunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::byte* mapping = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize used = 0;
  };
  vlk::Device* device = nullptr;
  std::vector<VkDescriptorPool> pools;
  std::size_t pool = 0;
  std::vector<Chunk> chunks;
  std::size_t chunk = 0;

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() {
    const VkDevice handle = device->handle();
    for (const auto item : pools) vkDestroyDescriptorPool(handle, item, nullptr);
    for (const auto& item : chunks) {
      if (item.mapping != nullptr) vkUnmapMemory(handle, item.memory);
      vkDestroyBuffer(handle, item.buffer, nullptr);
      vkFreeMemory(handle, item.memory, nullptr);
    }
  }

  void reset() {
    for (const auto item : pools) (void)vkResetDescriptorPool(device->handle(), item, 0);
    for (auto& item : chunks) item.used = 0;
    pool = chunk = 0;
  }

  VkDescriptorSet allocate(VkDescriptorSetLayout layout, std::uint32_t textures) {
    for (;; ++pool) {
      if (pool == pools.size()) {
        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kPoolSets},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(kPoolSets * 64U, textures)},
        }};
        const VkDescriptorPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kPoolSets,
            .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data(),
        };
        VkDescriptorPool created = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device->handle(), &info, nullptr, &created) != VK_SUCCESS) return VK_NULL_HANDLE;
        pools.push_back(created);
      }
      const VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
          .descriptorSetCount = 1,
          .pDescriptorCounts = &textures,
      };
      const VkDescriptorSetAllocateInfo info{
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
          .pNext = &variable,
          .descriptorPool = pools[pool],
          .descriptorSetCount = 1,
          .pSetLayouts = &layout,
      };
      VkDescriptorSet set = VK_NULL_HANDLE;
      const VkResult result = vkAllocateDescriptorSets(device->handle(), &info, &set);
      if (result == VK_SUCCESS) return set;
      if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
    }
  }

  std::optional<std::pair<VkBuffer, VkDeviceSize>> write(const void* data, VkDeviceSize size, VkDeviceSize padded) {
    const VkDeviceSize alignment = std::max<VkDeviceSize>(device->limits().minUniformBufferOffsetAlignment, 16);
    for (;; ++chunk) {
      if (chunk == chunks.size()) {
        Chunk created;
        created.size = std::max(kUniformChunk, padded);
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = created.size,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(device->handle(), &info, nullptr, &created.buffer) != VK_SUCCESS) return std::nullopt;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device->handle(), created.buffer, &requirements);
        const auto type = device->memory_type(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = type.value_or(0),
        };
        void* mapping = nullptr;
        if (!type || vkAllocateMemory(device->handle(), &allocate, nullptr, &created.memory) != VK_SUCCESS ||
            vkBindBufferMemory(device->handle(), created.buffer, created.memory, 0) != VK_SUCCESS ||
            vkMapMemory(device->handle(), created.memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
          if (created.memory != VK_NULL_HANDLE) vkFreeMemory(device->handle(), created.memory, nullptr);
          vkDestroyBuffer(device->handle(), created.buffer, nullptr);
          return std::nullopt;
        }
        created.mapping = static_cast<std::byte*>(mapping);
        chunks.push_back(created);
      }
      auto& current = chunks[chunk];
      const VkDeviceSize offset = (current.used + alignment - 1) / alignment * alignment;
      if (offset + padded > current.size) continue;
      std::memcpy(current.mapping + offset, data, size);
      std::memset(current.mapping + offset + size, 0, padded - size);
      current.used = offset + padded;
      return std::pair{current.buffer, offset};
    }
  }
};

struct Batch {
  vlk::Commands commands;
  std::shared_ptr<Arena> arena;
  std::vector<TextureRef> externals;
  std::vector<TextureRef> reads, writes;
  std::vector<const Program*> kept_programs;
  // Entry 0 is the opaque black placeholder.
  std::vector<TextureRef> textures;
  std::unordered_map<const Texture*, std::int32_t> texture_indices;
  VkDescriptorSet frame_set = VK_NULL_HANDLE;
  VkBuffer frame_set_buffer = VK_NULL_HANDLE;
  std::size_t frame_set_textures = 0;
  const Program* bound_config = nullptr;
  TextureRef target;
  bool rendering = false;
  std::optional<std::array<float, 4>> clear;
  VkPipeline bound = VK_NULL_HANDLE;
};

struct Snapshots {
  Size size;
  std::uint64_t used = 0;
  TextureRef background, reflection, overview_scene;
  std::vector<TextureRef> kawase;
};

}  // namespace

struct VulkanRenderer::State {
  std::shared_ptr<vlk::Device> device;
  VkSampler sampler = VK_NULL_HANDLE;
  VkPipelineCache cache = VK_NULL_HANDLE;
  VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout config_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::uint32_t maximum_textures = kMaximumTextures;
  std::vector<std::uint32_t> vertex_spirv;
  ProgramRef surface, glass, kawase;
  TextureRef dummy;

  struct CustomProgram {
    ProgramRef program;
    ShaderRole role = ShaderRole::window;
  };
  std::unordered_map<std::string, CustomProgram> custom_programs, pending_custom_programs;
  std::string window_shader, border_shader, background_shader, shadow_shader;
  std::string pending_window_shader, pending_border_shader, pending_background_shader, pending_shadow_shader;
  TextureRef wallpaper, pending_wallpaper;
  TextureRef overview_backdrop, pending_overview_backdrop;
  bool pending = false;
  std::unordered_map<TextureHandle, TextureRef> textures;
  std::vector<std::shared_ptr<Arena>> arenas;
  std::vector<Snapshots> snapshots;
  std::uint64_t frame_counter = 0;
  std::unordered_map<VkFormat, bool> blit_support;

  std::optional<Batch> batch;
  std::optional<VkRect2D> scissor;
  bool warned_layout = false;

  ~State() {
    if (device == nullptr) return;
    device->wait_idle();
    batch.reset();
    custom_programs.clear();
    pending_custom_programs.clear();
    surface.reset();
    glass.reset();
    kawase.reset();
    textures.clear();
    snapshots.clear();
    arenas.clear();
    wallpaper.reset();
    pending_wallpaper.reset();
    overview_backdrop.reset();
    pending_overview_backdrop.reset();
    dummy.reset();
    device->wait_idle();
    const VkDevice handle = device->handle();
    if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(handle, layout, nullptr);
    if (config_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(handle, config_layout, nullptr);
    if (frame_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(handle, frame_layout, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(handle, sampler, nullptr);
    if (cache != VK_NULL_HANDLE) vkDestroyPipelineCache(handle, cache, nullptr);
  }

  bool create_layouts(std::string& error) {
    const VkDevice handle = device->handle();
    const auto& limits = device->limits();
    maximum_textures = std::min({kMaximumTextures, limits.maxPerStageDescriptorSamplers,
                                 limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSamplers,
                                 limits.maxDescriptorSetSampledImages});
    const std::array<VkDescriptorSetLayoutBinding, 2> frame_bindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maximum_textures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const std::array<VkDescriptorBindingFlags, 2> frame_flags{
        0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(frame_flags.size()),
        .pBindingFlags = frame_flags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo frame_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .bindingCount = static_cast<std::uint32_t>(frame_bindings.size()),
        .pBindings = frame_bindings.data(),
    };
    const VkDescriptorSetLayoutBinding config_binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo config_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &config_binding,
    };
    if (vkCreateDescriptorSetLayout(handle, &frame_info, nullptr, &frame_layout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(handle, &config_info, nullptr, &config_layout) != VK_SUCCESS) {
      error = "could not create Vulkan descriptor set layouts";
      return false;
    }
    const std::array<VkDescriptorSetLayout, 2> sets{frame_layout, config_layout};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
        .pSetLayouts = sets.data(),
    };
    if (vkCreatePipelineLayout(handle, &layout_info, nullptr, &layout) != VK_SUCCESS) {
      error = "could not create the Vulkan pipeline layout";
      return false;
    }
    return true;
  }

  [[nodiscard]] ProgramRef make_program(std::string_view source, ShaderRole role,
                                        const std::vector<std::pair<std::string, ShaderValue>>& values,
                                        std::string& error) {
    detail::CompiledShader fragment;
    if (!detail::compile_fragment(source, &fragment, &error)) return nullptr;
    auto program = std::make_shared<Program>();
    program->device = device->handle();
    program->role = role;
    const auto module = [&](const std::vector<std::uint32_t>& code, VkShaderModule* out) {
      const VkShaderModuleCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          .codeSize = code.size() * sizeof(std::uint32_t),
          .pCode = code.data(),
      };
      return vkCreateShaderModule(device->handle(), &info, nullptr, out) == VK_SUCCESS;
    };
    if (!module(vertex_spirv, &program->vertex) || !module(fragment.spirv, &program->fragment)) {
      error = "could not create a Vulkan shader module";
      return nullptr;
    }
    // An unreferenced config block is stripped from SPIR-V; its values are unused.
    if (!fragment.config) return program;
    std::vector<std::byte> bytes;
    if (!encode_config(*fragment.config, values, role, &bytes, &error)) return nullptr;
    if (bytes.empty()) bytes.resize(16);
    if (!create_config(*program, bytes, error)) return nullptr;
    return program;
  }

  bool create_config(Program& program, const std::vector<std::byte>& bytes, std::string& error) {
    const VkDevice handle = device->handle();
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes.size(),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(handle, &info, nullptr, &program.config_buffer) != VK_SUCCESS) {
      error = "could not create a shader config buffer";
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(handle, program.config_buffer, &requirements);
    const auto type = device->memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type.value_or(0),
    };
    void* mapping = nullptr;
    if (!type || vkAllocateMemory(handle, &allocate, nullptr, &program.config_memory) != VK_SUCCESS ||
        vkBindBufferMemory(handle, program.config_buffer, program.config_memory, 0) != VK_SUCCESS ||
        vkMapMemory(handle, program.config_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
      error = "could not allocate shader config memory";
      return false;
    }
    std::memcpy(mapping, bytes.data(), bytes.size());
    vkUnmapMemory(handle, program.config_memory);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    if (vkCreateDescriptorPool(handle, &pool_info, nullptr, &program.config_pool) != VK_SUCCESS) {
      error = "could not create a shader config descriptor pool";
      return false;
    }
    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = program.config_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &config_layout,
    };
    if (vkAllocateDescriptorSets(handle, &set_info, &program.config_set) != VK_SUCCESS) {
      error = "could not allocate a shader config descriptor set";
      return false;
    }
    const VkDescriptorBufferInfo buffer{program.config_buffer, 0, bytes.size()};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = program.config_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &buffer,
    };
    vkUpdateDescriptorSets(handle, 1, &write, 0, nullptr);
    return true;
  }

  VkPipeline pipeline(Program& program, VkFormat format, bool blend_enabled, int specialization) {
    const auto key = std::tuple{format, blend_enabled, specialization};
    if (const auto found = program.pipelines.find(key); found != program.pipelines.end()) return found->second;
    const VkSpecializationMapEntry entry{0, 0, sizeof(std::int32_t)};
    const std::int32_t value = specialization;
    const VkSpecializationInfo specialization_info{1, &entry, sizeof(value), &value};
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = program.vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = program.fragment, .pName = "main", .pSpecializationInfo = &specialization_info},
    }};
    const VkPipelineVertexInputStateCreateInfo vertex_input{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Premultiplied alpha over.
    const VkPipelineColorBlendAttachmentState attachment{
        .blendEnable = blend_enabled ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    constexpr std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic.size()),
        .pDynamicStates = dynamic.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend_state,
        .pDynamicState = &dynamic_state,
        .layout = layout,
    };
    VkPipeline created = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device->handle(), cache, 1, &info, nullptr, &created) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    program.pipelines.emplace(key, created);
    return created;
  }

  std::shared_ptr<Arena> acquire_arena() {
    for (const auto& arena : arenas)
      if (arena.use_count() == 1) {
        arena->reset();
        return arena;
      }
    auto arena = std::make_shared<Arena>();
    arena->device = device.get();
    arenas.push_back(arena);
    return arena;
  }

  bool supports_blit(VkFormat format) {
    const auto found = blit_support.find(format);
    if (found != blit_support.end()) return found->second;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device->physical(), format, &properties);
    const bool supported = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    blit_support.emplace(format, supported);
    return supported;
  }

  Snapshots& snapshots_for(Size size) {
    ++frame_counter;
    for (auto& entry : snapshots)
      if (entry.size.width == size.width && entry.size.height == size.height) {
        entry.used = frame_counter;
        return entry;
      }
    if (snapshots.size() >= 4)
      snapshots.erase(std::min_element(snapshots.begin(), snapshots.end(),
                                       [](const auto& left, const auto& right) { return left.used < right.used; }));
    snapshots.push_back({size, frame_counter, {}, {}, {}, {}});
    return snapshots.back();
  }

  TextureRef make_target(std::uint32_t width, std::uint32_t height, std::string* error = nullptr) {
    return vlk::Image::create(device, width, height, kTextureFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, error);
  }

  void keep(const TextureRef& texture) {
    if (batch && texture != nullptr) batch->commands.keep(texture);
  }

  void end_rendering() {
    if (!batch || !batch->rendering) return;
    vkCmdEndRendering(batch->commands.get());
    batch->rendering = false;
  }

  // Must run outside dynamic rendering.
  void prepare_sampled(const TextureRef& texture) {
    if (!batch || texture == nullptr) return;
    keep(texture);
    if (texture->external()) {
      if (texture->acquired() && texture->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
      const bool first = !texture->acquired();
      texture->transition(batch->commands.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      if (first) {
        batch->externals.push_back(texture);
        if (texture->has_dmabuf()) {
          (void)batch->commands.wait_sync_file(texture->export_sync_file(false),
                                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
          batch->reads.push_back(texture);
        }
      }
      return;
    }
    if (texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      texture->transition(batch->commands.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  std::int32_t texture_index(const TextureRef& texture) {
    if (texture == nullptr) return 0;
    if (!texture->acquired() || texture->layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      if (!warned_layout) {
        warned_layout = true;
        std::fputs("zwwm: vulkan renderer sampled an unprepared texture; substituting black\n", stderr);
      }
      return 0;
    }
    if (const auto found = batch->texture_indices.find(texture.get()); found != batch->texture_indices.end())
      return found->second;
    if (batch->textures.size() >= maximum_textures) return 0;
    const auto index = static_cast<std::int32_t>(batch->textures.size());
    batch->textures.push_back(texture);
    batch->texture_indices.emplace(texture.get(), index);
    keep(texture);
    return index;
  }

  void set_target(const TextureRef& target, std::optional<std::array<float, 4>> clear, bool discard = false) {
    if (batch->target == target && !clear && !discard) return;
    end_rendering();
    keep(target);
    batch->target = target;
    batch->clear = clear;
    batch->bound = VK_NULL_HANDLE;
    const bool overwrite = discard || clear.has_value();
    if (target->external()) {
      const bool first = !target->acquired();
      if (first || target->layout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        target->transition(batch->commands.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, first && overwrite);
      if (first) {
        batch->externals.push_back(target);
        if (target->has_dmabuf()) {
          (void)batch->commands.wait_sync_file(target->export_sync_file(true),
                                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
          batch->writes.push_back(target);
        }
      }
      return;
    }
    if (target->layout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || overwrite)
      target->transition(batch->commands.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         overwrite || target->layout() == VK_IMAGE_LAYOUT_UNDEFINED);
  }

  void ensure_rendering() {
    if (batch->rendering) return;
    const auto& target = batch->target;
    VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target->view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = batch->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    if (batch->clear) std::memcpy(attachment.clearValue.color.float32, batch->clear->data(), sizeof(float) * 4);
    const VkRenderingInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {target->width(), target->height()}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(batch->commands.get(), &info);
    batch->rendering = true;
    batch->clear.reset();
    batch->bound = VK_NULL_HANDLE;
    batch->bound_config = nullptr;
  }

  bool bind_frame_set(VkBuffer buffer, VkDeviceSize offset) {
    if (batch->frame_set == VK_NULL_HANDLE || batch->frame_set_buffer != buffer ||
        batch->frame_set_textures != batch->textures.size()) {
      const auto count = static_cast<std::uint32_t>(batch->textures.size());
      const VkDescriptorSet set = batch->arena->allocate(frame_layout, count);
      if (set == VK_NULL_HANDLE) return false;
      std::vector<VkDescriptorImageInfo> images;
      images.reserve(count);
      for (const auto& texture : batch->textures)
        images.push_back({sampler, texture->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      const VkDescriptorBufferInfo uniform{buffer, 0, kDrawSize};
      const std::array<VkWriteDescriptorSet, 2> writes{{
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &uniform},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = count,
           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = images.data()},
      }};
      vkUpdateDescriptorSets(device->handle(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
      batch->frame_set = set;
      batch->frame_set_buffer = buffer;
      batch->frame_set_textures = count;
    }
    const auto dynamic_offset = static_cast<std::uint32_t>(offset);
    vkCmdBindDescriptorSets(batch->commands.get(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &batch->frame_set,
                            1, &dynamic_offset);
    return true;
  }

  bool draw(const ProgramRef& program_ref, DrawData data, bool blend, int specialization = 0) {
    auto& program = *program_ref;
    const auto& target = batch->target;
    if (target == nullptr) return false;
    const VkPipeline selected = pipeline(program, target->format(), blend, specialization);
    if (selected == VK_NULL_HANDLE) return false;
    if (std::find(batch->kept_programs.begin(), batch->kept_programs.end(), &program) == batch->kept_programs.end()) {
      batch->kept_programs.push_back(&program);
      batch->commands.keep(program_ref);
    }
    data.output_size = {static_cast<float>(target->width()), static_cast<float>(target->height())};
    const auto slice = batch->arena->write(&data, sizeof(data), kDrawSize);
    if (!slice) return false;
    ensure_rendering();
    const VkCommandBuffer commands = batch->commands.get();
    if (batch->bound != selected) {
      vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, selected);
      batch->bound = selected;
    }
    if (!bind_frame_set(slice->first, slice->second)) return false;
    if (program.config_set != VK_NULL_HANDLE && batch->bound_config != &program) {
      vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &program.config_set, 0, nullptr);
      batch->bound_config = &program;
    }
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(target->width()), static_cast<float>(target->height()),
                              0.0F, 1.0F};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D clip = scissor.value_or(VkRect2D{{0, 0}, {target->width(), target->height()}});
    vkCmdSetScissor(commands, 0, 1, &clip);
    vkCmdDraw(commands, 6, 1, 0, 0);
    return true;
  }

  bool copy_target(TextureRef& destination) {
    const auto& target = batch->target;
    if (target == nullptr || (target->usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || target->external())
      return false;
    const bool same_format = target->format() == kTextureFormat;
    if (!same_format && !supports_blit(target->format())) return false;
    if (destination == nullptr || destination->width() != target->width() || destination->height() != target->height()) {
      destination = make_target(target->width(), target->height());
      if (destination == nullptr) return false;
    }
    end_rendering();
    keep(destination);
    const VkCommandBuffer commands = batch->commands.get();
    target->transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    destination->transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
    const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (same_format) {
      const VkImageCopy region{layers, {0, 0, 0}, layers, {0, 0, 0}, {target->width(), target->height(), 1}};
      vkCmdCopyImage(commands, target->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
      const auto width = static_cast<std::int32_t>(target->width());
      const auto height = static_cast<std::int32_t>(target->height());
      VkImageBlit region{.srcSubresource = layers, .dstSubresource = layers};
      region.srcOffsets[1] = region.dstOffsets[1] = {width, height, 1};
      vkCmdBlitImage(commands, target->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    }
    destination->transition(commands, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    target->transition(commands, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    batch->bound = VK_NULL_HANDLE;
    return true;
  }
};

bool border_ring_contains(Size size, float corner_radius, float border_width, float x, float y) {
  if (size.width == 0 || size.height == 0 || !std::isfinite(corner_radius) || !std::isfinite(border_width) ||
      corner_radius < 0.0F || border_width <= 0.0F) return false;
  const float width = static_cast<float>(size.width);
  const float height = static_cast<float>(size.height);
  if (rounded_rectangle_distance(x, y, width, height, corner_radius) > 0.0F) return false;
  const float inner_width = width - 2.0F * border_width;
  const float inner_height = height - 2.0F * border_width;
  return inner_width <= 0.0F || inner_height <= 0.0F ||
         rounded_rectangle_distance(x - border_width, y - border_width, inner_width, inner_height,
                                    std::max(corner_radius - border_width, 0.0F)) > 0.0F;
}

VulkanRenderer::VulkanRenderer() = default;
VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::initialize(std::shared_ptr<vlk::Device> device) {
  if (ready()) return true;
  if (device == nullptr) {
    last_error_ = "Vulkan device is unavailable";
    return false;
  }
  auto state = std::make_unique<State>();
  state->device = std::move(device);
  const VkDevice handle = state->device->handle();
  const VkSamplerCreateInfo sampler{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0F,
  };
  const VkPipelineCacheCreateInfo cache{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (vkCreateSampler(handle, &sampler, nullptr, &state->sampler) != VK_SUCCESS ||
      vkCreatePipelineCache(handle, &cache, nullptr, &state->cache) != VK_SUCCESS) {
    last_error_ = "could not create Vulkan sampler state";
    return false;
  }
  if (!state->create_layouts(last_error_)) return false;
  detail::CompiledShader vertex;
  if (!detail::compile_vertex(shader::surface_vertex, &vertex, &last_error_)) {
    last_error_ = "surface.vert: " + last_error_;
    return false;
  }
  state->vertex_spirv = std::move(vertex.spirv);
  std::string error;
  const auto builtin = [&](const char* name, std::string_view source, ProgramRef* out) {
    *out = state->make_program(source, ShaderRole::builtin, {}, error);
    if (*out == nullptr) last_error_ = std::string(name) + ": " + error;
    return *out != nullptr;
  };
  if (!builtin("surface.frag", shader::surface_fragment, &state->surface) ||
      !builtin("glass.frag", shader::glass_fragment, &state->glass) ||
      !builtin("kawase.frag", shader::kawase_fragment, &state->kawase))
    return false;
  state->dummy = vlk::Image::create(state->device, 1, 1, kTextureFormat, 0, &error);
  constexpr std::uint32_t black = 0xff000000U;
  if (state->dummy == nullptr || !state->dummy->upload(&black, 1, 0, 0, 1, 1, &error)) {
    last_error_ = "could not create the placeholder texture: " + error;
    return false;
  }
  state_ = std::move(state);
  last_error_.clear();
  return true;
}

bool VulkanRenderer::prepare_shaders(const ShaderSources& sources) {
  discard_shaders();
  if (!ready() || sources.programs.empty() || sources.window.empty() || sources.border.empty()) {
    last_error_ = "named window and border shaders are required";
    return false;
  }
  auto& state = *state_;
  for (const auto& source : sources.programs) {
    if (source.name.empty() || state.pending_custom_programs.contains(source.name)) {
      last_error_ = "shader names must be nonempty and unique";
      discard_shaders();
      return false;
    }
    std::string error;
    auto program = state.make_program(source.source, source.role, source.values, error);
    if (program == nullptr) {
      last_error_ = "shader " + source.name + ": " + error;
      discard_shaders();
      return false;
    }
    state.pending_custom_programs.emplace(source.name, State::CustomProgram{std::move(program), source.role});
  }
  const auto valid_default = [&](const std::string& name, ShaderRole role) {
    const auto found = state.pending_custom_programs.find(name);
    return found != state.pending_custom_programs.end() && found->second.role == role;
  };
  if (!valid_default(sources.window, ShaderRole::window) || !valid_default(sources.border, ShaderRole::border) ||
      (!sources.background.empty() && !valid_default(sources.background, ShaderRole::background)) ||
      (!sources.shadow.empty() && !valid_default(sources.shadow, ShaderRole::shadow))) {
    last_error_ = "default shader names must resolve to programs with matching roles";
    discard_shaders();
    return false;
  }
  const auto load_image = [&](const std::string& path, TextureRef* texture) {
    std::vector<std::uint32_t> pixels;
    std::uint32_t width = 0, height = 0;
    std::string error;
    if (!load_png(path, &pixels, &width, &height, state.device->limits().maxImageDimension2D, error)) {
      last_error_ = error;
      return false;
    }
    *texture = upload(nullptr, pixels.data(), width, height, width);
    return *texture != nullptr;
  };
  if (!sources.background.empty() && !load_image(default_wallpaper_path(), &state.pending_wallpaper)) {
    discard_shaders();
    return false;
  }
  if (!sources.overview_backdrop_image.empty() &&
      !load_image(sources.overview_backdrop_image, &state.pending_overview_backdrop)) {
    discard_shaders();
    return false;
  }
  state.pending_window_shader = sources.window;
  state.pending_border_shader = sources.border;
  state.pending_background_shader = sources.background;
  state.pending_shadow_shader = sources.shadow;
  state.pending = true;
  last_error_.clear();
  return true;
}

void VulkanRenderer::commit_shaders() {
  if (!ready() || !state_->pending) return;
  auto& state = *state_;
  state.custom_programs = std::move(state.pending_custom_programs);
  state.pending_custom_programs.clear();
  state.window_shader = std::move(state.pending_window_shader);
  state.border_shader = std::move(state.pending_border_shader);
  state.background_shader = std::move(state.pending_background_shader);
  state.shadow_shader = std::move(state.pending_shadow_shader);
  state.wallpaper = std::exchange(state.pending_wallpaper, nullptr);
  state.overview_backdrop = std::exchange(state.pending_overview_backdrop, nullptr);
  state.pending = false;
}

void VulkanRenderer::discard_shaders() {
  if (state_ == nullptr) return;
  auto& state = *state_;
  state.pending_custom_programs.clear();
  state.pending_window_shader.clear();
  state.pending_border_shader.clear();
  state.pending_background_shader.clear();
  state.pending_shadow_shader.clear();
  state.pending_wallpaper.reset();
  state.pending_overview_backdrop.reset();
  state.pending = false;
}

void VulkanRenderer::shutdown() { state_.reset(); }

bool VulkanRenderer::ready() const { return state_ != nullptr; }

const std::string& VulkanRenderer::last_error() const { return last_error_; }

const std::shared_ptr<vlk::Device>& VulkanRenderer::device() const {
  static const std::shared_ptr<vlk::Device> none;
  return state_ == nullptr ? none : state_->device;
}

bool VulkanRenderer::bind_texture(TextureHandle handle, TextureRef texture) {
  if (!ready() || handle == 0 || texture == nullptr) return false;
  state_->textures[handle] = std::move(texture);
  return true;
}

void VulkanRenderer::unbind_texture(TextureHandle handle) {
  if (ready()) state_->textures.erase(handle);
}

TextureRef VulkanRenderer::upload(TextureRef texture, const std::uint32_t* pixels, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t row_pixels, std::optional<Rect> damage) {
  if (!ready() || pixels == nullptr || width == 0 || height == 0) return nullptr;
  const bool reuse = texture != nullptr && !texture->external() && texture->width() == width &&
                     texture->height() == height && texture->format() == kTextureFormat;
  if (!reuse) {
    texture = vlk::Image::create(state_->device, width, height, kTextureFormat, 0, &last_error_);
    if (texture == nullptr) return nullptr;
    damage.reset();
  }
  std::uint32_t left = 0, top = 0, right = width, bottom = height;
  if (damage) {
    left = static_cast<std::uint32_t>(std::clamp<std::int64_t>(damage->origin.x, 0, width));
    top = static_cast<std::uint32_t>(std::clamp<std::int64_t>(damage->origin.y, 0, height));
    right = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(damage->origin.x) + damage->size.width, left, width));
    bottom = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(damage->origin.y) + damage->size.height, top, height));
    if (right == left || bottom == top) return texture;
  }
  if (!texture->upload(pixels + static_cast<std::size_t>(top) * row_pixels + left, row_pixels, left, top,
                       right - left, bottom - top, &last_error_))
    return nullptr;
  return texture;
}

TextureRef VulkanRenderer::import_dmabuf(const DmabufAttributes& attributes) {
  if (!ready()) {
    last_error_ = "Vulkan renderer is not initialized";
    return nullptr;
  }
  return vlk::Image::import_dmabuf(state_->device, attributes, VK_IMAGE_USAGE_SAMPLED_BIT, &last_error_);
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> VulkanRenderer::dmabuf_formats() const {
  if (!ready()) return {};
  return state_->device->sampled_dmabuf_formats();
}

TextureRef VulkanRenderer::create_target(Size size) {
  if (!ready() || size.width == 0 || size.height == 0) return nullptr;
  return state_->make_target(size.width, size.height, &last_error_);
}

TextureRef VulkanRenderer::snapshot(const TextureRef& source) {
  if (!ready() || source == nullptr) return nullptr;
  auto copy_target = create_target({source->width(), source->height()});
  if (copy_target == nullptr || !copy(source, copy_target)) return nullptr;
  const bool own_batch = !recording();
  if (own_batch && !begin()) return nullptr;
  state_->end_rendering();
  state_->prepare_sampled(copy_target);
  if (own_batch && !submit()) return nullptr;
  return copy_target;
}

bool VulkanRenderer::begin() {
  if (!ready()) return false;
  if (recording()) return true;
  auto& state = *state_;
  vlk::Commands commands = state.device->begin(&last_error_);
  if (!commands) return false;
  state.batch.emplace();
  state.batch->commands = std::move(commands);
  state.batch->arena = state.acquire_arena();
  state.batch->commands.keep(state.batch->arena);
  state.batch->textures.push_back(state.dummy);
  state.batch->texture_indices.emplace(state.dummy.get(), 0);
  state.batch->commands.keep(state.dummy);
  return true;
}

bool VulkanRenderer::recording() const { return ready() && state_->batch.has_value(); }

bool VulkanRenderer::submit(bool wait) {
  if (!recording()) return false;
  auto& state = *state_;
  state.end_rendering();
  auto batch = std::move(*state.batch);
  state.batch.reset();
  state.scissor.reset();
  for (const auto& texture : batch.externals) texture->release(batch.commands.get());
  const bool share = !batch.reads.empty() || !batch.writes.empty();
  int sync_file = -1;
  if (!state.device->submit(batch.commands, share ? &sync_file : nullptr, wait, &last_error_)) return false;
  if (sync_file >= 0) {
    bool exported = true;
    for (const auto& texture : batch.reads) (void)texture->import_sync_file(sync_file, false);
    for (const auto& texture : batch.writes) exported = texture->import_sync_file(sync_file, true) && exported;
    (void)close(sync_file);
    // Without a dma-buf fence, scanout must not see a half-rendered frame.
    if (!exported) state.device->wait_idle();
  }
  return true;
}

bool VulkanRenderer::copy(const TextureRef& source, const TextureRef& target) {
  if (!ready() || source == nullptr || target == nullptr) return false;
  const bool own_batch = !recording();
  if (own_batch && !begin()) return false;
  auto& state = *state_;
  state.end_rendering();
  state.prepare_sampled(source);
  state.set_target(target, std::nullopt, true);
  DrawData data;
  data.draw_rect = {0.0F, 0.0F, static_cast<float>(target->width()), static_cast<float>(target->height())};
  data.flags = kHasTexture;
  data.window_texture = state.texture_index(source);
  if (!state.draw(state.surface, data, false)) {
    last_error_ = "could not record a Vulkan copy";
    return false;
  }
  if (own_batch) return submit();
  return true;
}

bool VulkanRenderer::render(const FramePlan& frame, const TextureRef& target,
                            std::optional<std::array<float, 4>> clear) {
  if (!ready() || target == nullptr) return false;
  const Size target_size{target->width(), target->height()};
  if (std::any_of(frame.draws.begin(), frame.draws.end(), [](const DrawCall& draw) {
        return draw.bounds.empty() || !std::isfinite(draw.corner_radius) || draw.corner_radius < 0.0F ||
               !std::isfinite(draw.border_width) || draw.border_width < 0.0F ||
               !std::all_of(draw.source_uv.begin(), draw.source_uv.end(), [](float value) { return std::isfinite(value); }) ||
               draw.source_uv[0] < 0.0F || draw.source_uv[1] < 0.0F ||
               draw.source_uv[2] > 1.0F || draw.source_uv[3] > 1.0F ||
               draw.source_uv[0] >= draw.source_uv[2] || draw.source_uv[1] >= draw.source_uv[3] ||
               !std::isfinite(draw.clip_radius) || draw.clip_radius < 0.0F ||
               !std::isfinite(draw.background_blur_radius) || draw.background_blur_radius < 0.0F;
      })) {
    last_error_ = "frame contains invalid draw geometry";
    return false;
  }
  const bool own_batch = !recording();
  if (own_batch && !begin()) return false;
  auto& state = *state_;
  auto& snapshots = state.snapshots_for(target_size);
  const auto target_width = static_cast<float>(target_size.width);
  const auto target_height = static_cast<float>(target_size.height);

  // Layout transitions cannot happen inside dynamic rendering.
  state.end_rendering();
  for (const auto& draw : frame.draws) state.prepare_sampled(draw.texture);
  state.prepare_sampled(state.wallpaper);
  state.prepare_sampled(state.overview_backdrop);
  state.set_target(target, clear);
  bool complete = true;

  auto prepare_blur = [&](const DrawCall& draw) {
    if (!state.copy_target(snapshots.background)) return false;
    if (draw.glass && !state.copy_target(snapshots.reflection)) return false;
    const std::size_t requested = std::clamp<std::size_t>(draw.background_blur_passes, 1, 8);
    const float base_offset = std::clamp(draw.background_blur_radius / (2.0F * static_cast<float>(requested)), 0.5F, 8.0F);
    std::vector<Size> sizes;
    for (std::uint32_t width = target_size.width, height = target_size.height;
         sizes.size() < requested && (width > 1 || height > 1);) {
      width = std::max(1U, width / 2U);
      height = std::max(1U, height / 2U);
      sizes.push_back({width, height});
    }
    if (snapshots.kawase.size() < sizes.size()) snapshots.kawase.resize(sizes.size());
    for (std::size_t index = 0; index < sizes.size(); ++index) {
      auto& level = snapshots.kawase[index];
      if (level == nullptr || level->width() != sizes[index].width || level->height() != sizes[index].height)
        level = state.make_target(sizes[index].width, sizes[index].height);
      if (level == nullptr) return false;
    }
    const auto pass = [&](const TextureRef& source, Size source_size, const TextureRef& destination, float offset,
                          int mode) {
      state.end_rendering();
      state.prepare_sampled(source);
      state.set_target(destination, std::nullopt, true);
      DrawData data;
      data.draw_rect = {0.0F, 0.0F, static_cast<float>(destination->width()), static_cast<float>(destination->height())};
      data.window_texture = state.texture_index(source);
      data.texel_size = {1.0F / static_cast<float>(source_size.width), 1.0F / static_cast<float>(source_size.height)};
      data.sample_offset = offset;
      data.brightness = draw.background_blur_brightness;
      data.contrast = draw.background_blur_contrast;
      data.saturation = draw.background_blur_saturation;
      data.noise = draw.background_blur_noise;
      return state.draw(state.kawase, data, false, mode);
    };
    bool passes = true;
    TextureRef source = snapshots.background;
    Size source_size = target_size;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
      passes = pass(source, source_size, snapshots.kawase[index], base_offset + static_cast<float>(index) * 0.5F,
                    kKawaseDownsample) && passes;
      source = snapshots.kawase[index];
      source_size = sizes[index];
    }
    for (std::size_t index = sizes.size(); index > 0; --index) {
      const std::size_t level = index - 1;
      passes = pass(snapshots.kawase[level], sizes[level], level == 0 ? snapshots.background : snapshots.kawase[level - 1],
                    base_offset * 0.5F + static_cast<float>(level) * 0.5F,
                    level == 0 ? kKawaseFinalize : kKawaseUpsample) && passes;
    }
    state.end_rendering();
    state.prepare_sampled(snapshots.background);
    state.set_target(target, std::nullopt);
    return passes;
  };

  if (frame.draw_background && !state.background_shader.empty() && state.wallpaper != nullptr) {
    const float output_aspect = target_width / target_height;
    const float image_aspect = static_cast<float>(state.wallpaper->width()) / static_cast<float>(state.wallpaper->height());
    DrawData data;
    data.draw_rect = {0.0F, 0.0F, target_width, target_height};
    if (output_aspect > image_aspect) {
      const float top = (1.0F - image_aspect / output_aspect) * 0.5F;
      data.source_uv = {0.0F, top, 1.0F, 1.0F - top};
    } else {
      const float left = (1.0F - output_aspect / image_aspect) * 0.5F;
      data.source_uv = {left, 0.0F, 1.0F - left, 1.0F};
    }
    data.flags = kHasTexture;
    data.window_texture = state.texture_index(state.wallpaper);
    complete = state.draw(state.surface, data, false) && complete;
  }
  const auto custom_program = [&](const std::string& requested, const std::string& fallback,
                                  ShaderRole role) -> const ProgramRef* {
    const auto selected = state.custom_programs.find(requested.empty() ? fallback : requested);
    if (selected == state.custom_programs.end() || selected->second.role != role) return nullptr;
    return &selected->second.program;
  };
  if (const auto* background_program = custom_program({}, state.background_shader, ShaderRole::background);
      frame.draw_background && background_program != nullptr) {
    DrawData data;
    data.draw_rect = data.clip_rect = data.texture_rect = data.toplevel_rect = {0.0F, 0.0F, target_width, target_height};
    data.time = frame.shader_time;
    data.background_texture = state.texture_index(state.wallpaper);
    complete = state.draw(*background_program, data, true) && complete;
  }
  if (frame.overview_backdrop) (void)state.copy_target(snapshots.overview_scene);

  const auto draw_surface = [&](const RenderDraw& draw, bool background_only) {
    const bool blend = background_only || !draw.opaque || draw.opacity < 1.0F || draw.corner_radius > 0.0F ||
                       draw.border_width > 0.0F || draw.clip_bounds.has_value();
    const float pixel_width = static_cast<float>(draw.bounds.size.width);
    const float pixel_height = static_cast<float>(draw.bounds.size.height);
    const bool sample_surface = draw.texture != nullptr && (!background_only || draw.background_mask_texture);
    DrawData data;
    data.draw_rect = rect_of(draw.bounds);
    data.corner_radius = std::min(draw.corner_radius, 0.5F * std::min(pixel_width, pixel_height));
    data.border_width = background_only ? 0.0F : std::min(draw.border_width, 0.5F * std::min(pixel_width, pixel_height));
    data.color = background_only ? std::array<float, 4>{1.0F, 1.0F, 1.0F, draw.opacity}
                                 : std::array<float, 4>{draw.color[0], draw.color[1], draw.color[2],
                                                        draw.color[3] * draw.opacity};
    data.mask_threshold = draw.background_mask_alpha_threshold;
    data.source_uv = draw.source_uv;
    data.texture_transform = sample_surface ? static_cast<std::int32_t>(draw.texture_transform) : 0;
    data.texture_corner_radius = draw.texture_corner_radius;
    if (draw.texture_bounds) data.texture_rect = rect_of(*draw.texture_bounds);
    data.flags = (sample_surface ? kHasTexture : 0U) | (draw.texture_bounds ? kHasTextureRect : 0U) |
                 (background_only ? kBackgroundOnly : 0U) | (draw.clip_bounds ? kHasClip : 0U) |
                 (draw.strict_clip ? kStrictClip : 0U);
    if (sample_surface) data.window_texture = state.texture_index(draw.texture);
    if (background_only) data.blurred_backdrop_texture = state.texture_index(snapshots.background);
    if (draw.clip_bounds) {
      data.clip_rect = rect_of(*draw.clip_bounds);
      data.clip_radius = draw.clip_radius;
      const float clip_width = static_cast<float>(std::max(1.0, draw.clip_bounds->size.width));
      const float clip_height = static_cast<float>(std::max(1.0, draw.clip_bounds->size.height));
      data.input_to_clip_0 = {pixel_width / clip_width, 0.0F,
                              static_cast<float>(draw.bounds.origin.x - draw.clip_bounds->origin.x) / clip_width, 0.0F};
      data.input_to_clip_1 = {0.0F, pixel_height / clip_height,
                              static_cast<float>(draw.bounds.origin.y - draw.clip_bounds->origin.y) / clip_height, 0.0F};
    }
    complete = state.draw(state.surface, data, blend) && complete;
  };

  if (frame.overview_backdrop) {
    std::vector<Rect> scenes;
    for (const auto& draw : frame.draws) if (draw.shader_role == ShaderRole::background) scenes.push_back(draw.bounds);
    auto regions = uncovered_regions(target_size, scenes);
    for (const auto& draw : frame.draws) if (draw.shader_role == ShaderRole::background && draw.corner_radius > 0) {
      const int radius = static_cast<int>(std::ceil(std::min(draw.corner_radius, 0.5F * std::min(draw.bounds.size.width, draw.bounds.size.height))));
      for (const int x : {draw.bounds.origin.x, draw.bounds.origin.x + static_cast<int>(draw.bounds.size.width) - radius})
        for (const int y : {draw.bounds.origin.y, draw.bounds.origin.y + static_cast<int>(draw.bounds.size.height) - radius}) {
          const int left = std::max(0, x), top = std::max(0, y);
          const int right = std::min(static_cast<int>(target_size.width), x + radius), bottom = std::min(static_cast<int>(target_size.height), y + radius);
          if (right > left && bottom > top) regions.push_back({{left, top}, {static_cast<unsigned>(right - left), static_cast<unsigned>(bottom - top)}});
        }
    }
    DrawCall backdrop(0, {{0, 0}, target_size}, std::clamp(frame.overview_backdrop_opacity, 0.0F, 1.0F), {1, 1, 1, 1}, nullptr);
    if (state.overview_backdrop != nullptr) {
      backdrop.texture = state.overview_backdrop;
      const float transition_opacity = backdrop.opacity;
      backdrop.opacity = 1.0F;
      const float screen = target_width / target_height;
      const float image = static_cast<float>(state.overview_backdrop->width()) / static_cast<float>(state.overview_backdrop->height());
      if (screen > image) { const float shown = image / screen; backdrop.source_uv = {0, (1 - shown) / 2, 1, (1 + shown) / 2}; }
      else { const float shown = screen / image; backdrop.source_uv = {(1 - shown) / 2, 0, (1 + shown) / 2, 1}; }
      // Blur the whole backdrop so scene colors cannot leak into the kernel.
      draw_surface(RenderDraw(backdrop), false);
      backdrop.opacity = transition_opacity;
    }
    backdrop.texture.reset();
    backdrop.background_blur_radius = frame.overview_backdrop_blur_radius * backdrop.opacity;
    backdrop.background_blur_passes = frame.overview_backdrop_blur_passes;
    if (!regions.empty() && backdrop.background_blur_radius > 0 && prepare_blur(backdrop)) {
      for (const auto& region : regions) {
        state.scissor = VkRect2D{{region.origin.x, region.origin.y}, {region.size.width, region.size.height}};
        draw_surface(RenderDraw(backdrop), true);
      }
    }
    state.scissor.reset();
  }

  const auto draw_glass = [&](const RenderDraw& draw) {
    const float pixel_width = static_cast<float>(draw.bounds.size.width);
    const float pixel_height = static_cast<float>(draw.bounds.size.height);
    DrawData data;
    data.draw_rect = rect_of(draw.bounds);
    data.corner_radius = std::min(draw.corner_radius, 0.5F * std::min(pixel_width, pixel_height));
    data.flags = draw.clip_bounds ? kHasClip : 0U;
    data.clip_rect = rect_of(draw.clip_bounds.value_or(draw.bounds));
    data.clip_radius = draw.clip_radius;
    data.opacity = draw.opacity;
    data.time = draw.glass_time;
    data.glass_tint = draw.glass_tint;
    data.glass_refraction = draw.glass_refraction;
    data.glass_dispersion = draw.glass_dispersion;
    data.glass_frost = draw.glass_frost;
    data.glass_ior = draw.glass_ior;
    data.glass_thickness = draw.glass_thickness;
    data.glass_roughness = draw.glass_roughness;
    data.blurred_backdrop_texture = state.texture_index(snapshots.background);
    data.backdrop_texture = state.texture_index(snapshots.reflection);
    const int quality = static_cast<int>(std::clamp<std::uint32_t>(draw.glass_quality, 1, 3));
    complete = state.draw(state.glass, data, true, quality) && complete;
  };
  const auto draw_custom = [&](const RenderDraw& draw, const ProgramRef& selected, bool blurred_backdrop_ready) {
    DrawData data;
    data.draw_rect = rect_of(draw.bounds);
    data.toplevel_rect = rect_of(draw.toplevel_bounds.empty() ? draw.bounds : draw.toplevel_bounds);
    data.texture_rect = rect_of(draw.texture_bounds.value_or(draw.bounds));
    data.clip_rect = rect_of(draw.clip_bounds.value_or(draw.bounds));
    data.source_uv = draw.source_uv;
    data.color = draw.color;
    data.opacity = draw.opacity;
    data.time = draw.shader_time;
    data.corner_radius = draw.corner_radius;
    data.border_width = draw.border_width;
    data.clip_radius = draw.clip_radius;
    data.texture_corner_radius = draw.texture_corner_radius;
    data.state = draw.toplevel_state;
    data.texture_transform = static_cast<std::int32_t>(draw.texture_transform);
    data.flags = (draw.texture != nullptr ? kHasTexture : 0U) | (draw.texture_bounds ? kHasTextureRect : 0U) |
                 (draw.clip_bounds ? kHasClip : 0U) | (draw.strict_clip ? kStrictClip : 0U);
    data.window_texture = state.texture_index(draw.texture);
    data.backdrop_texture = state.texture_index(snapshots.reflection);
    data.blurred_backdrop_texture = state.texture_index(blurred_backdrop_ready ? snapshots.background : snapshots.reflection);
    data.background_texture = state.texture_index(state.wallpaper);
    complete = state.draw(selected, data, true) && complete;
  };

  // Capture backdrops before a toplevel's shadow and border are drawn.
  std::unordered_map<NodeId, const DrawCall*> window_draws;
  for (const auto& draw : frame.draws)
    if (draw.shader_role == ShaderRole::window && draw.toplevel_id != 0) window_draws.try_emplace(draw.toplevel_id, &draw);
  NodeId backdrop_toplevel = 0;
  bool custom_toplevel_blur_ready = false;
  const DrawCall* shared_backdrop = nullptr;
  for (const DrawCall& item : frame.draws) {
    const RenderDraw draw(item);
    if (item.toplevel_id != 0 && item.toplevel_id != backdrop_toplevel) {
      const auto window = window_draws.find(item.toplevel_id);
      if (window != window_draws.end() &&
          custom_program(window->second->shader_name, state.window_shader, ShaderRole::window) != nullptr) {
        const DrawCall& target = *window->second;
        backdrop_toplevel = item.toplevel_id;
        const bool reuse = shared_backdrop != nullptr && target.shared_backdrop &&
                           target.background_blur_radius == shared_backdrop->background_blur_radius &&
                           target.background_blur_passes == shared_backdrop->background_blur_passes;
        if (!reuse) {
          (void)state.copy_target(snapshots.reflection);
          custom_toplevel_blur_ready = target.background_blur_radius > 0.0F && prepare_blur(target);
          shared_backdrop = target.shared_backdrop ? &target : nullptr;
        }
      }
    }
    if (draw.shader_role == ShaderRole::shadow) {
      if (const auto* shadow = custom_program(draw.shader_name, state.shadow_shader, ShaderRole::shadow))
        draw_custom(draw, *shadow, false);
      continue;
    }
    if (frame.overview_backdrop && draw.shader_role == ShaderRole::background && snapshots.overview_scene != nullptr) {
      DrawCall scene(item);
      scene.shader_role = ShaderRole::builtin;
      scene.texture = snapshots.overview_scene;
      scene.source_uv = {0, 0, 1, 1};
      draw_surface(RenderDraw(scene), false);
      continue;
    }
    const auto* selected_window = draw.shader_role == ShaderRole::window ?
        custom_program(draw.shader_name, state.window_shader, ShaderRole::window) : nullptr;
    const auto* selected_border = draw.shader_role == ShaderRole::border ?
        custom_program(draw.shader_name, state.border_shader, ShaderRole::border) : nullptr;
    const auto* selected_background = draw.shader_role == ShaderRole::background ?
        custom_program(draw.shader_name, state.background_shader, ShaderRole::background) : nullptr;
    const bool custom_window = selected_window != nullptr;
    bool blurred_backdrop_ready = custom_window ? custom_toplevel_blur_ready : false;
    if (!custom_window && (draw.background_blur_radius > 0.0F || draw.glass)) {
      // Overwrites the shared backdrop's snapshots.
      shared_backdrop = nullptr;
      backdrop_toplevel = 0;
      blurred_backdrop_ready = prepare_blur(draw);
      if (blurred_backdrop_ready) {
        if (draw.glass) draw_glass(draw);
        else draw_surface(draw, true);
      }
    }
    if (custom_window) draw_custom(draw, *selected_window, blurred_backdrop_ready);
    else if (selected_border != nullptr) draw_custom(draw, *selected_border, false);
    else if (selected_background != nullptr) draw_custom(draw, *selected_background, false);
    else draw_surface(draw, false);
  }
  state.scissor.reset();
  if (!complete) last_error_ = "could not record every Vulkan draw";
  if (own_batch) return submit() && complete;
  return complete;
}

bool VulkanRenderer::render_pixels(const FramePlan& frame, Size target_size, std::vector<std::uint32_t>* pixels) {
  if (pixels == nullptr || target_size.width == 0 || target_size.height == 0) return false;
  auto target = create_target(target_size);
  const bool success = target != nullptr && render(frame, target, std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F}) &&
                       read_pixels(target, pixels);
  if (!success) pixels->clear();
  return success;
}

bool VulkanRenderer::read_pixels(const TextureRef& texture, std::vector<std::uint32_t>* pixels) {
  if (!ready() || texture == nullptr || pixels == nullptr) return false;
  if (recording() && !submit()) return false;
  pixels->resize(static_cast<std::size_t>(texture->width()) * texture->height());
  if (!texture->read(*pixels, &last_error_)) {
    pixels->clear();
    return false;
  }
  return true;
}

FramePlan VulkanRenderer::build_frame(const Scene& scene, const DamageRegion& damage) const {
  FramePlan frame{damage.rectangles(), {}};
  if (damage.empty()) return frame;
  for (const SceneNode& node : scene.ordered_nodes()) {
    if (node.visible && node.opacity > 0.0F && damage.intersects(node.bounds)) {
      TextureRef texture;
      if (node.texture.has_value() && ready()) {
        const auto registered = state_->textures.find(*node.texture);
        if (registered != state_->textures.end()) texture = registered->second;
      }
      const float maximum_radius = 0.5F * static_cast<float>(std::min(node.bounds.size.width, node.bounds.size.height));
      frame.draws.push_back({node.id, node.bounds, node.opacity, node.color, texture,
                             std::min(node.corner_radius, maximum_radius)});
    }
  }
  return frame;
}

}  // namespace zwwm::renderer
