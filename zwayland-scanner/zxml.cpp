#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <sstream>

namespace zwayland::scanner {
namespace {

enum class TokenKind : std::uint8_t { identifier, number, string, doc, trailing_doc, punct, end };

struct Token {
  TokenKind kind = TokenKind::end;
  std::string text;
  std::size_t line = 1;
  std::size_t column = 1;
};

class Lexer {
 public:
  Lexer(std::string_view source, std::string_view path) : source_(source), path_(path) {}

  std::vector<Token> tokens() {
    std::vector<Token> result;
    while (true) {
      skip_trivia();
      Token token{TokenKind::end, {}, line_, column_};
      if (at_end()) { result.push_back(std::move(token)); return result; }
      const char character = peek();
      if (starts("///<")) {
        token.kind = TokenKind::trailing_doc;
        advance(4);
        token.text = doc_line();
        // A wrapped summary continues on following `///<` lines.
        while (next_line_starts("///<")) {
          advance(4);
          const auto more = doc_line();
          if (!more.empty()) token.text += (token.text.empty() ? "" : " ") + more;
        }
      } else if (starts("///") && peek(3) != '/') {
        token.kind = TokenKind::doc;
        token.text = doc_lines();
      } else if (character == 'R' && peek(1) == '"') {
        token.kind = TokenKind::string;
        token.text = raw_string();
      } else if (std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_') {
        token.kind = TokenKind::identifier;
        while (!at_end() && word_char(peek())) token.text += next();
      } else if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
        token.kind = TokenKind::number;
        while (!at_end() && word_char(peek())) token.text += next();
      } else if (character == '"') {
        token.kind = TokenKind::string;
        token.text = string();
      } else if (starts("[[") || starts("]]")) {
        token.kind = TokenKind::punct;
        token.text = source_.substr(position_, 2);
        advance(2);
      } else if (std::string_view("{}()<>,;=?.~-[]").contains(character)) {
        token.kind = TokenKind::punct;
        token.text = std::string(1, next());
      } else {
        fail("unexpected character '" + std::string(1, character) + "'");
      }
      result.push_back(std::move(token));
    }
  }

 private:
  [[noreturn]] void fail(const std::string& message) const {
    throw ParseError(std::string(path_) + ":" + std::to_string(line_) + ":" + std::to_string(column_) + ": " + message);
  }
  [[nodiscard]] bool at_end() const { return position_ >= source_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const {
    return position_ + ahead < source_.size() ? source_[position_ + ahead] : '\0';
  }
  [[nodiscard]] bool starts(std::string_view text) const { return source_.substr(position_).starts_with(text); }
  static bool word_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
  }
  char next() {
    const char character = source_[position_++];
    if (character == '\n') { ++line_; column_ = 1; }
    else ++column_;
    return character;
  }
  void advance(std::size_t count) { while (count-- > 0 && !at_end()) next(); }
  void skip_trivia() {
    while (!at_end()) {
      if (std::isspace(static_cast<unsigned char>(peek())) != 0) next();
      else if (starts("////") || (starts("//") && !starts("///"))) { while (!at_end() && peek() != '\n') next(); }
      else if (starts("/*")) {
        advance(2);
        while (!starts("*/")) { if (at_end()) fail("unterminated block comment"); next(); }
        advance(2);
      } else return;
    }
  }
  std::string doc_line() {
    if (peek() == ' ') next();
    std::string line;
    while (!at_end() && peek() != '\n') line += next();
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
    return line;
  }
  // Moves to the next line's `prefix` when that line starts with it (after indentation).
  bool next_line_starts(std::string_view prefix, std::initializer_list<std::string_view> excluded = {}) {
    std::size_t ahead = position_;
    while (ahead < source_.size() && (source_[ahead] == ' ' || source_[ahead] == '\t' || source_[ahead] == '\r')) ++ahead;
    if (ahead >= source_.size() || source_[ahead] != '\n') return false;
    ++ahead;
    while (ahead < source_.size() && (source_[ahead] == ' ' || source_[ahead] == '\t')) ++ahead;
    const auto rest = source_.substr(ahead);
    if (!rest.starts_with(prefix)) return false;
    for (const auto other : excluded) if (rest.starts_with(other)) return false;
    while (position_ < ahead) next();
    return true;
  }
  // Joins consecutive `///` lines; a blank line or any other token ends the block.
  std::string doc_lines() {
    std::string text;
    do {
      advance(3);
      text += doc_line() + '\n';
    } while (next_line_starts("///", {"////", "///<"}));
    text.pop_back();
    return text;
  }
  std::string string() {
    next();
    std::string result;
    while (peek() != '"') {
      if (at_end() || peek() == '\n') fail("unterminated string");
      if (peek() != '\\') { result += next(); continue; }
      next();
      const char escaped = at_end() ? '\0' : next();
      if (escaped == 'n') result += '\n';
      else if (escaped == 't') result += '\t';
      else if (escaped == '"' || escaped == '\\') result += escaped;
      else fail("unknown escape sequence");
    }
    next();
    return result;
  }
  // C++ raw string: R"delim( ... )delim".
  std::string raw_string() {
    advance(2);
    std::string delimiter;
    while (peek() != '(') {
      if (at_end() || delimiter.size() > 16 || std::string_view(" )\\\t\n").contains(peek())) fail("invalid raw string delimiter");
      delimiter += next();
    }
    next();
    const std::string closer = ")" + delimiter + "\"";
    std::string result;
    while (!starts(closer)) {
      if (at_end()) fail("unterminated raw string");
      result += next();
    }
    advance(closer.size());
    return result;
  }

  std::string_view source_;
  std::string_view path_;
  std::size_t position_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

struct Attribute {
  std::string name;
  std::optional<std::uint32_t> value;
  std::size_t line = 1;
  std::size_t column = 1;
};

Doc to_doc(std::string_view text) {
  Doc doc;
  const auto blank = text.find("\n\n");
  const auto first = text.substr(0, blank == std::string_view::npos ? text.size() : blank);
  if (!text.starts_with('\n')) {
    for (const char character : first) doc.summary += character == '\n' ? ' ' : character;
    text = blank == std::string_view::npos ? std::string_view() : text.substr(blank + 2);
  }
  doc.body = normalize_text(text);
  return doc;
}

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::string_view path) : tokens_(std::move(tokens)), path_(path) {}

  Protocol protocol() {
    Protocol result;
    result.doc = leading_doc();
    keyword("protocol");
    result.name = identifier("protocol name");
    punct(";");
    while (current().kind != TokenKind::end) {
      if (is_keyword("copyright")) {
        advance();
        if (current().kind != TokenKind::string) fail("expected a string after copyright");
        std::string text;
        while (current().kind == TokenKind::string) text += advance().text;
        punct(";");
        if (!result.copyright.empty()) fail("copyright is already set");
        result.copyright = normalize_text(text);
        continue;
      }
      result.interfaces.push_back(interface());
    }
    return result;
  }

 private:
  [[noreturn]] void fail(const std::string& message) const { fail_at(current().line, current().column, message); }
  [[noreturn]] void fail_at(std::size_t line, std::size_t column, const std::string& message) const {
    throw ParseError(std::string(path_) + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + message);
  }
  [[nodiscard]] const Token& current() const { return tokens_[index_]; }
  const Token& advance() {
    const Token& token = tokens_[index_];
    if (index_ + 1 < tokens_.size()) ++index_;
    return token;
  }
  [[nodiscard]] bool is_punct(std::string_view text) const {
    return current().kind == TokenKind::punct && current().text == text;
  }
  [[nodiscard]] bool is_keyword(std::string_view text) const {
    return current().kind == TokenKind::identifier && current().text == text;
  }
  std::string describe() const {
    switch (current().kind) {
      case TokenKind::end: return "end of file";
      case TokenKind::doc: return "doc comment";
      case TokenKind::trailing_doc: return "///< comment";
      case TokenKind::string: return "string";
      default: return "'" + current().text + "'";
    }
  }
  void punct(std::string_view text) {
    if (!is_punct(text)) fail("expected '" + std::string(text) + "', found " + describe());
    advance();
  }
  bool accept(std::string_view text) {
    if (!is_punct(text)) return false;
    advance();
    return true;
  }
  void keyword(std::string_view text) {
    if (!is_keyword(text)) fail("expected '" + std::string(text) + "', found " + describe());
    advance();
  }
  std::string identifier(std::string_view what) {
    if (current().kind != TokenKind::identifier) fail("expected " + std::string(what) + ", found " + describe());
    return advance().text;
  }
  std::uint32_t number(std::string_view what) {
    if (current().kind != TokenKind::number) fail("expected " + std::string(what) + ", found " + describe());
    const auto& text = current().text;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) fail("invalid " + std::string(what));
    advance();
    return value;
  }
  Doc leading_doc() { return current().kind == TokenKind::doc ? to_doc(advance().text) : Doc{}; }
  void trailing_doc(Doc& doc) {
    if (current().kind != TokenKind::trailing_doc) return;
    if (!doc.summary.empty()) fail("summary is already given by a leading doc comment");
    doc.summary = advance().text;
  }

  std::vector<Attribute> attributes() {
    std::vector<Attribute> result;
    while (is_punct("[[")) {
      advance();
      do {
        Attribute attribute{{}, std::nullopt, current().line, current().column};
        attribute.name = identifier("attribute name");
        if (accept("(")) {
          attribute.value = number("attribute value");
          punct(")");
        }
        result.push_back(std::move(attribute));
      } while (accept(","));
      punct("]]");
    }
    return result;
  }
  // Applies attributes to the given slots; anything else is rejected where it was written.
  void apply(const std::vector<Attribute>& attributes, bool* flag, std::string_view flag_name,
             std::uint32_t* since, std::uint32_t* deprecated) const {
    for (const auto& attribute : attributes) {
      const bool takes_value = attribute.name == "since" || attribute.name == "deprecated";
      std::uint32_t* slot = attribute.name == "since" ? since : attribute.name == "deprecated" ? deprecated : nullptr;
      if (flag != nullptr && attribute.name == flag_name) {
        if (attribute.value) fail_at(attribute.line, attribute.column, attribute.name + " takes no value");
        *flag = true;
      } else if (takes_value && slot != nullptr) {
        if (!attribute.value || *attribute.value == 0)
          fail_at(attribute.line, attribute.column, attribute.name + " needs a version, e.g. " + attribute.name + "(2)");
        *slot = *attribute.value;
      } else {
        fail_at(attribute.line, attribute.column, "attribute '" + attribute.name + "' is not allowed here");
      }
    }
  }

  Interface interface() {
    Interface result;
    result.doc = leading_doc();
    apply(attributes(), &result.frozen, "frozen", nullptr, nullptr);
    keyword("interface");
    result.name = identifier("interface name");
    keyword("version");
    result.version = number("interface version");
    punct("{");
    while (!accept("}")) {
      Doc doc = leading_doc();
      const auto attributes = this->attributes();
      if (is_keyword("enum")) result.enums.push_back(enumeration(std::move(doc), attributes));
      else if (is_keyword("request")) result.requests.push_back(message(std::move(doc), attributes));
      else if (is_keyword("event")) result.events.push_back(message(std::move(doc), attributes));
      else fail("expected 'enum', 'request' or 'event', found " + describe());
    }
    accept(";");
    return result;
  }

  // Parses one list separator, letting a `///<` summary sit before or after the comma.
  bool separator(Doc& doc, std::string_view closer) {
    trailing_doc(doc);
    const bool comma = accept(",");
    trailing_doc(doc);
    if (!comma && !is_punct(closer)) fail("expected ',' or '" + std::string(closer) + "', found " + describe());
    return comma;
  }

  Enum enumeration(Doc doc, const std::vector<Attribute>& attributes) {
    Enum result;
    result.doc = std::move(doc);
    apply(attributes, &result.bitfield, "bitfield", nullptr, nullptr);
    advance();
    result.name = identifier("enum name");
    punct("{");
    while (!accept("}")) {
      Entry entry;
      entry.doc = leading_doc();
      if (current().kind != TokenKind::identifier && current().kind != TokenKind::number)
        fail("expected an entry name, found " + describe());
      entry.name = advance().text;
      apply(this->attributes(), nullptr, {}, &entry.since, &entry.deprecated_since);
      punct("=");
      if (accept("-")) entry.value = "-";
      if (current().kind != TokenKind::number) fail("expected an entry value, found " + describe());
      entry.value += advance().text;
      separator(entry.doc, "}");
      result.entries.push_back(std::move(entry));
    }
    accept(";");
    return result;
  }

  Arg argument() {
    Arg arg;
    arg.doc = leading_doc();
    const auto type_line = current().line, type_column = current().column;
    const std::string type = identifier("argument type");
    auto bracket = [&](std::string& target, bool dotted) {
      if (!accept("<")) return;
      target = identifier(dotted ? "enum name" : "interface name");
      if (dotted && accept(".")) target += "." + identifier("enum name");
      punct(">");
    };
    if (type == "int" || type == "uint") { arg.type = type; bracket(arg.enum_name, true); }
    else if (type == "fixed" || type == "string" || type == "array" || type == "fd") arg.type = type;
    else if (type == "object") { arg.type = type; bracket(arg.interface, false); }
    else if (type == "new") { arg.type = "new_id"; bracket(arg.interface, false); }
    else fail_at(type_line, type_column, "unknown argument type '" + type + "'");
    arg.nullable = accept("?");
    arg.name = identifier("argument name");
    return arg;
  }

  Message message(Doc doc, const std::vector<Attribute>& attributes) {
    Message result;
    result.doc = std::move(doc);
    apply(attributes, nullptr, {}, &result.since, &result.deprecated_since);
    advance();
    if (accept("~")) result.type = "destructor";
    result.name = identifier("message name");
    punct("(");
    while (!accept(")")) {
      result.args.push_back(argument());
      separator(result.args.back().doc, ")");
    }
    punct(";");
    trailing_doc(result.doc);
    return result;
  }

  std::vector<Token> tokens_;
  std::size_t index_ = 0;
  std::string_view path_;
};

constexpr std::size_t line_limit = 100;

// Breaks a summary at spaces into lines of at most `width` characters where possible.
std::vector<std::string> wrap(std::string_view text, std::size_t width) {
  std::vector<std::string> lines;
  while (text.size() > width) {
    auto cut = text.rfind(' ', width);
    if (cut == std::string_view::npos || cut == 0) cut = text.find(' ', width);
    if (cut == std::string_view::npos) break;
    lines.emplace_back(text.substr(0, cut));
    text.remove_prefix(cut + 1);
  }
  lines.emplace_back(text);
  return lines;
}

void write_doc(std::ostream& out, const Doc& doc, const std::string& indent) {
  if (doc.empty()) return;
  if (doc.summary.empty()) out << indent << "///\n";
  else for (const auto& line : wrap(doc.summary, std::max<std::size_t>(40, line_limit - indent.size() - 4)))
    out << indent << "/// " << line << '\n';
  if (doc.body.empty()) return;
  if (!doc.summary.empty()) out << indent << "///\n";
  std::istringstream lines(doc.body);
  for (std::string line; std::getline(lines, line);) out << indent << "///" << (line.empty() ? "" : " " + line) << '\n';
}

std::string attribute_list(std::initializer_list<std::pair<std::string_view, std::uint32_t>> values, bool flag = false,
                           std::string_view flag_name = {}) {
  std::string result;
  if (flag) result += flag_name;
  for (const auto& [name, value] : values) if (value != 0) {
    if (!result.empty()) result += ", ";
    result += std::string(name) + "(" + std::to_string(value) + ")";
  }
  return result.empty() ? result : "[[" + result + "]]";
}

std::string type_text(const Arg& arg) {
  std::string result = arg.type == "new_id" ? "new" : arg.type;
  if (!arg.interface.empty()) result += "<" + arg.interface + ">";
  if (!arg.enum_name.empty()) result += "<" + arg.enum_name + ">";
  if (arg.nullable) result += "?";
  return result;
}

// Writes list items one per line, aligning `///<` summaries; bodies become leading doc blocks.
void write_items(std::ostream& out, const std::vector<std::pair<std::string, const Doc*>>& items, const std::string& indent,
                 bool trailing_comma) {
  std::size_t width = 0;
  for (std::size_t index = 0; index < items.size(); ++index)
    width = std::max(width, items[index].first.size() + (trailing_comma || index + 1 < items.size() ? 1 : 0));
  for (std::size_t index = 0; index < items.size(); ++index) {
    const auto& [text, doc] = items[index];
    if (!doc->body.empty()) write_doc(out, *doc, indent);
    std::string line = text + (trailing_comma || index + 1 < items.size() ? "," : "");
    if (doc->body.empty() && !doc->summary.empty()) {
      const std::size_t column = indent.size() + width + 2;
      const auto summary = wrap(doc->summary, std::max<std::size_t>(40, line_limit - std::min(line_limit, column + 5)));
      line += std::string(width - line.size() + 2, ' ') + "///< " + summary.front();
      for (std::size_t part = 1; part < summary.size(); ++part)
        line += "\n" + indent + std::string(width + 2, ' ') + "///< " + summary[part];
    }
    out << indent << line << '\n';
  }
}

void write_message(std::ostream& out, const Message& message, std::string_view kind) {
  write_doc(out, message.doc, "  ");
  const auto attributes = attribute_list({{"since", message.since > 1 ? message.since : 0},
                                          {"deprecated", message.deprecated_since}});
  std::string head = "  " + (attributes.empty() ? "" : attributes + " ") + std::string(kind) + " " +
                     (message.type == "destructor" ? "~" : "") + message.name + "(";
  std::vector<std::pair<std::string, const Doc*>> args;
  std::string inline_args;
  bool documented = false;
  for (const auto& arg : message.args) {
    args.emplace_back(type_text(arg) + " " + arg.name, &arg.doc);
    inline_args += (inline_args.empty() ? "" : ", ") + args.back().first;
    documented = documented || !arg.doc.empty();
  }
  if (!documented && head.size() + inline_args.size() + 2 <= 110) {
    out << head << inline_args << ");\n";
    return;
  }
  out << head << '\n';
  write_items(out, args, "    ", false);
  out << "  );\n";
}

std::string raw_string(std::string_view text) {
  std::string delimiter;
  while (text.find(")" + delimiter + "\"") != std::string_view::npos) delimiter += "z";
  return "R\"" + delimiter + "(\n" + std::string(text) + "\n)" + delimiter + "\"";
}

}  // namespace

Protocol parse_zxml(std::string_view source, std::string_view path) {
  return Parser(Lexer(source, path).tokens(), path).protocol();
}

void write_zxml(std::ostream& out, const Protocol& protocol) {
  write_doc(out, protocol.doc, "");
  out << "protocol " << protocol.name << ";\n";
  if (!protocol.copyright.empty()) out << "\ncopyright " << raw_string(protocol.copyright) << ";\n";
  for (const auto& interface : protocol.interfaces) {
    out << '\n';
    write_doc(out, interface.doc, "");
    const auto attributes = attribute_list({}, interface.frozen, "frozen");
    out << (attributes.empty() ? "" : attributes + " ") << "interface " << interface.name << " version "
        << interface.version << " {\n";
    // Undocumented members of one kind stay together; anything else gets a blank line.
    std::string_view previous_kind;
    bool previous_documented = false;
    auto separate = [&](std::string_view kind, const Doc& doc) {
      if (!previous_kind.empty() && (kind != previous_kind || previous_documented || !doc.empty())) out << '\n';
      previous_kind = kind;
      previous_documented = !doc.empty();
    };
    for (const auto& enumeration : interface.enums) {
      separate("enum", enumeration.doc);
      previous_documented = true;
      write_doc(out, enumeration.doc, "  ");
      const auto flag = attribute_list({}, enumeration.bitfield, "bitfield");
      out << "  " << (flag.empty() ? "" : flag + " ") << "enum " << enumeration.name << " {\n";
      std::vector<std::pair<std::string, const Doc*>> entries;
      for (const auto& entry : enumeration.entries) {
        const auto entry_attributes = attribute_list({{"since", entry.since > 1 ? entry.since : 0},
                                                      {"deprecated", entry.deprecated_since}});
        entries.emplace_back(entry.name + (entry_attributes.empty() ? "" : " " + entry_attributes) + " = " + entry.value,
                             &entry.doc);
      }
      write_items(out, entries, "    ", true);
      out << "  }\n";
    }
    for (const auto& request : interface.requests) { separate("request", request.doc); write_message(out, request, "request"); }
    for (const auto& event : interface.events) { separate("event", event.doc); write_message(out, event, "event"); }
    out << "}\n";
  }
}

}  // namespace zwayland::scanner
