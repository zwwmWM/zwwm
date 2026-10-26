#pragma once

#include "zwwm/renderer/scene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace zwwm::vlk { class Image; }

namespace zwwm::renderer {

using Texture = vlk::Image;
using TextureRef = std::shared_ptr<Texture>;

enum class ShaderRole : std::uint8_t { builtin, window, border, background, shadow };
using ShaderValue = std::variant<bool, std::int32_t, std::array<float, 4>,
                                  std::vector<std::int32_t>,
                                  std::vector<std::array<float, 4>>>;

struct NamedShaderSource {
  std::string name;
  ShaderRole role = ShaderRole::window;
  std::string source;
  std::vector<std::pair<std::string, ShaderValue>> values;
};

struct ShaderSources {
  std::vector<NamedShaderSource> programs;
  std::string window;
  std::string border;
  std::string background;
  std::string shadow;
  std::string overview_backdrop_image;
};

// Pixel-center geometry shared by ring planning tests and the shader contract.
[[nodiscard]] bool border_ring_contains(Size size, float corner_radius, float border_width,
                                        float x, float y);

struct FloatRect {
  struct Point { double x = 0, y = 0; } origin;
  struct Size { double width = 0, height = 0; } size;
  FloatRect() = default;
  FloatRect(Point p, Size s) : origin(p), size(s) {}
  FloatRect(Rect r) : origin{static_cast<double>(r.origin.x), static_cast<double>(r.origin.y)},
                      size{static_cast<double>(r.size.width), static_cast<double>(r.size.height)} {}
  bool empty() const { return size.width <= 0 || size.height <= 0; }
};

struct DrawGeometry {
  FloatRect bounds, toplevel;
  std::optional<FloatRect> clip, texture;
};

struct DrawCall {
  DrawCall() = default;
  DrawCall(NodeId node_id_value, Rect bounds_value, float opacity_value, std::array<float, 4> color_value,
           TextureRef texture_value, float corner_radius_value = 0.0F, float border_width_value = 0.0F,
           std::array<float, 4> source_uv_value = {0.0F, 0.0F, 1.0F, 1.0F},
           std::optional<Rect> clip_bounds_value = std::nullopt, float clip_radius_value = 0.0F)
      : node_id(node_id_value),
        bounds(bounds_value),
        opacity(opacity_value),
        color(color_value),
        texture(std::move(texture_value)),
        corner_radius(corner_radius_value),
        border_width(border_width_value),
        source_uv(source_uv_value),
        clip_bounds(clip_bounds_value),
        clip_radius(clip_radius_value) {}

  NodeId node_id = 0;
  Rect bounds;
  float opacity = 1.0F;
  std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
  // Null draws solid color geometry.
  TextureRef texture;
  float corner_radius = 0.0F;
  // Nonzero draws a rounded ring instead of a filled rectangle.
  float border_width = 0.0F;
  // Normalized source rectangle: left, top, right, bottom.
  std::array<float, 4> source_uv{0.0F, 0.0F, 1.0F, 1.0F};
  // Screen-space destination for source_uv, independent of decoration bounds.
  std::optional<Rect> texture_bounds;
  float texture_corner_radius = 0.0F;
  std::optional<Rect> clip_bounds;
  float clip_radius = 0.0F;
  bool strict_clip = false;
  float background_blur_radius = 0.0F;
  std::uint32_t background_blur_passes = 3;
  float background_blur_brightness = 1.0F;
  float background_blur_contrast = 1.0F;
  float background_blur_saturation = 1.0F;
  float background_blur_noise = 0.0F;
  // Use the client texture's alpha as backdrop coverage for shaped layer surfaces.
  bool background_mask_texture = false;
  float background_mask_alpha_threshold = 0.0F;
  bool glass = false;
  std::uint32_t glass_quality = 2;
  float glass_refraction = 0.0F;
  float glass_dispersion = 0.0F;
  float glass_frost = 0.0F;
  float glass_time = 0.0F;
  float glass_ior = 1.5F;
  float glass_thickness = 12.0F;
  float glass_roughness = 0.18F;
  std::array<float, 4> glass_tint{0.0F, 0.0F, 0.0F, 0.0F};
  std::uint32_t texture_transform = 0;
  bool opaque = false;
  ShaderRole shader_role = ShaderRole::builtin;
  std::string shader_name;
  NodeId toplevel_id = 0;
  Rect toplevel_bounds;
  std::uint32_t toplevel_state = 0;
  // Non-overlapping tiled toplevels may share one backdrop.
  bool shared_backdrop = false;
  float shader_time = 0.0F;
  std::optional<DrawGeometry> geometry;
};

struct FramePlan {
  std::vector<Rect> damage;
  std::vector<DrawCall> draws;
  bool draw_background = false;
  float shader_time = 0.0F;
  bool overview_backdrop = false;
  float overview_backdrop_opacity = 1.0F;
  float overview_backdrop_blur_radius = 0.0F;
  std::uint32_t overview_backdrop_blur_passes = 3;
};

}  // namespace zwwm::renderer
