#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
#include <ostream>
#include <set>

namespace zwayland::scanner {
namespace {

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<std::unique_ptr<Node>> children;
  std::string text;
  std::size_t line = 1;

  [[nodiscard]] const std::string* attribute(std::string_view key) const {
    for (const auto& [name, value] : attributes) if (name == key) return &value;
    return nullptr;
  }
};

// Reads the XML subset Wayland protocols use: elements, attributes, text, comments and CDATA.
class XmlReader {
 public:
  XmlReader(std::string_view source, std::string_view path) : source_(source), path_(path) {}

  std::unique_ptr<Node> document() {
    std::unique_ptr<Node> root;
    while (true) {
      skip_misc();
      if (at_end()) break;
      if (root != nullptr) fail("content after the root element");
      if (peek() != '<') fail("expected the root element");
      root = element();
    }
    if (root == nullptr) fail("no root element");
    return root;
  }

 private:
  [[noreturn]] void fail(const std::string& message) const {
    throw ParseError(std::string(path_) + ":" + std::to_string(line_) + ": " + message);
  }
  [[nodiscard]] bool at_end() const { return position_ >= source_.size(); }
  [[nodiscard]] char peek(std::size_t ahead = 0) const {
    return position_ + ahead < source_.size() ? source_[position_ + ahead] : '\0';
  }
  [[nodiscard]] bool starts(std::string_view text) const { return source_.substr(position_).starts_with(text); }
  char next() {
    if (at_end()) fail("unexpected end of file");
    const char character = source_[position_++];
    if (character == '\n') ++line_;
    return character;
  }
  void skip_space() { while (!at_end() && std::string_view(" \t\r\n").contains(peek())) next(); }
  void skip_past(std::string_view terminator) {
    while (!starts(terminator)) next();
    for (std::size_t index = 0; index < terminator.size(); ++index) next();
  }
  // Skips whitespace, declarations, processing instructions and comments between elements.
  void skip_misc() {
    while (true) {
      skip_space();
      if (starts("<?")) skip_past("?>");
      else if (starts("<!--")) skip_past("-->");
      else if (starts("<!DOCTYPE")) skip_past(">");
      else return;
    }
  }
  static bool name_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || std::string_view("_-:.").contains(character);
  }
  std::string name() {
    std::string result;
    while (!at_end() && name_char(peek())) result += next();
    if (result.empty()) fail("expected a name");
    return result;
  }
  void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x110000) {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else fail("character reference out of range");
  }
  void entity(std::string& out) {
    next();
    std::string name;
    while (peek() != ';') {
      if (at_end() || name.size() > 10) fail("unterminated entity reference");
      name += next();
    }
    next();
    if (name == "lt") out += '<';
    else if (name == "gt") out += '>';
    else if (name == "amp") out += '&';
    else if (name == "quot") out += '"';
    else if (name == "apos") out += '\'';
    else if (name.starts_with("#")) {
      const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
      const std::string_view digits = std::string_view(name).substr(hex ? 2 : 1);
      std::uint32_t code = 0;
      const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
      if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
        fail("invalid character reference &" + name + ";");
      append_utf8(out, code);
    } else fail("unknown entity &" + name + ";");
  }
  std::string quoted() {
    const char quote = next();
    if (quote != '"' && quote != '\'') fail("expected a quoted attribute value");
    std::string result;
    while (peek() != quote) {
      if (at_end()) fail("unterminated attribute value");
      if (peek() == '&') entity(result);
      else if (peek() == '<') fail("'<' in attribute value");
      else result += next();
    }
    next();
    return result;
  }
  std::unique_ptr<Node> element() {
    auto node = std::make_unique<Node>();
    node->line = line_;
    next();
    node->name = name();
    while (true) {
      skip_space();
      if (starts("/>")) { next(); next(); return node; }
      if (peek() == '>') { next(); break; }
      std::string key = name();
      skip_space();
      if (next() != '=') fail("expected '=' after attribute " + key);
      skip_space();
      if (node->attribute(key) != nullptr) fail("duplicate attribute " + key);
      node->attributes.emplace_back(std::move(key), quoted());
    }
    while (true) {
      if (at_end()) fail("unclosed element <" + node->name + ">");
      if (starts("</")) {
        next(); next();
        if (name() != node->name) fail("mismatched closing tag for <" + node->name + ">");
        skip_space();
        if (next() != '>') fail("expected '>'");
        return node;
      }
      if (starts("<!--")) skip_past("-->");
      else if (starts("<![CDATA[")) {
        for (int index = 0; index < 9; ++index) next();
        while (!starts("]]>")) node->text += next();
        next(); next(); next();
      } else if (starts("<?")) skip_past("?>");
      else if (peek() == '<') node->children.push_back(element());
      else if (peek() == '&') entity(node->text);
      else node->text += next();
    }
  }

  std::string_view source_;
  std::string_view path_;
  std::size_t position_ = 0;
  std::size_t line_ = 1;
};

class Builder {
 public:
  explicit Builder(std::string_view path) : path_(path) {}

  Protocol protocol(const Node& root) {
    if (root.name != "protocol") fail(root, "root element must be <protocol>");
    Protocol result{text(root, "name"), {}, {}, {}};
    for (const auto& child : root.children) {
      if (child->name == "copyright") result.copyright = normalize_text(child->text);
      else if (child->name == "description") result.doc = doc(*child);
      else if (child->name == "interface") result.interfaces.push_back(interface(*child));
    }
    return result;
  }

 private:
  [[noreturn]] void fail(const Node& node, const std::string& message) const {
    throw ParseError(std::string(path_) + ":" + std::to_string(node.line) + ": " + message);
  }
  static std::string text(const Node& node, std::string_view key) {
    const auto* value = node.attribute(key);
    return value == nullptr ? std::string() : *value;
  }
  bool flag(const Node& node, std::string_view key) const {
    const auto value = text(node, key);
    if (!value.empty() && value != "true" && value != "false")
      fail(node, std::string(key) + " must be \"true\" or \"false\"");
    return value == "true";
  }
  std::uint32_t number(const Node& node, std::string_view key, std::uint32_t fallback) const {
    const auto value = text(node, key);
    if (value.empty()) return fallback;
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      fail(node, "invalid numeric attribute " + std::string(key) + "=\"" + value + "\"");
    return result;
  }
  // Summaries are one line: wrapped or padded attribute values collapse to single spaces.
  static std::string summary(const Node& node) {
    std::string result;
    for (const char character : text(node, "summary")) {
      if (!std::string_view(" \t\r\n").contains(character)) result += character;
      else if (!result.empty() && result.back() != ' ') result += ' ';
    }
    if (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
  }
  static Doc doc(const Node& node) { return {summary(node), normalize_text(node.text)}; }
  static Doc child_doc(const Node& node) {
    Doc result{summary(node), {}};
    for (const auto& child : node.children) if (child->name == "description") {
      const Doc described = doc(*child);
      if (result.summary.empty()) result.summary = described.summary;
      result.body = described.body;
    }
    return result;
  }
  Message message(const Node& node) const {
    Message result{text(node, "name"), text(node, "type"), number(node, "since", 1),
                   number(node, "deprecated-since", 0), {}, child_doc(node)};
    for (const auto& child : node.children) if (child->name == "arg")
      result.args.push_back({text(*child, "name"), text(*child, "type"), text(*child, "interface"),
                             text(*child, "enum"), flag(*child, "allow-null"), child_doc(*child)});
    return result;
  }
  Interface interface(const Node& node) const {
    Interface result;
    result.name = text(node, "name");
    result.version = number(node, "version", 1);
    result.frozen = flag(node, "frozen");
    for (const auto& child : node.children) {
      if (child->name == "description") result.doc = doc(*child);
      else if (child->name == "request") result.requests.push_back(message(*child));
      else if (child->name == "event") result.events.push_back(message(*child));
      else if (child->name == "enum") {
        Enum enumeration{text(*child, "name"), flag(*child, "bitfield"), {}, child_doc(*child)};
        for (const auto& entry : child->children) if (entry->name == "entry")
          enumeration.entries.push_back({text(*entry, "name"), text(*entry, "value"), number(*entry, "since", 1),
                                         number(*entry, "deprecated-since", 0), child_doc(*entry)});
        result.enums.push_back(std::move(enumeration));
      }
    }
    return result;
  }

  std::string_view path_;
};

std::string escape(std::string_view text, bool attribute) {
  std::string result;
  for (const char character : text) {
    if (character == '&') result += "&amp;";
    else if (character == '<') result += "&lt;";
    else if (character == '>') result += "&gt;";
    else if (attribute && character == '"') result += "&quot;";
    else result += character;
  }
  return result;
}

void write_block(std::ostream& out, std::string_view text, const std::string& indent) {
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = std::min(text.find('\n', start), text.size());
    const auto line = text.substr(start, end - start);
    out << (line.empty() ? "" : indent) << escape(line, false) << '\n';
    start = end + 1;
  }
}

void write_description(std::ostream& out, const Doc& doc, const std::string& indent) {
  if (doc.empty()) return;
  out << indent << "<description";
  if (!doc.summary.empty()) out << " summary=\"" << escape(doc.summary, true) << '"';
  if (doc.body.empty()) { out << "/>\n"; return; }
  out << ">\n";
  write_block(out, doc.body, indent + "  ");
  out << indent << "</description>\n";
}

// Leaf elements carry a lone summary as an attribute and only nest a description for a body.
void write_leaf(std::ostream& out, const std::string& indent, const std::string& opening, const Doc& doc) {
  out << indent << '<' << opening;
  if (!doc.summary.empty()) out << " summary=\"" << escape(doc.summary, true) << '"';
  if (doc.body.empty()) { out << "/>\n"; return; }
  out << ">\n";
  write_description(out, {{}, doc.body}, indent + "  ");
  out << indent << "</" << opening.substr(0, opening.find(' ')) << ">\n";
}

void write_message(std::ostream& out, const Message& message, std::string_view kind) {
  out << "    <" << kind << " name=\"" << message.name << '"';
  if (!message.type.empty()) out << " type=\"" << message.type << '"';
  if (message.since > 1) out << " since=\"" << message.since << '"';
  if (message.deprecated_since != 0) out << " deprecated-since=\"" << message.deprecated_since << '"';
  if (message.doc.empty() && message.args.empty()) { out << "/>\n"; return; }
  out << ">\n";
  write_description(out, message.doc, "      ");
  for (const auto& arg : message.args) {
    std::string opening = "arg name=\"" + arg.name + "\" type=\"" + arg.type + '"';
    if (!arg.interface.empty()) opening += " interface=\"" + arg.interface + '"';
    if (arg.nullable) opening += " allow-null=\"true\"";
    if (!arg.enum_name.empty()) opening += " enum=\"" + arg.enum_name + '"';
    write_leaf(out, "      ", opening, arg.doc);
  }
  out << "    </" << kind << ">\n";
}

}  // namespace

std::string normalize_text(std::string_view text) {
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start <= text.size();) {
    const auto end = std::min(text.find('\n', start), text.size());
    auto line = text.substr(start, end - start);
    while (!line.empty() && std::string_view(" \t\r").contains(line.back())) line.remove_suffix(1);
    lines.push_back(line);
    start = end + 1;
  }
  while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  std::size_t indent = std::string_view::npos;
  for (const auto line : lines) if (!line.empty())
    indent = std::min(indent, line.find_first_not_of(" \t"));
  std::string result;
  for (const auto line : lines) {
    if (!result.empty()) result += '\n';
    if (!line.empty()) result += line.substr(indent);
  }
  return result;
}

void validate(const Protocol& protocol) {
  static const std::set<std::string_view> types{"int", "uint", "fixed", "string", "object", "new_id", "array", "fd"};
  if (protocol.name.empty() || protocol.interfaces.empty()) throw ParseError("empty protocol");
  for (const auto& interface : protocol.interfaces) {
    if (interface.name.empty()) throw ParseError("interface without a name");
    if (interface.version == 0) throw ParseError(interface.name + ": version must be at least 1");
    for (const auto* messages : {&interface.requests, &interface.events}) for (const auto& message : *messages) {
      const std::string where = interface.name + "." + message.name;
      if (message.name.empty()) throw ParseError(interface.name + ": message without a name");
      if (message.since > interface.version) throw ParseError(where + ": since version exceeds interface version");
      if (!message.type.empty() && message.type != "destructor") throw ParseError(where + ": unknown message type " + message.type);
      if (std::count_if(message.args.begin(), message.args.end(), [](const Arg& arg) { return arg.type == "new_id"; }) > 1)
        throw ParseError(where + ": message has more than one new_id argument");
      for (const auto& arg : message.args) {
        if (arg.name.empty()) throw ParseError(where + ": argument without a name");
        if (!types.contains(arg.type)) throw ParseError(where + ": unsupported argument type " + arg.type);
        if (arg.nullable && arg.type != "string" && arg.type != "object")
          throw ParseError(where + ": nullable is only valid for string and object arguments");
        if (!arg.interface.empty() && arg.type != "object" && arg.type != "new_id")
          throw ParseError(where + ": only object and new_id arguments name an interface");
      }
    }
    for (const auto& enumeration : interface.enums) {
      if (enumeration.name.empty()) throw ParseError(interface.name + ": enum without a name");
      for (const auto& entry : enumeration.entries)
        if (entry.name.empty() || entry.value.empty())
          throw ParseError(interface.name + "." + enumeration.name + ": entry needs a name and a value");
    }
  }
}

Protocol parse_xml(std::string_view source, std::string_view path) {
  const auto root = XmlReader(source, path).document();
  return Builder(path).protocol(*root);
}

void write_xml(std::ostream& out, const Protocol& protocol) {
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<protocol name=\"" << protocol.name << "\">\n";
  if (!protocol.copyright.empty()) {
    out << "  <copyright>\n";
    write_block(out, protocol.copyright, "    ");
    out << "  </copyright>\n";
  }
  write_description(out, protocol.doc, "  ");
  for (const auto& interface : protocol.interfaces) {
    out << "\n  <interface name=\"" << interface.name << "\" version=\"" << interface.version << '"';
    if (interface.frozen) out << " frozen=\"true\"";
    out << ">\n";
    write_description(out, interface.doc, "    ");
    for (const auto& enumeration : interface.enums) {
      out << "    <enum name=\"" << enumeration.name << '"' << (enumeration.bitfield ? " bitfield=\"true\"" : "") << ">\n";
      write_description(out, enumeration.doc, "      ");
      for (const auto& entry : enumeration.entries) {
        std::string opening = "entry name=\"" + entry.name + "\" value=\"" + entry.value + '"';
        if (entry.since > 1) opening += " since=\"" + std::to_string(entry.since) + '"';
        if (entry.deprecated_since != 0) opening += " deprecated-since=\"" + std::to_string(entry.deprecated_since) + '"';
        write_leaf(out, "      ", opening, entry.doc);
      }
      out << "    </enum>\n";
    }
    for (const auto& request : interface.requests) write_message(out, request, "request");
    for (const auto& event : interface.events) write_message(out, event, "event");
    out << "  </interface>\n";
  }
  out << "</protocol>\n";
}

}  // namespace zwayland::scanner
