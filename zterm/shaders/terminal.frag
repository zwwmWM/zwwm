#version 450

layout(constant_id = 0) const bool glyphs = false;
const float atlas_size = 2048.0;

layout(set = 0, binding = 0) uniform sampler2DArray atlas;
layout(set = 0, binding = 1) uniform sampler2DArray color_atlas;

layout(push_constant) uniform Push {
  vec2 viewport;
  float text_rounding;
} push;

layout(location = 0) in vec3 uv;
layout(location = 1) in vec4 tint;
layout(location = 2) flat in float colored;
layout(location = 3) flat in float pixel_snap;
layout(location = 4) flat in vec4 uv_bounds;
layout(location = 0) out vec4 output_color;

vec3 coordinates(vec2 point) {
  vec2 inset = vec2(0.5 / atlas_size);
  vec2 first = uv_bounds.xy + inset, last = uv_bounds.zw - inset;
  return vec3(clamp(point, min(first, last), max(first, last)), uv.z);
}

float mask(vec2 point) { return texture(atlas, coordinates(point)).r; }

void main() {
  if (!glyphs) {
    output_color = vec4(tint.rgb * tint.a, tint.a);
    return;
  }
  if (colored > 0.5) {
    output_color = texture(color_atlas, coordinates(uv.xy)) * tint.a;
    return;
  }
  vec2 dx = dFdx(uv.xy), dy = dFdy(uv.xy);
  float coverage;
  if (pixel_snap > 0.5) {
    dx *= 0.25; dy *= 0.25;
    coverage = max(max(mask(uv.xy - dx - dy), mask(uv.xy + dx - dy)),
                   max(mask(uv.xy - dx + dy), mask(uv.xy + dx + dy)));
  } else if (push.text_rounding > 0.0) {
    vec2 x = dx * push.text_rounding * 0.8;
    vec2 y = dy * push.text_rounding * 0.8;
    vec2 d1 = (x + y) * 0.70710678;
    vec2 d2 = (x - y) * 0.70710678;
    coverage = mask(uv.xy) * 0.25 +
      (mask(uv.xy + x) + mask(uv.xy - x) + mask(uv.xy + y) + mask(uv.xy - y)) * 0.125 +
      (mask(uv.xy + d1) + mask(uv.xy - d1) + mask(uv.xy + d2) + mask(uv.xy - d2)) * 0.0625;
  } else {
    dx *= 0.25; dy *= 0.25;
    coverage = (mask(uv.xy - dx - dy) + mask(uv.xy + dx - dy) + mask(uv.xy - dx + dy) + mask(uv.xy + dx + dy)) * 0.25;
  }
  coverage = pow(clamp((coverage - 0.5) * 1.04 + 0.5, 0.0, 1.0), 0.95);
  float alpha = tint.a * coverage;
  output_color = vec4(tint.rgb * alpha, alpha);
}
