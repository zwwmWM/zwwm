# zwwm-vlk

Vulkan layer shared by the renderer and backends.

- `Device`: instance/device selection (matched to the DRM node), submission,
  staging, DMA-BUF format table
- `Image`: owned and imported DMA-BUF images, uploads, readback, sync_file
  import/export
- `Window`: nested-session presentation through wl_shm
- `DmabufAttributes`: Vulkan-free DMA-BUF description for the protocol layer

Needs Vulkan 1.3 with dynamic rendering and synchronization2; DMA-BUF import
also needs `VK_EXT_external_memory_dma_buf` and
`VK_EXT_image_drm_format_modifier`.
