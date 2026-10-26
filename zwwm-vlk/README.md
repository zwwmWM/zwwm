# zwwm-vlk

Small Vulkan layer shared by the zwwm renderer and backends. It replaces the
former `zwwm-egl` presenter.

- `Device`: Vulkan 1.3 instance and device selection (matched to a DRM node for
  direct sessions), command submission with keep-alive tracking, staging
  buffers, and the DMA-BUF format/modifier table.
- `Image`: owned images, DMA-BUF imports (including disjoint planes), layout and
  foreign queue-family ownership tracking, uploads, readback, and sync_file
  import/export for implicit synchronization.
- `Window`: presents a render target on a parent Wayland surface through
  triple-buffered wl_shm copies for nested sessions.
- `DmabufAttributes`: Vulkan-free DMA-BUF description used by the protocol
  layer.

Requirements: Vulkan 1.3 with dynamic rendering, synchronization2, and
demote-to-helper-invocation. DMA-BUF import additionally needs
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, and
`VK_EXT_image_drm_format_modifier`.
