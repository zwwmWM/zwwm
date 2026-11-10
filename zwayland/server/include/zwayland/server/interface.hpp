#pragma once

#include <cstdint>
#include <string_view>

namespace zwayland::server {

// Replacement for wl_interface / wl_message.
struct Message {
  const char* name;
  const char* signature;  // libwayland-style: "u?o", "n", "usun", ...
  std::uint32_t since = 1;
};

struct Interface {
  const char* name;
  std::uint32_t version;
  const Message* methods;
  std::uint32_t method_count;
  const Message* events;
  std::uint32_t event_count;
};

}  // namespace zwayland::server
