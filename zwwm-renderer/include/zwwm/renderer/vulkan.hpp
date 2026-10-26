#pragma once

#include "zwwm/renderer/dmabuf.hpp"
#include "zwwm/renderer/frame.hpp"
#include "zwwm/renderer/scene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zwwm::vlk { class Device; }

namespace zwwm::renderer {

class VulkanRenderer {
 public:
  VulkanRenderer();
  ~VulkanRenderer();
  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  [[nodiscard]] bool initialize(std::shared_ptr<vlk::Device> device);
  // Compiles a complete candidate set without changing the active programs.
  [[nodiscard]] bool prepare_shaders(const ShaderSources& sources);
  void commit_shaders();
  void discard_shaders();
  void shutdown();
  [[nodiscard]] bool ready() const;
  [[nodiscard]] const std::string& last_error() const;
  [[nodiscard]] const std::shared_ptr<vlk::Device>& device() const;

  [[nodiscard]] bool bind_texture(TextureHandle handle, TextureRef texture);
  void unbind_texture(TextureHandle handle);

  // Reallocates `texture` when absent, imported, or differently sized.
  [[nodiscard]] TextureRef upload(TextureRef texture, const std::uint32_t* pixels, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t row_pixels,
                                  std::optional<Rect> damage = std::nullopt);
  [[nodiscard]] TextureRef import_dmabuf(const DmabufAttributes& attributes);
  // Exact import set for linux-dmabuf advertisement.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>> dmabuf_formats() const;
  [[nodiscard]] TextureRef snapshot(const TextureRef& source);
  [[nodiscard]] TextureRef create_target(Size size);

  // render() and copy() outside begin()/submit() submit immediately.
  [[nodiscard]] bool begin();
  [[nodiscard]] bool recording() const;
  [[nodiscard]] bool render(const FramePlan& frame, const TextureRef& target,
                            std::optional<std::array<float, 4>> clear = std::nullopt);
  [[nodiscard]] bool copy(const TextureRef& source, const TextureRef& target);
  [[nodiscard]] bool submit(bool wait = false);

  // Renders an isolated frame into top-down XRGB8888 CPU storage.
  [[nodiscard]] bool render_pixels(const FramePlan& frame, Size target_size, std::vector<std::uint32_t>* pixels);
  [[nodiscard]] bool read_pixels(const TextureRef& texture, std::vector<std::uint32_t>* pixels);
  [[nodiscard]] FramePlan build_frame(const Scene& scene, const DamageRegion& damage) const;

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::string last_error_;
};

}  // namespace zwwm::renderer
