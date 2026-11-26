#pragma once

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zwayland::scanner {

struct Doc {
  std::string summary;
  std::string body;
  [[nodiscard]] bool empty() const { return summary.empty() && body.empty(); }
};

struct Arg {
  std::string name;
  std::string type;
  std::string interface;
  std::string enum_name;
  bool nullable = false;
  Doc doc;
};
struct Message {
  std::string name;
  std::string type;
  std::uint32_t since = 1;
  std::uint32_t deprecated_since = 0;
  std::vector<Arg> args;
  Doc doc;
};
struct Entry {
  std::string name;
  std::string value;
  std::uint32_t since = 1;
  std::uint32_t deprecated_since = 0;
  Doc doc;
};
struct Enum {
  std::string name;
  bool bitfield = false;
  std::vector<Entry> entries;
  Doc doc;
};
struct Interface {
  std::string name;
  std::uint32_t version = 1;
  bool frozen = false;
  std::vector<Message> requests;
  std::vector<Message> events;
  std::vector<Enum> enums;
  Doc doc;
};
struct Protocol {
  std::string name;
  std::string copyright;
  Doc doc;
  std::vector<Interface> interfaces;
};

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Dedents a text block and trims blank lines at both ends.
std::string normalize_text(std::string_view text);
// Shared semantic checks for both source formats.
void validate(const Protocol& protocol);

Protocol parse_xml(std::string_view source, std::string_view path);
Protocol parse_zxml(std::string_view source, std::string_view path);
void write_xml(std::ostream& out, const Protocol& protocol);
void write_zxml(std::ostream& out, const Protocol& protocol);

}  // namespace zwayland::scanner
