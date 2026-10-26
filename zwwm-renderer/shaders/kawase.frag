#version 450
#include "zwwm.glsl"

const int KAWASE_DOWNSAMPLE = 0;
const int KAWASE_UPSAMPLE = 1;
const int KAWASE_FINALIZE = 2;
layout(constant_id = 0) const int kawase_mode = KAWASE_DOWNSAMPLE;

uvec2 grain_hash22(uvec2 p) {
  p = uvec2(p.x * 1597334677U, p.y * 3812015801U);
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  p *= 0x45d9f3bU;
  p ^= p >> 16U;
  return p;
}

float grain_value(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  uvec2 h00 = grain_hash22(uvec2(i));
  uvec2 h10 = grain_hash22(uvec2(i) + uvec2(1U, 0U));
  uvec2 h01 = grain_hash22(uvec2(i) + uvec2(0U, 1U));
  uvec2 h11 = grain_hash22(uvec2(i) + uvec2(1U, 1U));
  float v00 = float((h00.x ^ h00.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v10 = float((h10.x ^ h10.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v01 = float((h01.x ^ h01.y) & 0xFFFFU) / 32768.0 - 1.0;
  float v11 = float((h11.x ^ h11.y) & 0xFFFFU) / 32768.0 - 1.0;
  return mix(mix(v00, v10, u.x), mix(v01, v11, u.x), u.y);
}

// Coarse noise varies the grain density without becoming a color offset.
float grain_noise(vec2 point) {
  float envelope = 0.55 + 0.45 * (grain_value(point * 0.035) * 0.5 + 0.5);
  vec2 rotated = mat2(0.8, -0.6, 0.6, 0.8) * point;
  float detail = grain_value(point * 1.7) * 0.7 + grain_value(rotated * 3.9) * 0.3;
  return clamp(detail, -1.0, 1.0) * envelope * 0.5;
}

vec4 source(vec2 uv) { return zwwm_window(uv); }

void main() {
  vec2 step_size = zwwm.texel_size * zwwm.sample_offset;
  vec2 uv = zwwm_uv;
  vec4 color;
  if (kawase_mode == KAWASE_DOWNSAMPLE) {
    color = source(uv) * 4.0;
    color += source(uv + vec2(step_size.x, step_size.y));
    color += source(uv + vec2(-step_size.x, step_size.y));
    color += source(uv + vec2(step_size.x, -step_size.y));
    color += source(uv - step_size);
    color /= 8.0;
  } else {
    color = source(uv + vec2(step_size.x, 0.0)) * 2.0;
    color += source(uv - vec2(step_size.x, 0.0)) * 2.0;
    color += source(uv + vec2(0.0, step_size.y)) * 2.0;
    color += source(uv - vec2(0.0, step_size.y)) * 2.0;
    color += source(uv + step_size);
    color += source(uv - step_size);
    color += source(uv + vec2(step_size.x, -step_size.y));
    color += source(uv + vec2(-step_size.x, step_size.y));
    color /= 12.0;
  }
  if (kawase_mode == KAWASE_FINALIZE) {
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    color.rgb = mix(vec3(luminance), color.rgb, zwwm.saturation);
    color.rgb = (color.rgb - vec3(0.5)) * zwwm.contrast + vec3(0.5);
    color.rgb *= zwwm.brightness;
    color.rgb += vec3(grain_noise(gl_FragCoord.xy) * zwwm.noise);
    color.rgb = clamp(color.rgb, 0.0, 1.0);
  }
  zwwm_out = color;
}
