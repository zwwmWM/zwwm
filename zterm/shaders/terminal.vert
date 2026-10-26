#version 450

layout(location = 0) in vec4 rect;     // content pixels: x, y, width, height
layout(location = 1) in vec4 texrect;  // atlas uv; negative width marks color glyphs
layout(location = 2) in vec4 color;
layout(location = 3) in float layer;   // atlas layer; +16 requests pixel snapping

layout(push_constant) uniform Push {
  vec2 viewport;
  float text_rounding;
} push;

layout(location = 0) out vec3 uv;
layout(location = 1) out vec4 tint;
layout(location = 2) flat out float colored;
layout(location = 3) flat out float pixel_snap;
layout(location = 4) flat out vec4 uv_bounds;

const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));

void main() {
  vec2 q = corners[gl_VertexIndex];
  vec2 pixel = rect.xy + q * rect.zw;
  gl_Position = vec4(pixel / push.viewport * 2.0 - 1.0, 0.0, 1.0);
  pixel_snap = layer >= 16.0 ? 1.0 : 0.0;
  uv = vec3(texrect.xy + q * abs(texrect.zw), layer - pixel_snap * 16.0);
  uv_bounds = vec4(texrect.xy, texrect.xy + abs(texrect.zw));
  tint = color;
  colored = texrect.z < 0.0 ? 1.0 : 0.0;
}
