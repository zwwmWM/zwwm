#include "shader_compiler.hpp"
#include "shaders.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace zwwm::renderer::detail {
namespace {

constexpr const char* kDrawBlock = "ZwwmDraw";

void initialize_glslang() {
  static std::once_flag once;
  std::call_once(once, [] { (void)glslang::InitializeProcess(); });
}

// Filesystem includes are not allowed.
class Includer final : public glslang::TShader::Includer {
 public:
  IncludeResult* includeLocal(const char* header, const char*, size_t) override { return resolve(header); }
  IncludeResult* includeSystem(const char* header, const char*, size_t) override { return resolve(header); }
  void releaseInclude(IncludeResult* result) override { delete result; }

 private:
  static IncludeResult* resolve(std::string_view header) {
    std::string_view source;
    if (header == "zwwm.glsl") source = shader::zwwm_library;
    else if (header == "glass_physics.glsl") source = shader::glass_physics;
    else return nullptr;
    return new IncludeResult(std::string(header), source.data(), source.size(), nullptr);
  }
};

struct Interface {
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> bindings;
  bool push_constants = false;
  bool storage_buffers = false;
};

Interface spirv_interface(const std::vector<std::uint32_t>& spirv) {
  constexpr std::uint32_t kOpDecorate = 71, kOpVariable = 59;
  constexpr std::uint32_t kDecorationBinding = 33, kDecorationSet = 34;
  constexpr std::uint32_t kUniformConstant = 0, kUniform = 2, kPushConstant = 9, kStorageBuffer = 12;
  std::map<std::uint32_t, std::uint32_t> sets, bindings;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> variables;
  Interface result;
  for (std::size_t word = 5; word < spirv.size();) {
    const std::uint32_t count = spirv[word] >> 16U, opcode = spirv[word] & 0xffffU;
    if (count == 0 || word + count > spirv.size()) break;
    if (opcode == kOpDecorate && count >= 4) {
      if (spirv[word + 2] == kDecorationSet) sets[spirv[word + 1]] = spirv[word + 3];
      if (spirv[word + 2] == kDecorationBinding) bindings[spirv[word + 1]] = spirv[word + 3];
    } else if (opcode == kOpVariable && count >= 4) {
      variables.emplace_back(spirv[word + 2], spirv[word + 3]);
    }
    word += count;
  }
  for (const auto& [id, storage] : variables) {
    if (storage == kPushConstant) result.push_constants = true;
    if (storage == kStorageBuffer) result.storage_buffers = true;
    if (storage != kUniformConstant && storage != kUniform && storage != kStorageBuffer) continue;
    ++result.bindings[{sets.contains(id) ? sets[id] : 0U, bindings.contains(id) ? bindings[id] : 0U}];
  }
  return result;
}

bool compile(EShLanguage stage, std::string_view source, CompiledShader* shader, std::string* error) {
  initialize_glslang();
  glslang::TShader unit(stage);
  const std::string text(source);
  const char* strings = text.c_str();
  const char* name = stage == EShLangVertex ? "vertex" : "fragment";
  unit.setStringsWithLengthsAndNames(&strings, nullptr, &name, 1);
  unit.setPreamble(stage == EShLangFragment
                       ? "#extension GL_GOOGLE_include_directive : require\n#define ZWWM_FRAGMENT_STAGE 1\n"
                       : "#extension GL_GOOGLE_include_directive : require\n");
  unit.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
  unit.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
  unit.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
  const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
  Includer includer;
  if (!unit.parse(GetDefaultResources(), 450, ENoProfile, false, false, messages, includer)) {
    if (error != nullptr) *error = unit.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&unit);
  if (!program.link(messages)) {
    if (error != nullptr) *error = program.getInfoLog();
    return false;
  }
  CompiledShader result;
  glslang::SpvOptions options;
  options.validate = false;
  glslang::GlslangToSpv(*program.getIntermediate(stage), result.spirv, &options);
  if (result.spirv.empty()) {
    if (error != nullptr) *error = "SPIR-V generation failed";
    return false;
  }

  const auto interface = spirv_interface(result.spirv);
  if (interface.push_constants || interface.storage_buffers) {
    if (error != nullptr) *error = "push constants and storage buffers are not part of the zwwm shader ABI";
    return false;
  }
  for (const auto& [slot, count] : interface.bindings) {
    const bool known = slot == std::pair{0U, 0U} || slot == std::pair{0U, 1U} || slot == std::pair{1U, 0U};
    if (!known || count > 1) {
      if (error != nullptr)
        *error = "descriptor set " + std::to_string(slot.first) + " binding " + std::to_string(slot.second) +
                 (known ? " is declared twice" : " is not part of the zwwm shader ABI");
      return false;
    }
  }
  if (interface.bindings.contains({1U, 0U})) {
    if (!program.buildReflection(EShReflectionBasicArraySuffix | EShReflectionAllBlockVariables)) {
      if (error != nullptr) *error = "could not reflect the shader config block";
      return false;
    }
    int block = -1;
    for (int index = 0; index < program.getNumUniformBlocks(); ++index)
      if (program.getUniformBlock(index).name != kDrawBlock) {
        block = index;
        result.config = ConfigBlock{static_cast<std::uint32_t>(std::max(program.getUniformBlock(index).size, 0)), {}};
      }
    for (int index = 0; block >= 0 && index < program.getNumUniformVariables(); ++index) {
      const auto& uniform = program.getUniform(index);
      if (uniform.index != block) continue;
      std::string member = uniform.name;
      if (const auto dot = member.rfind('.'); dot != std::string::npos) member.erase(0, dot + 1);
      if (member.ends_with("[0]")) member.resize(member.size() - 3);
      if (member.find('[') != std::string::npos) {
        if (error != nullptr) *error = "config member '" + uniform.name + "' uses an unsupported aggregate type";
        return false;
      }
      result.config->members.push_back({member, static_cast<std::uint32_t>(uniform.glDefineType),
                                        static_cast<std::uint32_t>(std::max(uniform.offset, 0)),
                                        static_cast<std::uint32_t>(std::max(uniform.size, 1)),
                                        static_cast<std::uint32_t>(std::max(uniform.arrayStride, 0))});
    }
  }
  *shader = std::move(result);
  return true;
}

}  // namespace

bool compile_vertex(std::string_view source, CompiledShader* shader, std::string* error) {
  return compile(EShLangVertex, source, shader, error);
}

bool compile_fragment(std::string_view source, CompiledShader* shader, std::string* error) {
  if (source.find("#version 300 es") != std::string_view::npos) {
    if (error != nullptr)
      *error = "OpenGL ES shaders are no longer supported; port it to '#version 450' with "
               "'#include \"zwwm.glsl\"' (see CONFIG.md, User Shaders)";
    return false;
  }
  return compile(EShLangFragment, source, shader, error);
}

}  // namespace zwwm::renderer::detail
