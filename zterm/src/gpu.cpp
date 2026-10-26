#include "gpu.hpp"
#include "terminal-frag.spv.h"
#include "terminal-vert.spv.h"

#include <linux-dmabuf-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zwwm-vlk/device.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace zterm {
namespace p = zwayland::generated;
namespace {

constexpr int atlas_size = 2048;
constexpr unsigned atlas_layers = 8;

struct Push {
  float viewport[2];
  float text_rounding;
};

int render_node(dev_t device) {
  std::array<drmDevicePtr, 64> devices{};
  const int count = drmGetDevices2(0, devices.data(), devices.size());
  int result = -1;
  for (int i = 0; i < count && result < 0; ++i) {
    bool matches = false;
    for (int node = 0; node < DRM_NODE_MAX; ++node) {
      if (!(devices[i]->available_nodes & (1 << node))) continue;
      struct stat info{};
      if (stat(devices[i]->nodes[node], &info) == 0 && info.st_rdev == device) matches = true;
    }
    if (matches && (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
      result = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
  }
  if (count > 0) drmFreeDevices(devices.data(), count);
  if (result < 0) throw std::runtime_error("cannot open the compositor GPU render node");
  return result;
}

void check(VkResult result, const char* what) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed");
}

}  // namespace

struct Gpu::Buffer {
  Gpu* owner = nullptr;
  gbm_bo* bo = nullptr;
  zwwm::vlk::ImageRef image;
  zwayland::client::Proxy* proxy = nullptr;
  bool busy = false, retired = false;
  ~Buffer() {
    if (proxy != nullptr) p::wl_buffer_destroy(owner->display_, proxy->id);
    image.reset();
    if (bo != nullptr) gbm_bo_destroy(bo);
  }
};

Gpu::Gpu(zwayland::client::Display& display) : display_(display) {}

Gpu::~Gpu() {
  if (vulkan_ != nullptr) {
    vulkan_->wait_idle();
    buffers_.clear();
    atlas_.reset();
    color_atlas_.reset();
    const VkDevice device = vulkan_->handle();
    for (const auto pipeline : pipelines_) if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
    if (fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragment_, nullptr);
    if (vertex_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertex_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device, sampler_, nullptr);
    vulkan_.reset();
  }
  buffers_.clear();
  if (gbm_ != nullptr) gbm_device_destroy(gbm_);
  if (device_fd_ >= 0) close(device_fd_);
}

void Gpu::initialize(dev_t device, const std::vector<Format>& formats, FontFamilies fonts, unsigned size,
                     unsigned weight, float text_rounding, float icon_scale) {
  fonts_ = std::move(fonts);
  font_weight_ = weight;
  text_rounding_ = text_rounding;
  icon_scale_ = icon_scale;
  device_fd_ = render_node(device);
  gbm_ = gbm_create_device(device_fd_);
  if (gbm_ == nullptr) throw std::runtime_error("cannot initialize GBM");
  // Matching the compositor's DRM node also rules out software rasterizers.
  std::string error;
  vulkan_ = zwwm::vlk::Device::create({.drm_device = device, .require_dmabuf = true}, &error);
  if (vulkan_ == nullptr) throw std::runtime_error("cannot initialize Vulkan: " + error);
  for (const auto preferred : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}) {
    const auto renderable = vulkan_->render_modifiers(preferred);
    for (const auto& entry : formats)
      if (entry.format == preferred && entry.modifier != DRM_FORMAT_MOD_INVALID &&
          std::find(renderable.begin(), renderable.end(), entry.modifier) != renderable.end())
        modifiers_.push_back(entry.modifier);
    if (!modifiers_.empty()) { format_ = preferred; break; }
  }
  if (!format_) throw std::runtime_error("no shared GPU buffer format");

  atlas_ = zwwm::vlk::Image::create_array(vulkan_, atlas_size, atlas_size, atlas_layers, VK_FORMAT_R8_UNORM, 0, &error);
  color_atlas_ = atlas_ == nullptr ? nullptr :
      zwwm::vlk::Image::create_array(vulkan_, atlas_size, atlas_size, 1, VK_FORMAT_R8G8B8A8_UNORM, 0, &error);
  if (color_atlas_ == nullptr) throw std::runtime_error("cannot allocate glyph atlases: " + error);
  {
    // Atlases start readable so unused layers sample as empty.
    auto commands = vulkan_->begin(&error);
    if (!commands) throw std::runtime_error("cannot record atlas setup: " + error);
    atlas_->transition(commands.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
    color_atlas_->transition(commands.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
    if (!vulkan_->submit(commands, nullptr, false, &error)) throw std::runtime_error("cannot submit atlas setup: " + error);
  }
  const VkDevice handle = vulkan_->handle();
  const VkSamplerCreateInfo sampler{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  };
  check(vkCreateSampler(handle, &sampler, nullptr, &sampler_), "vkCreateSampler");
  const std::array<VkSampler, 2> immutable{sampler_, sampler_};
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &immutable[0]},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &immutable[1]},
  }};
  const VkDescriptorSetLayoutCreateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<std::uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  check(vkCreateDescriptorSetLayout(handle, &set_info, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
  const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
  const VkPipelineLayoutCreateInfo layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout_,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push,
  };
  check(vkCreatePipelineLayout(handle, &layout_info, nullptr, &layout_), "vkCreatePipelineLayout");
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
  };
  check(vkCreateDescriptorPool(handle, &pool_info, nullptr, &pool_), "vkCreateDescriptorPool");
  const VkDescriptorSetAllocateInfo allocate{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool_,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout_,
  };
  check(vkAllocateDescriptorSets(handle, &allocate, &set_), "vkAllocateDescriptorSets");
  const std::array<VkDescriptorImageInfo, 2> images{{
      {VK_NULL_HANDLE, atlas_->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, color_atlas_->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  }};
  const std::array<VkWriteDescriptorSet, 2> writes{{
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set_, .dstBinding = 0, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set_, .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[1]},
  }};
  vkUpdateDescriptorSets(handle, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
  create_pipelines();
  set_font_size(size);
}

void Gpu::create_pipelines() {
  const VkDevice handle = vulkan_->handle();
  const VkShaderModuleCreateInfo vertex{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(terminal_vertex_spirv),
      .pCode = terminal_vertex_spirv,
  };
  const VkShaderModuleCreateInfo fragment{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(terminal_fragment_spirv),
      .pCode = terminal_fragment_spirv,
  };
  check(vkCreateShaderModule(handle, &vertex, nullptr, &vertex_), "vkCreateShaderModule");
  check(vkCreateShaderModule(handle, &fragment, nullptr, &fragment_), "vkCreateShaderModule");
  const VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
  const std::array<VkVertexInputAttributeDescription, 4> attributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, x)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, u)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, r)},
      {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Instance, layer)},
  }};
  const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
      .pVertexAttributeDescriptions = attributes.data(),
  };
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
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  constexpr std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<std::uint32_t>(dynamic.size()),
      .pDynamicStates = dynamic.data(),
  };
  const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  const VkPipelineRenderingCreateInfo rendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &format,
  };
  constexpr auto all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  for (const auto pass : {Pass::Opaque, Pass::Blended, Pass::Inverted, Pass::Glyphs}) {
    const VkBool32 glyphs = pass == Pass::Glyphs ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entry{0, 0, sizeof(VkBool32)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(glyphs), &glyphs};
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_, .pName = "main", .pSpecializationInfo = &specialization},
    }};
    VkPipelineColorBlendAttachmentState attachment{
        .blendEnable = pass == Pass::Opaque ? VK_FALSE : VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = all,
    };
    if (pass == Pass::Inverted) {
      attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
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
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state,
        .layout = layout_,
    };
    check(vkCreateGraphicsPipelines(handle, VK_NULL_HANDLE, 1, &info, nullptr,
                                    &pipelines_[static_cast<std::size_t>(pass)]),
          "vkCreateGraphicsPipelines");
  }
}

void Gpu::set_font_size(unsigned size) {
  if (font_size_ == size) return;
  font_size_ = size;
  text_renderer_.configure(fonts_, size, font_weight_);
  cell_width_ = text_renderer_.width(); cell_height_ = text_renderer_.height();
  reset_atlas();
}

void Gpu::reset_atlas() {
  glyphs_.clear(); atlas_cursors_.fill({}); ++atlas_generation_;
  for (auto& pending : pending_glyphs_) pending.clear();
  cached_revision_ = 0;
}

void Gpu::resize(unsigned width, unsigned height, unsigned font_size) {
  set_font_size(font_size);
  if (width == width_ && height == height_) return;
  width_ = width; height_ = height;
  for (auto& buffer : buffers_) buffer->retired = true;
}

void Gpu::set_padding(unsigned horizontal, unsigned vertical) {
  horizontal = std::min(horizontal,width_ > cell_width_ ? (width_ - cell_width_) / 2 : 0);
  vertical = std::min(vertical,height_ > cell_height_ ? (height_ - cell_height_) / 2 : 0);
  if (horizontal == padding_x_ && vertical == padding_y_) return;
  padding_x_ = horizontal; padding_y_ = vertical; cached_revision_ = 0;
}

void Gpu::warm(zwayland::client::Proxy& dmabuf) {
  for (const auto style : {0,1,2,3}) for (char character = 33; character < 127; ++character)
    (void)glyph(std::string(1, character), style & 1, style & 2, 0xffffff);
  for (const auto role : {FontRole::Symbols, FontRole::Emoji}) {
    if ((role == FontRole::Symbols ? fonts_.symbols : fonts_.emoji).empty()) continue;
    std::string text; Text::append_utf8(text, role == FontRole::Symbols ? U'\ue0a0' : U'😀');
    (void)glyph(text, false, false, 0xffffff, false, false, role);
  }
  flush_glyphs();
  if (width_ && height_ && buffers_.empty())
    for (unsigned count = 0; count < 3; ++count) buffers_.push_back(create_buffer(dmabuf));
}

void Gpu::detach() {
  cached_revision_ = 0;
}

void Gpu::configure(FontFamilies fonts, unsigned weight, float rounding, float icon_scale,
                     Color background, std::array<float, 4> selection, float background_opacity,
                     std::optional<Color> selection_foreground, std::optional<Color> cursor_color, std::optional<Color> cursor_text) {
  if (background_opacity < 1 && format_ != DRM_FORMAT_ARGB8888)
    throw std::runtime_error("background opacity requires ARGB8888 DMA-BUF support");
  if (fonts != fonts_ || weight != font_weight_) font_size_ = 0;
  fonts_ = std::move(fonts); font_weight_ = weight; text_rounding_ = rounding; icon_scale_ = icon_scale;
  background_ = background; selection_color_ = selection; background_opacity_ = background_opacity; cached_revision_ = 0;
  selection_foreground_ = selection_foreground; cursor_color_ = cursor_color; cursor_text_ = cursor_text;
}

Gpu::Glyph Gpu::glyph(const std::string& text, bool bold, bool italic, std::uint32_t rgb, bool grid, bool separator, FontRole role) {
  std::string key = text;
  key.push_back('\0'); key.push_back(bold); key.push_back(italic); key.push_back(grid); key.push_back(separator);
  key.push_back(static_cast<char>(role));
  if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
  std::string colored_key = key; colored_key.push_back('\1'); colored_key.append(reinterpret_cast<const char*>(&rgb), sizeof(rgb));
  if (auto it = glyphs_.find(colored_key); it != glyphs_.end()) return it->second;
  const auto bitmap = text_renderer_.rasterize(text, bold, italic, rgb, grid, separator, role);
  const bool color = bitmap.color;
  Glyph result{};
  if (bitmap.width > 0 && bitmap.height > 0) {
    auto& atlas = atlas_cursors_[color ? 1 : 0];
    if (atlas.x + bitmap.width + 1 >= atlas_size) {
      atlas.x = 1; atlas.y += atlas.row + 1; atlas.row = 0;
    }
    if (atlas.y + bitmap.height + 1 >= atlas_size) {
      ++atlas.layer; atlas.x = atlas.y = 1; atlas.row = 0;
    }
    if (atlas.layer >= (color ? 1 : static_cast<int>(atlas_layers)) || glyphs_.size() >= 32768) reset_atlas();
    const auto bytes = static_cast<std::uint32_t>(bitmap.width) * (color ? 4U : 1U);
    pending_glyphs_[color ? 1 : 0].push_back({bitmap.pixels, {nullptr, bytes,
        static_cast<std::uint32_t>(atlas.x), static_cast<std::uint32_t>(atlas.y), static_cast<std::uint32_t>(bitmap.width),
        static_cast<std::uint32_t>(bitmap.height), static_cast<std::uint32_t>(atlas.layer)}});
    result = {float(atlas.x), float(atlas.y), float(bitmap.width) / bitmap.scale, float(bitmap.height) / bitmap.scale,
              float(bitmap.x), float(bitmap.y), float(atlas.layer), float(bitmap.width), float(bitmap.height), color};
    atlas.x += bitmap.width + 1; atlas.row = std::max(atlas.row, bitmap.height);
  }
  result.reference = bitmap.reference;
  glyphs_.emplace(color ? colored_key : key, result);
  return result;
}

void Gpu::flush_glyphs() {
  for (std::size_t color = 0; color < pending_glyphs_.size(); ++color) {
    auto& pending = pending_glyphs_[color];
    if (pending.empty()) continue;
    std::vector<zwwm::vlk::Image::Region> regions;
    regions.reserve(pending.size());
    for (auto& glyph : pending) {
      glyph.region.data = glyph.pixels.data();
      regions.push_back(glyph.region);
    }
    std::string error;
    if (!(color ? color_atlas_ : atlas_)->upload(regions, &error))
      throw std::runtime_error("cannot upload glyphs: " + error);
    pending.clear();
  }
}

std::unique_ptr<Gpu::Buffer> Gpu::create_buffer(zwayland::client::Proxy& dmabuf) {
  auto buffer = std::make_unique<Buffer>(); buffer->owner = this;
  buffer->bo = gbm_bo_create_with_modifiers2(gbm_, width_, height_, format_, modifiers_.data(), modifiers_.size(), GBM_BO_USE_RENDERING);
  if (buffer->bo == nullptr) throw std::runtime_error("cannot allocate a shared GPU framebuffer");
  const int planes = gbm_bo_get_plane_count(buffer->bo);
  if (planes < 1 || planes > 4) throw std::runtime_error("unsupported GPU plane count");
  const auto modifier = gbm_bo_get_modifier(buffer->bo);
  std::array<int, 4> fds{-1, -1, -1, -1};
  const auto close_fds = [&fds] { for (int& fd : fds) if (fd >= 0) { close(fd); fd = -1; } };
  zwwm::vlk::DmabufAttributes attributes{width_, height_, format_, {}};
  for (int plane = 0; plane < planes; ++plane) {
    fds[plane] = gbm_bo_get_fd_for_plane(buffer->bo, plane);
    if (fds[plane] < 0) {
      close_fds();
      throw std::runtime_error("cannot export a GPU buffer plane");
    }
    attributes.planes.push_back({fds[plane], gbm_bo_get_stride_for_plane(buffer->bo, plane),
                                 gbm_bo_get_offset(buffer->bo, plane), modifier});
  }
  std::string error;
  buffer->image = zwwm::vlk::Image::import_dmabuf(vulkan_, attributes, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &error);
  if (buffer->image == nullptr) {
    close_fds();
    throw std::runtime_error("cannot import the GPU framebuffer into Vulkan: " + error);
  }
  const auto params = p::zwp_linux_dmabuf_v1_create_params(display_, dmabuf.id);
  for (int plane = 0; plane < planes; ++plane)
    p::zwp_linux_buffer_params_v1_add(display_, params, fds[plane], plane,
      gbm_bo_get_offset(buffer->bo, plane), gbm_bo_get_stride_for_plane(buffer->bo, plane), modifier >> 32U, modifier);
  close_fds();
  const auto id = p::zwp_linux_buffer_params_v1_create_immed(display_, params, width_, height_, format_, 0);
  p::zwp_linux_buffer_params_v1_destroy(display_, params);
  buffer->proxy = display_.find_proxy(id);
  struct Release {
    Buffer* buffer;
    void release(zwayland::client::Proxy&) const {
      buffer->busy = false;
      if (buffer->owner->released) buffer->owner->released();
    }
  };
  buffer->proxy->set_observer(p::wl_buffer_observer(Release{buffer.get()}));
  return buffer;
}

Gpu::Glyph Gpu::fit_glyph(Glyph glyph, GlyphFit fit, unsigned cells, unsigned available) const {
  if (glyph.width <= 0 || glyph.height <= 0) return glyph;
  const float width = cells * cell_width_, height = cell_height_, span = available * cell_width_;
  const float ink_width = std::max(1.f, glyph.width - 2), ink_height = std::max(1.f, glyph.height - 2);
  if (fit == GlyphFit::Grid || fit == GlyphFit::Separator) {
    auto reference = glyph.reference;
    if (reference.width <= 0 || reference.height <= 0) reference = {0,0,float(cell_width_),float(cell_height_)};
    const float sx = width / reference.width, sy = height / reference.height;
    glyph.left = (glyph.left - reference.x) * sx; glyph.top = (glyph.top - reference.y) * sy;
    glyph.width *= sx; glyph.height *= sy; glyph.pixel_snap = true;
  } else if (fit == GlyphFit::Icon || fit == GlyphFit::Emoji) {
    const float scale = fit == GlyphFit::Emoji ? std::min(width * 0.95f / ink_width, height * 0.95f / ink_height) :
                        std::min({icon_scale_, span * 0.96f / ink_width, height * 0.85f / ink_height});
    glyph.left = std::max(0.f, std::floor((width - ink_width * scale) / 2)) - scale;
    glyph.top = std::floor((height - ink_height * scale) / 2) - scale;
    glyph.width *= scale; glyph.height *= scale;
  }
  const auto clip = [](float& origin, float& extent, float& sample, float& sample_extent, float limit) {
    const float first = std::max(0.f, origin), last = std::min(limit, origin + extent);
    if (last <= first) { extent = sample_extent = 0; return; }
    sample += (first - origin) / extent * sample_extent;
    sample_extent *= (last - first) / extent;
    origin = first; extent = last - first;
  };
  clip(glyph.left,glyph.width,glyph.x,glyph.sample_width,fit == GlyphFit::Icon || fit == GlyphFit::Text ? span : width);
  clip(glyph.top,glyph.height,glyph.y,glyph.sample_height,height);
  return glyph;
}

void Gpu::append_glyph(const Glyph& glyph, float x, float y, Color color) {
  if (glyph.width <= 0 || glyph.height <= 0) return;
  text_.push_back({x+glyph.left,y+glyph.top,glyph.width,glyph.height,
    glyph.x / atlas_size,glyph.y / atlas_size,(glyph.color ? -glyph.sample_width : glyph.sample_width) / atlas_size,
    glyph.sample_height / atlas_size,color.red / 255.f,color.green / 255.f,color.blue / 255.f,1,
    glyph.layer + (glyph.pixel_snap ? 16.f : 0.f)});
}

void Gpu::build_text(Terminal& terminal) {
  backgrounds_.clear(); text_.clear(); cursor_glyph_.reset();
  const auto cursor = terminal.cursor_position();
  for (int row = 0; row < terminal.rows(); ++row) {
    const auto selection = terminal.selection_range(row);
    for (int col = 0; col < terminal.columns(); ++col) {
      auto cell = terminal.cell(row, col);
      if (cell.width == 0 || cell.chars[0] == UINT32_MAX) continue;
      if (cell.attrs.reverse) std::swap(cell.fg, cell.bg);
      if (cell.attrs.hidden) cell.fg = cell.bg;
      if (cell.attrs.dim) { cell.fg.red /= 2; cell.fg.green /= 2; cell.fg.blue /= 2; }
      if (col >= selection.first && col < selection.second) {
        if (selection_foreground_) cell.fg = *selection_foreground_;
        else if (selection_color_[3] >= 0.999f) {
          const auto foreground = terminal.foreground_color();
          const auto luminance = [](Color color) { return color.red*0.2126f+color.green*0.7152f+color.blue*0.0722f; };
          const float selected = 255*(selection_color_[0]*0.2126f+selection_color_[1]*0.7152f+selection_color_[2]*0.0722f);
          cell.fg = std::abs(luminance(foreground)-selected) >= std::abs(luminance(background_)-selected) ? foreground : background_;
        } else {
          const float alpha = selection_color_[3];
          cell.fg = {static_cast<std::uint8_t>(std::lround(cell.fg.red*(1-alpha)+selection_color_[0]*255*alpha)),
                     static_cast<std::uint8_t>(std::lround(cell.fg.green*(1-alpha)+selection_color_[1]*255*alpha)),
                     static_cast<std::uint8_t>(std::lround(cell.fg.blue*(1-alpha)+selection_color_[2]*255*alpha))};
        }
      }
      const float x = col * cell_width_, y = row * cell_height_;
      const float width = std::max(1, int(cell.width)) * cell_width_;
      const float alpha = cell.bg_index == -1 && !cell.attrs.reverse ? background_opacity_ : 1.f;
      backgrounds_.push_back({x,y,width,float(cell_height_),0,0,0,0, cell.bg.red / 255.f,cell.bg.green / 255.f,cell.bg.blue / 255.f,alpha});
      const std::uint32_t foreground = (std::uint32_t(cell.fg.red) << 16U) | (std::uint32_t(cell.fg.green) << 8U) | cell.fg.blue;
      if (!cell.attrs.hidden && (cell.attrs.underline || cell.attrs.strike)) {
        const float line_y = cell.attrs.strike ? y + cell_height_ / 2 : y + cell_height_ - 2;
        backgrounds_.push_back({x,line_y,width,1,0,0,0,0,cell.fg.red / 255.f,cell.fg.green / 255.f,cell.fg.blue / 255.f,1});
      }
      if (cell.attrs.hidden || !cell.chars[0] || (cell.chars[0] == U' ' && !cell.chars[1])) continue;
      const auto character = cell.chars[0];
      const bool grid = (character >= 0x2500 && character <= 0x259f) || (character >= 0x2800 && character <= 0x28ff) ||
                        (character >= 0x1fb00 && character <= 0x1fbff) || (character >= 0x1cc00 && character <= 0x1cebf);
      const bool separator = character >= 0xe0b0 && character <= 0xe0d7;
      const bool icon = (character >= 0xe000 && character <= 0xf8ff) ||
                        (character >= 0xf0000 && character <= 0xffffd) ||
                        (character >= 0x100000 && character <= 0x10fffd);
      const bool text_presentation = std::find(cell.chars.begin(), cell.chars.end(), char32_t{0xfe0e}) != cell.chars.end();
      const bool emoji = !text_presentation && ((character >= 0x1f000 && character <= 0x1faff) ||
        std::find(cell.chars.begin(), cell.chars.end(), char32_t{0xfe0f}) != cell.chars.end() ||
        std::find(cell.chars.begin(), cell.chars.end(), char32_t{0x20e3}) != cell.chars.end());
      std::string cluster;
      for (const auto codepoint : cell.chars) { if (!codepoint) break; Text::append_utf8(cluster, codepoint); }
      unsigned available = cell.width;
      if (!grid && !separator && !emoji && col + cell.width < terminal.columns()) {
        const auto next = terminal.cell(row, col + cell.width);
        const auto next_background = next.attrs.reverse ? next.fg : next.bg;
        if ((next.chars[0] == 0 || next.chars[0] == U' ') && next.width == 1 && next_background == cell.bg) ++available;
      }
      const auto fit = grid ? GlyphFit::Grid : separator ? GlyphFit::Separator : icon ? GlyphFit::Icon : emoji ? GlyphFit::Emoji : GlyphFit::Text;
      const auto role = grid || separator || icon ? FontRole::Symbols : emoji ? FontRole::Emoji : FontRole::Mono;
      const auto before = text_.size();
      append_glyph(fit_glyph(glyph(cluster,cell.attrs.bold,grid ? false : cell.attrs.italic,foreground,grid,separator,role),fit,cell.width,available),x,y,cell.fg);
      if (row == cursor.row && col == cursor.col && text_.size() > before) cursor_glyph_ = text_.back();
    }
  }
}

bool Gpu::draw(Terminal& terminal, zwayland::client::Proxy& surface, zwayland::client::Proxy& dmabuf, bool cursor) {
  vulkan_->collect();
  std::erase_if(buffers_, [](const auto& buffer) { return buffer->retired && !buffer->busy; });
  Buffer* target = nullptr;
  unsigned current = 0;
  for (auto& buffer : buffers_) if (!buffer->retired) {
    ++current; if (!buffer->busy) target = buffer.get();
  }
  if (target == nullptr && current < 3) {
    buffers_.push_back(create_buffer(dmabuf)); target = buffers_.back().get();
  }
  if (target == nullptr) return false;
  if (cached_revision_ != terminal.revision() || cached_selection_revision_ != terminal.selection_revision()) {
    for (int attempt = 0; ; ++attempt) {
      const auto generation = atlas_generation_;
      build_text(terminal);
      if (generation == atlas_generation_) break;
      if (attempt >= 2) throw std::runtime_error("visible text exceeds the GPU cache");
    }
    cached_revision_ = terminal.revision();
    cached_selection_revision_ = terminal.selection_revision();
  }
  flush_glyphs();

  selection_rects_.clear();
  if (terminal.has_selection()) for (int row = 0; row < terminal.rows(); ++row) {
    const auto [first, last] = terminal.selection_range(row);
    if (last > first) selection_rects_.push_back({first * float(cell_width_), row * float(cell_height_),
      (last - first) * float(cell_width_), float(cell_height_),0,0,0,0,
      selection_color_[0],selection_color_[1],selection_color_[2],selection_color_[3]});
  }
  std::vector<Instance> cursor_quad, cursor_text;
  Pass cursor_pass = Pass::Blended;
  VkRect2D cursor_cell{};
  const auto position = terminal.cursor_position();
  const auto style = terminal.cursor_style();
  if ((cursor || !style.blink) && !terminal.has_selection() && terminal.cursor_at(position.row, position.col)) {
    const float width = std::max(1, int(terminal.cell(position.row, position.col).width)) * cell_width_;
    const float thickness = std::max(1.f, std::round(cell_height_ * 0.09f));
    const auto color = cursor_color_.value_or(terminal.foreground_color());
    Instance cursor_instance{position.col * float(cell_width_), position.row * float(cell_height_), width, float(cell_height_),
                             0,0,0,0,color.red / 255.f,color.green / 255.f,color.blue / 255.f,1};
    if (style.shape == CursorShape::Block) {
      if (!cursor_color_ && !cursor_text_) {
        cursor_pass = Pass::Inverted;
        cursor_instance.r = cursor_instance.g = cursor_instance.b = 1;
      }
    } else if (style.shape == CursorShape::Beam) cursor_instance.width = thickness;
    else { cursor_instance.y += cell_height_ - thickness; cursor_instance.height = thickness; }
    cursor_quad.push_back(cursor_instance);
    if (style.shape == CursorShape::Block && (cursor_color_ || cursor_text_) && cursor_glyph_) {
      auto glyph = *cursor_glyph_; const auto foreground = cursor_text_.value_or(background_);
      glyph.r = foreground.red/255.f; glyph.g = foreground.green/255.f; glyph.b = foreground.blue/255.f;
      cursor_text.push_back(glyph);
      cursor_cell = {{static_cast<std::int32_t>(padding_x_ + position.col * cell_width_),
                      static_cast<std::int32_t>(padding_y_ + position.row * cell_height_)},
                     {static_cast<std::uint32_t>(width), cell_height_}};
    }
  }

  struct Range { std::uint32_t first = 0, count = 0; };
  std::vector<Instance> instances;
  const auto append = [&instances](const std::vector<Instance>& batch) {
    const Range range{static_cast<std::uint32_t>(instances.size()), static_cast<std::uint32_t>(batch.size())};
    instances.insert(instances.end(), batch.begin(), batch.end());
    return range;
  };
  const Range backgrounds = append(backgrounds_), selections = append(selection_rects_), text = append(text_),
              cursor_range = append(cursor_quad), cursor_glyph = append(cursor_text);
  std::string error;
  std::shared_ptr<zwwm::vlk::StagingBuffer> vertices;
  if (!instances.empty()) {
    vertices = vulkan_->staging(instances.size() * sizeof(Instance), &error);
    if (vertices == nullptr) throw std::runtime_error("cannot allocate terminal vertices: " + error);
    std::memcpy(vertices->mapping, instances.data(), instances.size() * sizeof(Instance));
  }

  auto commands = vulkan_->begin(&error);
  if (!commands) throw std::runtime_error("cannot record a terminal frame: " + error);
  const VkCommandBuffer cb = commands.get();
  auto& image = *target->image;
  // Wait for the compositor to finish reading this buffer's previous frame.
  (void)commands.wait_sync_file(image.export_sync_file(true), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
  image.transition(cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
  VkRenderingAttachmentInfo attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = image.view(),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  attachment.clearValue.color = {{background_.red / 255.f * background_opacity_, background_.green / 255.f * background_opacity_,
                                  background_.blue / 255.f * background_opacity_, background_opacity_}};
  const VkRenderingInfo rendering{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, {width_, height_}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &attachment,
  };
  vkCmdBeginRendering(cb, &rendering);
  const VkViewport viewport{float(padding_x_), float(padding_y_), float(content_width()), float(content_height()), 0, 1};
  const VkRect2D content{{static_cast<std::int32_t>(padding_x_), static_cast<std::int32_t>(padding_y_)},
                         {content_width(), content_height()}};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  vkCmdSetScissor(cb, 0, 1, &content);
  const Push push{{float(content_width()), float(content_height())}, text_rounding_};
  vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
  if (vertices != nullptr) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vertices->buffer, &offset);
  }
  const auto run = [&](Pass pass, Range range) {
    if (range.count == 0) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[static_cast<std::size_t>(pass)]);
    vkCmdDraw(cb, 6, range.count, 0, range.first);
  };
  run(Pass::Opaque, backgrounds);
  run(Pass::Blended, selections);
  run(Pass::Glyphs, text);
  run(cursor_pass, cursor_range);
  if (cursor_glyph.count != 0) {
    vkCmdSetScissor(cb, 0, 1, &cursor_cell);
    run(Pass::Glyphs, cursor_glyph);
  }
  vkCmdEndRendering(cb);
  image.release(cb);
  commands.keep(target->image);
  commands.keep(atlas_);
  commands.keep(color_atlas_);
  commands.keep(vertices);
  int sync_file = -1;
  if (!vulkan_->submit(commands, &sync_file, false, &error)) throw std::runtime_error("cannot submit a terminal frame: " + error);
  // The compositor waits on this fence implicitly through the DMA-BUF.
  if (sync_file >= 0) {
    if (!image.import_sync_file(sync_file, true)) vulkan_->wait_idle();
    close(sync_file);
  }
  p::wl_surface_attach(display_, surface.id, target->proxy, 0, 0);
  p::wl_surface_damage_buffer(display_, surface.id, 0, 0, width_, height_);
  target->busy = true;
  return true;
}

}  // namespace zterm
