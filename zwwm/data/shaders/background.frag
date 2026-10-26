#version 450
#include "zwwm.glsl"

void main() {
  vec2 uv = zwwm_local / max(zwwm.draw_rect.zw, vec2(1.0));
  float output_aspect = zwwm.draw_rect.z / max(zwwm.draw_rect.w, 1.0);
  vec2 image_size = vec2(zwwm_background_size());
  float image_aspect = image_size.x / max(image_size.y, 1.0);
  if (output_aspect > image_aspect)
    uv.y = 0.5 + (uv.y - 0.5) * image_aspect / output_aspect;
  else
    uv.x = 0.5 + (uv.x - 0.5) * output_aspect / image_aspect;
  zwwm_out = zwwm_background(uv);
}
