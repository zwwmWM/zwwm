#version 450
#include "zwwm.glsl"

layout(location = 0) out vec2 zwwm_uv;
layout(location = 1) out vec2 zwwm_local;
layout(location = 2) out vec2 zwwm_position;

const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
  vec2 corner = corners[gl_VertexIndex];
  vec2 pixel = zwwm.draw_rect.xy + corner * zwwm.draw_rect.zw;
  gl_Position = vec4(pixel / max(zwwm.output_size, vec2(1.0)) * 2.0 - 1.0, 0.0, 1.0);
  zwwm_uv = mix(zwwm.source_uv.xy, zwwm.source_uv.zw, zwwm_transform(corner));
  zwwm_local = corner * zwwm.draw_rect.zw;
  zwwm_position = pixel;
}
