#version 450
#include "zwwm.glsl"

// 1 = 4, 2 = 8, 3 = 16 sampled paths.
layout(constant_id = 0) const int glass_quality = 2;

#include "glass_physics.glsl"

void main() {
  float coverage = zwwm_coverage(zwwm_rounded_rect_distance(zwwm_local, zwwm.draw_rect.zw, zwwm.corner_radius));
  if ((zwwm.flags & ZWWM_HAS_CLIP) != 0u)
    coverage *= zwwm_coverage(zwwm_rounded_rect_distance(zwwm_position - zwwm.clip_rect.xy, zwwm.clip_rect.zw,
                                                         zwwm.clip_radius));
  zwwm_out = physically_based_glass(zwwm_screen_uv()) * (coverage * zwwm.opacity);
}
