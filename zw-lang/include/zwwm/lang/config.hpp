#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace zwwm::lang {

struct SourceLocation {
  SourceLocation() = default;
  SourceLocation(std::size_t line, std::size_t column = 1, std::string file = {})
      : line(line), column(column), file(std::move(file)) {}

  std::size_t line = 1;
  std::size_t column = 1;
  std::string file;  // empty for in-memory input
};

enum class DiagnosticLevel { warning, error };

struct Diagnostic {
  DiagnosticLevel level = DiagnosticLevel::error;
  SourceLocation location;
  std::string message;
};

struct VariableReference {
  std::string name;
};

struct Value;

struct ArrayValue {
  std::vector<Value> values;
};

struct ListValue {
  std::vector<Value> values;
};

struct ObjectValue {
  std::vector<std::string> names;
  std::vector<Value> values;

  void add(std::string name, Value value);
};

struct Value {
  using Array = ArrayValue;
  using List = ListValue;
  using Object = ObjectValue;
  using Storage = std::variant<std::monostate, std::string, bool, std::int64_t, VariableReference, Array, List, Object>;

  Value() = default;
  template <typename T>
    requires(!std::same_as<std::remove_cvref_t<T>, Value> && std::constructible_from<Storage, T>)
  Value(T&& value, SourceLocation where = {}) : data(std::forward<T>(value)), location(std::move(where)) {}

  Storage data;
  SourceLocation location;
  // Set for decimal literals: the integer holds thousandths (0.85 -> 850).
  bool decimal = false;
};

inline void ObjectValue::add(std::string name, Value value) {
  names.push_back(std::move(name));
  try {
    values.push_back(std::move(value));
  } catch (...) {
    names.pop_back();
    throw;
  }
}

struct Assignment {
  SourceLocation location;
  std::string key;
  Value value;
};

struct Config {
  std::vector<Assignment> assignments;
};

struct ParseResult {
  Config config;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> source_paths;

  [[nodiscard]] bool ok() const;
};

struct ValidationResult {
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool ok() const;
};

class ValidationContext {
 public:
  explicit ValidationContext(std::vector<Diagnostic>& diagnostics);

  void warning(SourceLocation location, std::string message);
  void error(SourceLocation location, std::string message);

 private:
  std::vector<Diagnostic>& diagnostics_;
};

// Component validators add semantic checks without coupling them to zw-lang.
// Parsing resolves $VARIABLES and "${VARIABLE}" splices, so validators see plain values.
using ValidationRule = std::function<void(const Assignment&, ValidationContext&)>;

[[nodiscard]] ParseResult parse_config(const std::string& input);
[[nodiscard]] ParseResult parse_config_file(const std::string& path);
[[nodiscard]] ValidationResult validate_config(const Config& config);
[[nodiscard]] ValidationResult validate_config(const Config& config, std::span<const ValidationRule> rules);

// Lookups return the last occurrence, matching how later assignments override earlier ones.
[[nodiscard]] const Value* find_assignment(const Config& config, std::string_view key);
[[nodiscard]] const Value* find_field(const Value::Object& object, std::string_view key);
[[nodiscard]] const std::string* as_string(const Value& value);
[[nodiscard]] const bool* as_boolean(const Value& value);
[[nodiscard]] const std::int64_t* as_integer(const Value& value);
[[nodiscard]] const Value::Array* as_array(const Value& value);
[[nodiscard]] const Value::List* as_list(const Value& value);
[[nodiscard]] const Value::Object* as_object(const Value& value);

}  // namespace zwwm::lang
