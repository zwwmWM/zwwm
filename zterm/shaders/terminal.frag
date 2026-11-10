#version 450

layout(constant_id = 0) const bool glyphs = false;
const float atlas_size = 2048.0;

layout(set = 0, binding = 0) uniform sampler2DArray atlas;
layout(set = 0, binding = 1) uniform sampler2DArray color_atlas;

layout(push_constant) uniform Push {
  vec2 viewport;
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
  } else if (abs(abs(dx.x) * atlas_size - 1.0) < 0.001 && abs(abs(dy.y) * atlas_size - 1.0) < 0.001) {
    // One texel per pixel: read it exactly, any filter here only blurs.
    vec2 texel = clamp(floor(uv.xy * atlas_size), uv_bounds.xy * atlas_size, uv_bounds.zw * atlas_size - 1.0);
    coverage = texelFetch(atlas, ivec3(texel, uv.z), 0).r;
  } else {
    dx *= 0.25; dy *= 0.25;
    coverage = (mask(uv.xy - dx - dy) + mask(uv.xy + dx - dy) + mask(uv.xy - dx + dy) + mask(uv.xy + dx + dy)) * 0.25;
  }
  coverage = pow(clamp((coverage - 0.5) * 1.04 + 0.5, 0.0, 1.0), 0.95);
  float alpha = tint.a * coverage;
  output_color = vec4(tint.rgb * alpha, alpha);
}
