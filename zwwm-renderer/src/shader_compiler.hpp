#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zwwm::renderer::detail {

namespace gl {
inline constexpr std::uint32_t kInt = 0x1404;
inline constexpr std::uint32_t kUnsignedInt = 0x1405;
inline constexpr std::uint32_t kFloat = 0x1406;
inline constexpr std::uint32_t kFloatVec2 = 0x8B50;
inline constexpr std::uint32_t kFloatVec3 = 0x8B51;
inline constexpr std::uint32_t kFloatVec4 = 0x8B52;
inline constexpr std::uint32_t kIntVec2 = 0x8B53;
inline constexpr std::uint32_t kIntVec3 = 0x8B54;
inline constexpr std::uint32_t kIntVec4 = 0x8B55;
inline constexpr std::uint32_t kBool = 0x8B56;
}  // namespace gl

struct ConfigMember {
  std::string name;
  std::uint32_t type = 0;
  std::uint32_t offset = 0;
  std::uint32_t array_size = 1;
  std::uint32_t array_stride = 0;
};

struct ConfigBlock {
  std::uint32_t size = 0;
  std::vector<ConfigMember> members;
};

struct CompiledShader {
  std::vector<std::uint32_t> spirv;
  std::optional<ConfigBlock> config;
};

// Descriptors outside the zwwm.glsl ABI and the set 1 config block are rejected.
[[nodiscard]] bool compile_vertex(std::string_view source, CompiledShader* shader, std::string* error);
[[nodiscard]] bool compile_fragment(std::string_view source, CompiledShader* shader, std::string* error);

}  // namespace zwwm::renderer::detail
