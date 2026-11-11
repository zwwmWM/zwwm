#include "zwwm/runtime_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

// Compositor-specific checks, run through zw-lang's validation rules.
namespace zwwm {
namespace {

using lang::Assignment;
using lang::find_field;
using lang::SourceLocation;
using lang::ValidationContext;
using lang::Value;
using lang::VariableReference;

bool is_bool(const Value& value) { return std::holds_alternative<bool>(value.data); }
bool is_object(const Value& value) { return std::holds_alternative<Value::Object>(value.data); }

bool is_nonnegative_integer(const Value& value) {
  const auto* integer = std::get_if<std::int64_t>(&value.data);
  return integer != nullptr && *integer >= 0 && *integer <= std::numeric_limits<std::int32_t>::max();
}

bool is_output_mode(const Value& value) {
  const auto* text = std::get_if<std::string>(&value.data);
  if (text == nullptr) return false;
  if (*text == "preferred") return true;
  const auto x = text->find('x');
  const auto at = text->find('@', x == std::string::npos ? 0 : x + 1);
  if (x == std::string::npos || at == std::string::npos || x == 0 || at <= x + 1 || at + 1 == text->size()) return false;
  std::uint32_t width = 0, height = 0;
  double refresh = 0.0;
  const auto width_result = std::from_chars(text->data(), text->data() + x, width);
  const auto height_result = std::from_chars(text->data() + x + 1, text->data() + at, height);
  const auto refresh_result = std::from_chars(text->data() + at + 1, text->data() + text->size(), refresh);
  return width_result.ec == std::errc{} && width_result.ptr == text->data() + x && width > 0 &&
         height_result.ec == std::errc{} && height_result.ptr == text->data() + at && height > 0 &&
         refresh_result.ec == std::errc{} && refresh_result.ptr == text->data() + text->size() && refresh > 0.0;
}

void warn_unknown_fields(const Value::Object& object, std::span<const std::string_view> known,
                         ValidationContext& context, SourceLocation location,
                         std::string_view object_name) {
  for (const auto& name : object.names) {
    if (std::find(known.begin(), known.end(), name) == known.end()) {
      context.warning(location, "unknown " + std::string(object_name) + " setting: " + name);
    }
  }
}

bool is_uppercase_name(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isupper(character) || std::isdigit(character) || character == '_';
  });
}

void validate_assignment(const Assignment& assignment, ValidationContext& context) {
  if (is_uppercase_name(assignment.key) || assignment.key == "import") return;
  const auto validate_output = [&](const Value::Object& object, SourceLocation location,
                                   std::string_view prefix) {
    if (const Value* value = find_field(object, "mode"); value != nullptr && !is_output_mode(*value))
      context.error(value->location,
                       std::string(prefix) + "mode must be \"preferred\" or WIDTHxHEIGHT@HZ");
    if (const Value* value = find_field(object, "scale-per-mille"); value != nullptr) {
      const auto* scale = std::get_if<std::int64_t>(&value->data);
      if (scale == nullptr || *scale < 250 || *scale > 8000)
        context.error(value->location,
                         std::string(prefix) + "scale-per-mille must be an integer from 250 to 8000");
    }
    if (const Value* value = find_field(object, "bit-depth"); value != nullptr) {
      const auto* depth = std::get_if<std::int64_t>(&value->data);
      if (depth == nullptr || (*depth != 8 && *depth != 10))
        context.error(value->location,
                         std::string(prefix) + "bit-depth must be 8 or 10");
    }
    if (const Value* value = find_field(object, "transform"); value != nullptr) {
      const auto* transform = std::get_if<std::string>(&value->data);
      if (transform == nullptr || (*transform != "normal" && *transform != "90" &&
                                   *transform != "180" && *transform != "270"))
        context.error(value->location,
                         std::string(prefix) +
                             "transform must be \"normal\", \"90\", \"180\", or \"270\"");
    }
    constexpr std::array<std::string_view, 4> fields = {
        "mode", "scale-per-mille", "bit-depth", "transform"};
    warn_unknown_fields(object, fields, context, location,
                        prefix.substr(0, prefix.size() - 1));
  };
  if (assignment.key == "exec-sh-on-startup") {
    if (!std::holds_alternative<Value::Array>(assignment.value.data)) {
      context.error(assignment.location, "exec-sh-on-startup must be an array");
    }
    return;
  }
  if (assignment.key == "plugins") {
    const auto* paths = std::get_if<Value::Array>(&assignment.value.data);
    if (paths == nullptr) {
      context.error(assignment.location, "plugins must be an array");
    } else for (const auto& path : paths->values) {
      if (!std::holds_alternative<std::string>(path.data) &&
          !std::holds_alternative<VariableReference>(path.data))
        context.error(path.location, "plugin paths must be strings or variable references");
    }
    return;
  }
  if (assignment.key == "plugin-settings") {
    const auto* settings = std::get_if<Value::Object>(&assignment.value.data);
    if (settings == nullptr) {
      context.error(assignment.location, "plugin-settings must be an object");
    } else for (std::size_t index = 0; index < settings->names.size(); ++index) {
      if (settings->names[index].empty() || !is_object(settings->values[index]))
        context.error(settings->values[index].location,
                         "plugin-settings entries must have a nonempty name and an object value");
    }
    return;
  }
  if (assignment.key == "bind") {
    const auto* list = std::get_if<Value::List>(&assignment.value.data);
    if (list == nullptr || (list->values.size() != 4 && list->values.size() != 5)) {
      context.error(assignment.location,
                       "bind must contain modifier, key, action, argument, and optional flags");
    }
    return;
  }
  if (assignment.key == "shaders") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "shaders must be an object");
      return;
    }
    for (const auto& value : object->values) {
      if (!is_object(value))
        context.error(value.location, "each shader must be an object");
    }
    return;
  }
  if (assignment.key == "layout") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "layout must be an object");
      return;
    }
    if (const Value* value = find_field(*object, "default"); value != nullptr) {
      const auto* name = std::get_if<std::string>(&value->data);
      if (name == nullptr || (*name != "master-stack" && *name != "focus-fibonacci" &&
                              *name != "endless-canvas"))
        context.error(value->location,
                          "layout.default must be \"master-stack\", \"focus-fibonacci\", or \"endless-canvas\"");
    }
    if (const Value* value = find_field(*object, "master-count"); value != nullptr) {
      const auto* count = std::get_if<std::int64_t>(&value->data);
      if (count == nullptr || *count <= 0 || *count > std::numeric_limits<std::int32_t>::max()) {
        context.error(value->location, "layout.master-count must be a positive integer");
      }
    }
    if (const Value* value = find_field(*object, "master-ratio"); value != nullptr) {
      const auto* ratio = std::get_if<std::int64_t>(&value->data);
      if (ratio == nullptr || *ratio < 10 || *ratio > 90) {
        context.error(value->location,
                         "layout.master-ratio must be an integer percent from 10 to 90");
      }
    }
    for (const char* name : {"outer-gap", "inner-gap", "gaps", "min-zoom-per-mille",
                             "max-zoom-per-mille"}) {
      if (const Value* value = find_field(*object, name); value != nullptr && !is_nonnegative_integer(*value)) {
        context.error(value->location,
                         std::string("layout.") + name + " must be a nonnegative integer");
      }
    }
    if (const Value* value = find_field(*object, "smart-gaps"); value != nullptr && !is_bool(*value)) {
      context.error(value->location, "layout.smart-gaps must be a boolean");
    }
    constexpr std::array<std::string_view, 9> fields = {
        "default", "master-count", "master-ratio", "outer-gap", "inner-gap", "gaps",
        "min-zoom-per-mille", "max-zoom-per-mille", "smart-gaps"};
    warn_unknown_fields(*object, fields, context, assignment.location, "layout");
    return;
  }
  if (assignment.key == "input") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "input must be an object");
      return;
    }
    if (const Value* value = find_field(*object, "focus-mode"); value != nullptr) {
      const auto* mode = std::get_if<std::string>(&value->data);
      if (mode == nullptr || (*mode != "hover" && *mode != "by-click" && *mode != "click"))
        context.error(value->location,
                         "input.focus-mode must be \"hover\" or \"by-click\"");
    }
    if (const Value* value = find_field(*object, "reverse-mouse-scrolling");
        value != nullptr && !is_bool(*value))
      context.error(value->location,
                       "input.reverse-mouse-scrolling must be a boolean");
    constexpr std::array<std::string_view, 2> input_fields = {
        "focus-mode", "reverse-mouse-scrolling"};
    warn_unknown_fields(*object, input_fields, context, assignment.location, "input");
    return;
  }
  if (assignment.key == "keyboard") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "keyboard must be an object");
      return;
    }
    for (const char* name : {"rules", "model", "layout", "variant", "options"}) {
      if (const Value* value = find_field(*object, name);
          value != nullptr && !std::holds_alternative<std::string>(value->data)) {
        context.error(value->location,
                         std::string("keyboard.") + name + " must be a string");
      }
    }
    constexpr std::array<std::string_view, 5> keyboard_fields = {
        "rules", "model", "layout", "variant", "options"};
    warn_unknown_fields(*object, keyboard_fields, context, assignment.location, "keyboard");
    return;
  }
  if (assignment.key == "mobile") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "mobile must be an object");
      return;
    }
    if (const auto* value = find_field(*object, "windows"); value != nullptr && !is_bool(*value))
      context.error(value->location, "mobile.windows must be a boolean");
    for (const auto& [name, low, high] : {std::tuple{"edge-swipe-size", 0, 256}, std::tuple{"hold-ms", 50, 5000}}) {
      const auto* value = find_field(*object, name);
      if (value == nullptr) return;
      const auto* integer = std::get_if<std::int64_t>(&value->data);
      if (integer == nullptr || *integer < low || *integer > high)
        context.error(value->location,
                         "mobile." + std::string(name) + " must be " + std::to_string(low) + ".." + std::to_string(high));
    }
    constexpr std::array<std::string_view, 3> fields = {"windows", "edge-swipe-size", "hold-ms"};
    warn_unknown_fields(*object, fields, context, assignment.location, "mobile");
    return;
  }
  if (assignment.key == "overview") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "overview must be an object");
      return;
    }
    for (const auto* name : {"backdrop-blur-radius", "backdrop-blur-passes"}) {
      const auto* value = find_field(*object, name);
      if (value == nullptr) return;
      const auto* integer = std::get_if<std::int64_t>(&value->data);
      const bool passes = std::string_view(name) == "backdrop-blur-passes";
      if (integer == nullptr || *integer < (passes ? 1 : 0) || *integer > (passes ? 8 : 128))
        context.error(value->location, "invalid overview." + std::string(name));
    }
    if (const auto* value = find_field(*object, "scene-radius")) {
      const auto* integer = std::get_if<std::int64_t>(&value->data);
      if (integer == nullptr || *integer < 0 || *integer > 512)
        context.error(value->location, "overview.scene-radius must be 0..512");
    }
    if (const auto* value = find_field(*object, "master-ratio")) {
      const auto* integer = std::get_if<std::int64_t>(&value->data);
      if (integer == nullptr || *integer < 10 || *integer > 90)
        context.error(value->location, "overview.master-ratio must be 10..90");
    }
    constexpr std::array<std::string_view, 6> fields = {"backdrop-image", "backdrop-blur-radius", "backdrop-blur-passes", "scene-radius", "layout", "master-ratio"};
    warn_unknown_fields(*object, fields, context, assignment.location, "overview");
    return;
  }
  if (assignment.key == "animations") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "animations must be an object");
      return;
    }
    for (const char* name : {"enabled", "open-window", "resize", "close"}) {
      if (const Value* value = find_field(*object, name); value != nullptr && !is_bool(*value)) {
        context.error(value->location,
                         std::string("animations.") + name + " must be a boolean");
      }
    }
    if (const Value* value = find_field(*object, "duration-ms"); value != nullptr && !is_nonnegative_integer(*value)) {
      context.error(value->location,
                       "animations.duration-ms must be a nonnegative integer");
    }
    const auto ranged_animation = [&](const char* name, std::int64_t minimum, std::int64_t maximum) {
      const Value* value = find_field(*object, name);
      if (value == nullptr) return;
      const auto* integer = std::get_if<std::int64_t>(&value->data);
      if (integer == nullptr || *integer < minimum || *integer > maximum) {
        context.error(value->location,
                         std::string("animations.") + name + " must be an integer from " +
                             std::to_string(minimum) + " to " + std::to_string(maximum));
      }
    };
    ranged_animation("open-scale-per-mille", 500, 1000);
    ranged_animation("close-scale-per-mille", 500, 1000);
    ranged_animation("open-offset-px", 0, 4096);
    ranged_animation("close-offset-px", 0, 4096);
    ranged_animation("tag-scale-per-mille", 500, 1000);
    ranged_animation("tag-parallax-per-mille", 0, 1000);
    ranged_animation("tag-fade-per-mille", 0, 1000);
    ranged_animation("tag-duration-ms", 0, 10000);
    ranged_animation("overview-duration-ms", 0, 10000);
    ranged_animation("open-duration-ms", 0, 10000);
    ranged_animation("close-duration-ms", 0, 10000);
    ranged_animation("move-duration-ms", 0, 10000);
    ranged_animation("fade-duration-ms", 0, 10000);
    if (const Value* value = find_field(*object, "tag-transition-axis"); value != nullptr) {
      const auto* axis = std::get_if<std::string>(&value->data);
      if (axis == nullptr || (*axis != "horizontal" && *axis != "vertical"))
        context.error(value->location,
                         "animations.tag-transition-axis must be horizontal or vertical");
    }
    if (const Value* value = find_field(*object, "spring"); value != nullptr) {
      const auto* spring = std::get_if<Value::Object>(&value->data);
      if (spring == nullptr) {
        context.error(value->location, "animations.spring must be an object");
      } else {
        for (const char* name : {"stiffness", "damping", "mass-per-mille"}) {
          const Value* parameter = find_field(*spring, name);
          const auto* integer = parameter == nullptr ? nullptr : std::get_if<std::int64_t>(&parameter->data);
          const bool invalid = integer == nullptr || *integer < (std::string_view(name) == "damping" ? 0 : 1) ||
                               *integer > std::numeric_limits<std::int32_t>::max();
          if (parameter != nullptr && invalid) {
            context.error(parameter->location,
                             std::string("animations.spring.") + name +
                                 (std::string_view(name) == "damping" ? " must be a nonnegative integer"
                                                                      : " must be a positive integer"));
          }
        }
        constexpr std::array<std::string_view, 3> spring_fields = {"stiffness", "damping", "mass-per-mille"};
        warn_unknown_fields(*spring, spring_fields, context, assignment.location, "animations.spring");
      }
    }
    if (const Value* value = find_field(*object, "cubic-bezier"); value != nullptr) {
      const auto* bezier = std::get_if<Value::Object>(&value->data);
      if (bezier == nullptr) {
        context.error(value->location, "animations.cubic-bezier must be an object");
      } else {
        for (const std::string name : {"x1", "y1", "x2", "y2", "x1-per-mille", "y1-per-mille", "x2-per-mille", "y2-per-mille"}) {
          const Value* point = find_field(*bezier, name);
          const auto* integer = point == nullptr ? nullptr : std::get_if<std::int64_t>(&point->data);
          if (point != nullptr && (integer == nullptr || *integer < 0 || *integer > 1000)) {
            context.error(point->location,
                             std::string("animations.cubic-bezier.") + name + " must be an integer from 0 to 1000");
          }
        }
        constexpr std::array<std::string_view, 8> bezier_fields = {
            "x1", "y1", "x2", "y2", "x1-per-mille", "y1-per-mille", "x2-per-mille", "y2-per-mille"};
        warn_unknown_fields(*bezier, bezier_fields, context, assignment.location,
                            "animations.cubic-bezier");
      }
    }
    constexpr std::array<std::string_view, 21> animation_fields = {
        "enabled", "duration-ms", "open-duration-ms", "close-duration-ms", "move-duration-ms", "fade-duration-ms",
        "tag-duration-ms", "spring", "cubic-bezier", "open-window",
        "resize", "close", "open-scale-per-mille", "close-scale-per-mille", "open-offset-px",
        "close-offset-px", "tag-scale-per-mille", "tag-parallax-per-mille", "tag-fade-per-mille",
        "tag-transition-axis", "overview-duration-ms"};
    warn_unknown_fields(*object, animation_fields, context, assignment.location, "animations");
    return;
  }
  if (assignment.key == "output") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "output must be an object");
      return;
    }
    validate_output(*object, assignment.location, "output.");
    return;
  }
  if (assignment.key == "outputs") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "outputs must be an object");
      return;
    }
    for (std::size_t index = 0; index < object->names.size(); ++index) {
      if (object->names[index].empty()) {
        context.error(assignment.location, "outputs connector names must not be empty");
        return;
      }
      const auto* configured = std::get_if<Value::Object>(&object->values[index].data);
      if (configured == nullptr) {
        context.error(assignment.location,
                         "outputs." + object->names[index] + " must be an object");
        return;
      }
      validate_output(*configured, assignment.location,
                      "outputs." + object->names[index] + ".");
    }
    return;
  }
  if (assignment.key == "environment") {
    const auto* object = std::get_if<Value::Object>(&assignment.value.data);
    if (object == nullptr) {
      context.error(assignment.location, "environment must be an object");
      return;
    }
    for (std::size_t index = 0; index < object->names.size(); ++index) {
      const auto& name = object->names[index];
      const auto& value = object->values[index];
      const bool valid_name = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
          std::all_of(std::next(name.begin()), name.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '_';
          });
      if (!valid_name) context.error(assignment.location, "environment variable names must be shell identifiers");
      if (!std::holds_alternative<std::string>(value.data) && !std::holds_alternative<VariableReference>(value.data))
        context.error(assignment.location, "environment values must be strings or variable references");
    }
    return;
  }
  if (assignment.key == "rule") {
    const auto* rule = std::get_if<Value::Object>(&assignment.value.data);
    if (rule == nullptr) {
      context.error(assignment.location, "rule must be an object");
      return;
    }
    const Value* match = find_field(*rule, "match");
    const Value* set = find_field(*rule, "set");
    if (match == nullptr || !is_object(*match)) {
      context.error(match == nullptr ? assignment.location : match->location, "rule.match must be an object");
    }
    if (set == nullptr || !is_object(*set)) {
      context.error(set == nullptr ? assignment.location : set->location, "rule.set must be an object");
      return;
    }
    const auto& actions = std::get<Value::Object>(set->data);
    for (const char* name : {"floating", "centered", "always-on-top", "glass"}) {
      if (const Value* value = find_field(actions, name); value != nullptr && !is_bool(*value)) {
        context.error(value->location, std::string("rule.set.") + name + " must be a boolean");
      }
    }
    return;
  }
  // Layer rules are checked field by field when the configuration is compiled.
  if (assignment.key == "layer-rule") return;
  context.warning(assignment.location, "unknown top-level setting: " + assignment.key);
}

}  // namespace

lang::ValidationResult validate_runtime_config(const lang::Config& config) {
  static const std::array<lang::ValidationRule, 1> rules{validate_assignment};
  return lang::validate_config(config, rules);
}

}  // namespace zwwm
