#version 450
#include "zwwm.glsl"

void main() {
  bool background_only = (zwwm.flags & ZWWM_BACKGROUND_ONLY) != 0u;
  bool has_texture_rect = (zwwm.flags & ZWWM_HAS_TEXTURE_RECT) != 0u;
  vec4 color = background_only ? zwwm_blurred_backdrop(zwwm_screen_uv()) * zwwm.color.a
                               : vec4(zwwm.color.rgb * zwwm.color.a, zwwm.color.a);
  if ((zwwm.flags & ZWWM_HAS_TEXTURE) != 0u) {
    vec4 surface = zwwm_window(has_texture_rect ? zwwm_texture_rect_uv() : zwwm_uv);
    if (background_only) color *= surface.a > zwwm.mask_threshold ? surface.a : 0.0;
    else color *= surface;
  }

  float coverage = zwwm_coverage(zwwm_rounded_rect_distance(zwwm_local, zwwm.draw_rect.zw, zwwm.corner_radius));
  if (has_texture_rect && !background_only && zwwm.border_width <= 0.0)
    coverage = zwwm_coverage(zwwm_rounded_rect_distance(zwwm_position - zwwm.texture_rect.xy, zwwm.texture_rect.zw,
                                                        zwwm.texture_corner_radius));
  if ((zwwm.flags & ZWWM_HAS_CLIP) != 0u) {
    vec3 input_position = vec3(zwwm_local / max(zwwm.draw_rect.zw, vec2(1.0)), 1.0);
    vec2 clip = vec2(dot(zwwm.input_to_clip_0.xyz, input_position),
                     dot(zwwm.input_to_clip_1.xyz, input_position)) * zwwm.clip_rect.zw;
    float clip_distance = zwwm_rounded_rect_distance(clip, zwwm.clip_rect.zw, zwwm.clip_radius);
    if ((zwwm.flags & ZWWM_STRICT_CLIP) != 0u && clip_distance > 0.0) discard;
    coverage = min(coverage, zwwm_coverage(clip_distance));
  }
  if (zwwm.border_width > 0.0) {
    // The window's edge, so both antialiasing ramps are complementary.
    vec2 inner_size = zwwm.draw_rect.zw - vec2(2.0 * zwwm.border_width);
    float inner = has_texture_rect
        ? zwwm_rounded_rect_distance(zwwm_position - zwwm.texture_rect.xy, zwwm.texture_rect.zw, zwwm.texture_corner_radius)
        : zwwm_rounded_rect_distance(zwwm_local - vec2(zwwm.border_width), inner_size,
                                     max(zwwm.corner_radius - zwwm.border_width, 0.0));
    if (has_texture_rect || (inner_size.x > 0.0 && inner_size.y > 0.0))
      coverage = max(coverage - zwwm_coverage(inner), 0.0);
  }
  zwwm_out = color * coverage;
}
