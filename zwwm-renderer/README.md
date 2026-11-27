# zwwm-renderer

Scene ordering, damage planning and Vulkan drawing.

`Scene` holds surfaces and decorations. `VulkanRenderer::build_frame()` turns
damaged nodes into draw calls and `render()` records them; everything between
`begin()` and `submit()` is one GPU submission.

- shm and DMA-BUF textures, premultiplied alpha
- Antialiased rounded clipping and borders
- Dual-Kawase blur and glass
- Implicit sync with DMA-BUF producers and KMS

Shaders are Vulkan GLSL 4.50 built on `shaders/zwwm.glsl`: one std140
`ZwwmDraw` block per draw, a bindless `zwwm_textures[]` table, and an optional
per-shader config block (set 1) filled from `config.zw`.
