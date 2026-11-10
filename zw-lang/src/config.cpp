#include "zwwm/lang/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace zwwm::lang {
namespace {

constexpr std::size_t kMaxDiagnostics = 64;
// An escaped "\$" is stored as NUL + '$' until resolution so it is never spliced.
constexpr char kEscapedDollar = '\0';

bool is_variable_name(std::string_view name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return std::isupper(character) || std::isdigit(character) || character == '_';
  });
}

void append_utf8(std::string& text, std::uint32_t code) {
  if (code < 0x80) {
    text += static_cast<char>(code);
  } else if (code < 0x800) {
    text += static_cast<char>(0xc0 | (code >> 6));
    text += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x10000) {
    text += static_cast<char>(0xe0 | (code >> 12));
    text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    text += static_cast<char>(0x80 | (code & 0x3f));
  } else {
    text += static_cast<char>(0xf0 | (code >> 18));
    text += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    text += static_cast<char>(0x80 | (code & 0x3f));
  }
}

class Parser {
 public:
  Parser(std::string_view input, std::string file) : input_(input) { location_.file = std::move(file); }

  ParseResult parse() {
    Config config;
    skip_ignored();
    while (!at_end()) {
      const std::size_t start = position_;
      const SourceLocation location = location_;
      const auto key = parse_identifier();
      if (!key.has_value()) {
        synchronize(0, start);
        continue;
      }
      if (!consume('=')) {
        error("expected '=' after '" + *key + "'");
        synchronize(0, start);
        continue;
      }
      auto value = parse_value_list();
      if (!value.has_value()) {
        synchronize(0, start);
        continue;
      }
      config.assignments.push_back({location, *key, std::move(*value)});
      skip_ignored();
    }
    return {std::move(config), std::move(diagnostics_), {}};
  }

 private:
  [[nodiscard]] bool at_end() const { return position_ >= input_.size(); }

  [[nodiscard]] char peek(std::size_t ahead = 0) const {
    return position_ + ahead < input_.size() ? input_[position_ + ahead] : '\0';
  }

  char take() {
    const char character = peek();
    if (!at_end()) {
      ++position_;
      if (character == '\n') {
        ++location_.line;
        location_.column = 1;
      } else {
        ++location_.column;
      }
    }
    return character;
  }

  void skip_ignored() {
    while (!at_end()) {
      if (std::isspace(static_cast<unsigned char>(peek()))) {
        take();
        continue;
      }
      if (peek() == '-' && peek(1) == '-') {
        while (!at_end() && take() != '\n') {
        }
        continue;
      }
      break;
    }
  }

  bool consume(char expected) {
    skip_ignored();
    if (peek() != expected) return false;
    take();
    if (expected == '[' || expected == '{') ++depth_;
    if (expected == ']' || expected == '}') --depth_;
    skip_ignored();
    return true;
  }

  static bool is_identifier_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
  }

  static bool is_identifier_character(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-';
  }

  std::optional<std::string> parse_identifier() {
    skip_ignored();
    if (!is_identifier_start(peek())) {
      error(at_end() ? "unexpected end of file" : std::string("unexpected '") + peek() + "'");
      return std::nullopt;
    }
    std::string identifier;
    while (is_identifier_character(peek())) identifier += take();
    skip_ignored();
    return identifier;
  }

  std::optional<Value> parse_value_list() {
    skip_ignored();
    const SourceLocation location = location_;
    Value::List list;
    if (peek() == ',') {
      list.values.emplace_back(std::string{}, location);
    } else {
      auto first = parse_value();
      if (!first.has_value()) return std::nullopt;
      list.values.push_back(std::move(*first));
    }
    while (consume(',')) {
      auto next = parse_value();
      if (!next.has_value()) return std::nullopt;
      list.values.push_back(std::move(*next));
    }
    if (list.values.size() == 1) return std::move(list.values.front());
    return Value{std::move(list), location};
  }

  std::optional<Value> parse_value() {
    skip_ignored();
    const SourceLocation location = location_;
    if (peek() == '"') return parse_string();
    if (peek() == '$') {
      take();
      const auto name = parse_identifier();
      if (!name.has_value()) return std::nullopt;
      return Value{VariableReference{*name}, location};
    }
    if (peek() == '[') return parse_array();
    if (peek() == '{') return parse_object();
    if (std::isdigit(static_cast<unsigned char>(peek())) ||
        (peek() == '-' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
      return parse_number();
    }
    const auto identifier = parse_identifier();
    if (!identifier.has_value()) return std::nullopt;
    if (*identifier == "true") return Value{true, location};
    if (*identifier == "false") return Value{false, location};
    if (*identifier == "null") return Value{std::monostate{}, location};
    return Value{*identifier, location};
  }

  std::optional<Value> parse_string() {
    const SourceLocation location = location_;
    take();
    std::string value;
    while (!at_end() && peek() != '"') {
      if (peek() == '\0') {
        error("strings cannot contain NUL bytes");
        return std::nullopt;
      }
      if (peek() != '\\') {
        value += take();
        continue;
      }
      take();
      if (at_end()) break;
      const char escaped = take();
      switch (escaped) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'r': value += '\r'; break;
        case 'e': value += '\x1b'; break;
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '$': value += kEscapedDollar; value += '$'; break;
        case 'u': {
          if (!parse_unicode_escape(value)) return std::nullopt;
          break;
        }
        default:
          // Unknown escapes stay literal, so regexes like "^foo\.bar$" keep working.
          value += '\\';
          value += escaped;
      }
    }
    if (at_end()) {
      diagnostics_.push_back({DiagnosticLevel::error, location, "unterminated string"});
      return std::nullopt;
    }
    take();
    skip_ignored();
    return Value{std::move(value), location};
  }

  bool parse_unicode_escape(std::string& value) {
    if (peek() != '{') {
      error("expected '{' after \\u, as in \\u{2192}");
      return false;
    }
    take();
    std::uint32_t code = 0;
    int digits = 0;
    while (std::isxdigit(static_cast<unsigned char>(peek())) && digits < 6) {
      const char digit = take();
      code = code * 16 + static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(digit))
                                                         ? digit - '0'
                                                         : std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10);
      ++digits;
    }
    if (digits == 0 || peek() != '}' || code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
      error("invalid unicode escape; use \\u{1} to \\u{10FFFF}");
      return false;
    }
    take();
    append_utf8(value, code);
    return true;
  }

  std::optional<Value> parse_number() {
    const SourceLocation location = location_;
    const bool negative = peek() == '-';
    if (negative) take();
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    std::int64_t value = 0;
    bool overflow = false;
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      const int digit = take() - '0';
      if (value > (limit - digit) / 10) overflow = true;
      else value = value * 10 + digit;
    }
    bool decimal = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
      take();
      decimal = true;
      int places = 0;
      std::int64_t fraction = 0;
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        const int digit = take() - '0';
        if (++places <= 3) fraction = fraction * 10 + digit;
      }
      if (places > 3) {
        diagnostics_.push_back({DiagnosticLevel::error, location, "decimals have at most 3 places"});
        return std::nullopt;
      }
      for (; places < 3; ++places) fraction *= 10;
      if (value > (limit - fraction) / 1000) overflow = true;
      else value = value * 1000 + fraction;
    }
    if (overflow) {
      diagnostics_.push_back({DiagnosticLevel::error, location, "number is out of range"});
      return std::nullopt;
    }
    skip_ignored();
    Value result{negative ? -value : value, location};
    result.decimal = decimal;
    return result;
  }

  std::optional<Value> parse_array() {
    const SourceLocation location = location_;
    consume('[');
    Value::Array values;
    if (consume(']')) return Value{std::move(values), location};
    while (true) {
      auto value = parse_value();
      if (!value.has_value()) return std::nullopt;
      values.values.push_back(std::move(*value));
      if (consume(']')) return Value{std::move(values), location};
      if (!consume(',')) {
        error("expected ',' or ']' in array");
        return std::nullopt;
      }
      if (consume(']')) return Value{std::move(values), location};
    }
  }

  std::optional<Value> parse_object() {
    const SourceLocation location = location_;
    consume('{');
    const int depth = depth_;
    Value::Object fields;
    std::unordered_set<std::string> seen;
    while (!consume('}')) {
      if (at_end()) {
        diagnostics_.push_back({DiagnosticLevel::error, location, "unterminated object"});
        return std::nullopt;
      }
      const std::size_t start = position_;
      const SourceLocation field_location = location_;
      std::optional<std::string> key;
      if (peek() == '"') {
        auto quoted = parse_string();
        if (quoted.has_value()) key = std::get<std::string>(quoted->data);
      } else {
        key = parse_identifier();
      }
      std::optional<Value> value;
      if (key.has_value()) {
        if (!consume('=')) error("expected '=' after '" + *key + "'");
        else value = parse_value_list();
      }
      // A broken field skips to the next line of this object instead of the whole object.
      if (!value.has_value()) {
        synchronize(depth, start);
        continue;
      }
      if (!seen.insert(*key).second)
        diagnostics_.push_back({DiagnosticLevel::warning, field_location,
                                "duplicate field '" + *key + "'; the last one is used"});
      fields.add(*key, std::move(*value));
    }
    return Value{std::move(fields), location};
  }

  // Brackets left open by the broken value are abandoned, then this skips to the next line
  // at depth `target` or stops before the closer of the enclosing object.
  void synchronize(int target, std::size_t start) {
    depth_ = target;
    while (!at_end()) {
      const char character = peek();
      if (character == '\n' && depth_ <= target && position_ > start) break;
      if ((character == '}' || character == ']') && target > 0 && depth_ <= target && position_ > start) break;
      if (character == '"') {
        take();
        while (!at_end() && peek() != '"' && peek() != '\n') {
          if (peek() == '\\') take();
          take();
        }
        if (peek() == '"') take();
        continue;
      }
      if (character == '-' && peek(1) == '-') {
        while (!at_end() && peek() != '\n') take();
        continue;
      }
      if (character == '{' || character == '[') ++depth_;
      if ((character == '}' || character == ']') && depth_ > 0) --depth_;
      take();
    }
    skip_ignored();
  }

  void error(std::string message) {
    if (diagnostics_.size() < kMaxDiagnostics)
      diagnostics_.push_back({DiagnosticLevel::error, location_, std::move(message)});
  }

  std::string_view input_;
  std::size_t position_ = 0;
  int depth_ = 0;
  SourceLocation location_;
  std::vector<Diagnostic> diagnostics_;
};

std::string decimal_text(std::int64_t thousandths) {
  std::string text = thousandths < 0 ? "-" : "";
  const std::uint64_t magnitude = thousandths < 0 ? 0 - static_cast<std::uint64_t>(thousandths)
                                                  : static_cast<std::uint64_t>(thousandths);
  text += std::to_string(magnitude / 1000);
  std::string fraction = std::to_string(1000 + magnitude % 1000).substr(1);
  while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
  return fraction.empty() ? text : text + "." + fraction;
}

// Substitutes variables in place. The last definition of a variable wins everywhere, so a
// later override also reaches values from files imported before it.
class Resolver {
 public:
  Resolver(Config& config, std::vector<Diagnostic>& diagnostics) : config_(config), diagnostics_(diagnostics) {}

  void run() {
    for (auto& assignment : config_.assignments)
      if (is_variable_name(assignment.key)) variables_[assignment.key] = &assignment;
    for (auto& assignment : config_.assignments) {
      if (assignment.key == "import") continue;
      if (is_variable_name(assignment.key)) {
        if (variables_[assignment.key] == &assignment) define(assignment.key);
        else resolve(assignment.value);
        continue;
      }
      resolve(assignment.value);
      settle_decimal(assignment.key, assignment.value);
    }
  }

 private:
  enum class State { pending, active, done };

  // Resolves a variable's own value once; reports cycles instead of recursing forever.
  const Value* define(const std::string& name) {
    const auto found = variables_.find(name);
    if (found == variables_.end()) return nullptr;
    auto& state = states_[name];
    if (state == State::active) {
      error(found->second->location, "variable $" + name + " refers to itself");
      return nullptr;
    }
    if (state == State::pending) {
      state = State::active;
      resolve(found->second->value);
      state = State::done;
    }
    return &found->second->value;
  }

  void resolve(Value& value) {
    if (auto* reference = std::get_if<VariableReference>(&value.data)) {
      const std::string name = reference->name;
      if (const Value* target = define(name)) {
        value.data = target->data;
        value.decimal = target->decimal;
      } else if (variables_.contains(name)) {
        value.data = std::string{};
      } else if (const char* environment = std::getenv(name.c_str())) {
        value.data = std::string(environment);
      } else {
        error(value.location, "undefined variable $" + name);
        value.data = std::string{};
      }
      return;
    }
    if (auto* text = std::get_if<std::string>(&value.data)) {
      splice(*text, value.location);
      return;
    }
    const auto elements = [&](std::vector<Value>& values) {
      for (auto& element : values) {
        resolve(element);
        if (element.decimal)
          error(element.location, "decimals can only be setting values, not list or array items");
      }
    };
    if (auto* array = std::get_if<Value::Array>(&value.data)) elements(array->values);
    else if (auto* list = std::get_if<Value::List>(&value.data)) elements(list->values);
    else if (auto* object = std::get_if<Value::Object>(&value.data)) {
      for (std::size_t index = 0; index < object->values.size(); ++index) {
        resolve(object->values[index]);
        settle_decimal(object->names[index], object->values[index]);
      }
    }
  }

  // "name = 0.85" is stored as "name-per-mille = 850", the form consumers read.
  void settle_decimal(std::string& name, const Value& value) {
    if (!value.decimal) return;
    if (name.ends_with("-per-mille")) {
      error(value.location, name + " takes a whole number; write " +
                                name.substr(0, name.size() - 10) + " = " +
                                decimal_text(std::get<std::int64_t>(value.data)) + " instead");
      return;
    }
    name += "-per-mille";
  }

  // Expands $NAME and ${NAME}. Unknown names stay literal so shell commands keep their own.
  void splice(std::string& text, const SourceLocation& location) {
    if (text.find('$') == std::string::npos) return;
    std::string result;
    for (std::size_t index = 0; index < text.size();) {
      if (text[index] == kEscapedDollar && index + 1 < text.size() && text[index + 1] == '$') {
        result += '$';
        index += 2;
        continue;
      }
      if (text[index] != '$') {
        result += text[index++];
        continue;
      }
      std::size_t end = index + 1;
      std::string name;
      if (end < text.size() && text[end] == '{') {
        const auto close = text.find('}', end + 1);
        if (close != std::string::npos) name = text.substr(end + 1, close - end - 1);
        end = close == std::string::npos ? end : close + 1;
      } else {
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
          ++end;
        if (end > index + 1 && !std::isdigit(static_cast<unsigned char>(text[index + 1])))
          name = text.substr(index + 1, end - index - 1);
      }
      const auto replacement = name.empty() ? std::nullopt : lookup_text(name, location);
      if (replacement.has_value()) {
        result += *replacement;
        index = end;
      } else {
        result += text[index++];
      }
    }
    text = std::move(result);
  }

  std::optional<std::string> lookup_text(const std::string& name, const SourceLocation& location) {
    if (variables_.contains(name)) {
      const Value* value = define(name);
      if (value == nullptr) return std::string{};
      if (const auto* text = std::get_if<std::string>(&value->data)) return *text;
      if (const auto* integer = std::get_if<std::int64_t>(&value->data))
        return value->decimal ? decimal_text(*integer) : std::to_string(*integer);
      if (const auto* boolean = std::get_if<bool>(&value->data)) return *boolean ? "true" : "false";
      error(location, "$" + name + " is not text, a number, or a boolean, so it cannot go inside a string");
      return std::string{};
    }
    if (const char* environment = std::getenv(name.c_str())) return std::string(environment);
    return std::nullopt;
  }

  void error(const SourceLocation& location, std::string message) {
    diagnostics_.push_back({DiagnosticLevel::error, location, std::move(message)});
  }

  Config& config_;
  std::vector<Diagnostic>& diagnostics_;
  std::unordered_map<std::string, Assignment*> variables_;
  std::unordered_map<std::string, State> states_;
};

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.level == DiagnosticLevel::error;
  });
}

void check_variables(const Value& value, std::vector<Diagnostic>& diagnostics) {
  std::visit(
      [&](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, VariableReference>) {
          diagnostics.push_back({DiagnosticLevel::error, value.location, "undefined variable $" + item.name});
        } else if constexpr (std::is_same_v<Item, Value::Array> || std::is_same_v<Item, Value::List>) {
          for (const Value& nested : item.values) check_variables(nested, diagnostics);
        } else if constexpr (std::is_same_v<Item, Value::Object>) {
          for (const auto& nested : item.values) check_variables(nested, diagnostics);
        }
      },
      value.data);
}

std::optional<std::string> expand_environment(std::string_view value, std::string& error) {
  std::string expanded;
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] == kEscapedDollar && index + 1 < value.size() && value[index + 1] == '$') {
      expanded += '$';
      index += 2;
      continue;
    }
    if (value[index] != '$') {
      expanded += value[index++];
      continue;
    }

    const std::size_t marker = index++;
    std::string name;
    if (index < value.size() && value[index] == '{') {
      const auto end = value.find('}', ++index);
      if (end == std::string_view::npos || end == index) {
        error = "invalid environment variable in import path";
        return std::nullopt;
      }
      name = value.substr(index, end - index);
      index = end + 1;
    } else {
      const std::size_t begin = index;
      while (index < value.size() &&
             (std::isalnum(static_cast<unsigned char>(value[index])) || value[index] == '_')) ++index;
      if (begin == index) {
        expanded += value[marker];
        continue;
      }
      name = value.substr(begin, index - begin);
    }

    const char* environment = std::getenv(name.c_str());
    if (environment == nullptr) {
      error = "undefined environment variable $" + name + " in import path";
      return std::nullopt;
    }
    expanded += environment;
  }
  return expanded;
}

std::optional<std::string> import_path(const Value& value, std::string& error) {
  if (const auto* text = std::get_if<std::string>(&value.data)) return expand_environment(*text, error);
  if (const auto* reference = std::get_if<VariableReference>(&value.data)) {
    const char* environment = std::getenv(reference->name.c_str());
    if (environment != nullptr) return std::string(environment);
    error = "undefined environment variable $" + reference->name + " in import path";
    return std::nullopt;
  }
  error = "import paths must be strings or environment variable references";
  return std::nullopt;
}

ParseResult parse_config_file_recursive(const std::filesystem::path& requested,
                                        std::vector<std::filesystem::path>& stack,
                                        const SourceLocation& location = {}) {
  std::error_code error_code;
  auto path = std::filesystem::weakly_canonical(requested, error_code);
  if (error_code) path = std::filesystem::absolute(requested).lexically_normal();
  const auto requested_path = std::filesystem::absolute(requested).lexically_normal().string();
  if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
    return {{}, {{DiagnosticLevel::error, location, "configuration import cycle at " + path.string()}},
            {path.string(), requested_path}};
  }

  std::ifstream file(path);
  if (!file) {
    return {{}, {{DiagnosticLevel::error, location, "could not open configuration file: " + path.string()}},
            {path.string(), requested_path}};
  }
  const std::string input{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto parsed = Parser(input, requested_path).parse();
  parsed.source_paths.push_back(path.string());
  parsed.source_paths.push_back(requested_path);
  if (!parsed.ok()) return parsed;

  stack.push_back(path);
  Config merged;
  for (auto& assignment : parsed.config.assignments) {
    if (assignment.key != "import") {
      merged.assignments.push_back(std::move(assignment));
      continue;
    }

    std::vector<const Value*> values;
    if (const auto* array = std::get_if<Value::Array>(&assignment.value.data)) {
      for (const auto& value : array->values) values.push_back(&value);
    } else {
      values.push_back(&assignment.value);
    }
    for (const Value* value : values) {
      std::string import_error;
      const auto imported_path = import_path(*value, import_error);
      if (!imported_path) {
        parsed.diagnostics.push_back({DiagnosticLevel::error, value->location, std::move(import_error)});
        continue;
      }
      std::filesystem::path child(*imported_path);
      if (child.is_relative()) child = path.parent_path() / child;
      auto imported = parse_config_file_recursive(child, stack, value->location);
      parsed.source_paths.insert(parsed.source_paths.end(), imported.source_paths.begin(), imported.source_paths.end());
      parsed.diagnostics.insert(parsed.diagnostics.end(),
                                std::make_move_iterator(imported.diagnostics.begin()),
                                std::make_move_iterator(imported.diagnostics.end()));
      merged.assignments.insert(merged.assignments.end(),
                                std::make_move_iterator(imported.config.assignments.begin()),
                                std::make_move_iterator(imported.config.assignments.end()));
    }
  }
  stack.pop_back();
  parsed.config = std::move(merged);
  return parsed;
}

}  // namespace

bool ParseResult::ok() const { return !has_errors(diagnostics); }
bool ValidationResult::ok() const { return !has_errors(diagnostics); }

ValidationContext::ValidationContext(std::vector<Diagnostic>& diagnostics) : diagnostics_(diagnostics) {}

void ValidationContext::warning(SourceLocation location, std::string message) {
  diagnostics_.push_back({DiagnosticLevel::warning, std::move(location), std::move(message)});
}

void ValidationContext::error(SourceLocation location, std::string message) {
  diagnostics_.push_back({DiagnosticLevel::error, std::move(location), std::move(message)});
}

ParseResult parse_config(const std::string& input) {
  auto result = Parser(input, {}).parse();
  if (result.ok()) Resolver(result.config, result.diagnostics).run();
  return result;
}

ParseResult parse_config_file(const std::string& path) {
  std::vector<std::filesystem::path> stack;
  auto result = parse_config_file_recursive(path, stack);
  if (result.ok()) Resolver(result.config, result.diagnostics).run();
  std::sort(result.source_paths.begin(), result.source_paths.end());
  result.source_paths.erase(std::unique(result.source_paths.begin(), result.source_paths.end()),
                            result.source_paths.end());
  return result;
}

ValidationResult validate_config(const Config& config) { return validate_config(config, {}); }

ValidationResult validate_config(const Config& config, std::span<const ValidationRule> rules) {
  ValidationResult result;
  ValidationContext context(result.diagnostics);
  for (const Assignment& assignment : config.assignments) {
    if (assignment.key == "import") {
      const auto valid_path = [](const Value& value) {
        return std::holds_alternative<std::string>(value.data) ||
               std::holds_alternative<VariableReference>(value.data);
      };
      if (const auto* array = std::get_if<Value::Array>(&assignment.value.data)) {
        if (!std::all_of(array->values.begin(), array->values.end(), valid_path))
          context.error(assignment.location, "import paths must be strings or environment variable references");
      } else if (!valid_path(assignment.value)) {
        context.error(assignment.location, "import must be a path or an array of paths");
      }
    } else {
      check_variables(assignment.value, result.diagnostics);
    }
    for (const ValidationRule& rule : rules) rule(assignment, context);
  }
  return result;
}

const Value* find_assignment(const Config& config, std::string_view key) {
  const auto assignment = std::find_if(config.assignments.rbegin(), config.assignments.rend(),
                                       [key](const Assignment& item) { return item.key == key; });
  return assignment == config.assignments.rend() ? nullptr : &assignment->value;
}

const Value* find_field(const Value::Object& object, std::string_view key) {
  const auto entry = std::find(object.names.rbegin(), object.names.rend(), key);
  if (entry == object.names.rend()) return nullptr;
  return &object.values[static_cast<std::size_t>(object.names.rend() - entry - 1)];
}

const std::string* as_string(const Value& value) { return std::get_if<std::string>(&value.data); }
const bool* as_boolean(const Value& value) { return std::get_if<bool>(&value.data); }
const std::int64_t* as_integer(const Value& value) { return std::get_if<std::int64_t>(&value.data); }
const Value::Array* as_array(const Value& value) { return std::get_if<Value::Array>(&value.data); }
const Value::List* as_list(const Value& value) { return std::get_if<Value::List>(&value.data); }
const Value::Object* as_object(const Value& value) { return std::get_if<Value::Object>(&value.data); }

}  // namespace zwwm::lang
