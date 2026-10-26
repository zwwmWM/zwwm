# zwwm-renderer

`zwwm-renderer` owns backend-neutral scene ordering, damage planning, and
Vulkan drawing. It has no wlroots dependency.

`Scene` stores visible surfaces and compositor decorations.
`VulkanRenderer::build_frame()` converts damaged scene nodes into ordered draw
calls, and `render()` records them with dynamic rendering. Work between
`begin()` and `submit()` becomes one GPU submission; `render()` and `copy()`
outside a recording submit on their own.

The renderer supports:

- Uploaded shm textures, imported DMA-BUFs, and solid compositor geometry
- Premultiplied-alpha blending
- Antialiased rounded clipping
- Decoration borders composed from ordered scene nodes
- Offscreen scene rendering, dual-Kawase backdrop blur, and glass
- Implicit synchronization with DMA-BUF producers and KMS

All shaders are Vulkan GLSL 4.50 against `shaders/zwwm.glsl`, compiled to
SPIR-V with glslang when shaders load:

- One `ZwwmDraw` std140 block per draw, written to a ring buffer and bound with
  a dynamic offset; a single vertex shader builds every quad from it.
- A bindless `sampler2D zwwm_textures[]` table written once per frame; draws
  carry texture indices.
- An optional per-program config block (set 1) filled once from `config.zw`.
- Specialization constants select Kawase pass modes and glass sample counts.
- Native top-left coordinates; snapshots are plain top-down image copies.

Textures are `std::shared_ptr<vlk::Image>`; submissions keep everything they
reference alive until the GPU retires them.
