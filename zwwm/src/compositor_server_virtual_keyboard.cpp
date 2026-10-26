#include "compositor_server_internal.hpp"
#include <zwwm-virtual-keyboard-v1-zwayland-server.h>
#include <virtual-keyboard-unstable-v1-zwayland-server.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {
struct VirtualKeyboard { SeatState* seat; std::uint64_t source; };

void send_keymap(zwayland::server::Resource& resource, SeatState* seat) {
  const auto fd = keymap_fd(*seat);
  if (!fd) { resource.client->post_no_memory(); return; }
  protocol::zwwm_virtual_keyboard_v1_send_keymap(resource, protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                               fd.get(), static_cast<std::uint32_t>(seat->keymap.size() + 1));
}

struct KeyboardHandler {
  void key(zwayland::server::Client&, zwayland::server::Resource& resource,
           std::uint32_t time, std::uint32_t key, std::uint32_t state) {
    auto* keyboard = resource.data<VirtualKeyboard>();
    if (key > KEY_MAX) {
      resource.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEY, "invalid evdev keycode");
      return;
    }
    if (state > protocol::WL_KEYBOARD_KEY_STATE_PRESSED) {
      resource.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_STATE, "invalid key state");
      return;
    }
    if (keyboard->seat->session_lock != nullptr) return;
    keyboard->seat->compositor->keyboard_key_from_source(keyboard->source, time, key, state);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

struct ManagerHandler {
  void create_keyboard(zwayland::server::Client& client, zwayland::server::Resource& manager,
                        std::uint32_t id, zwayland::server::Resource* seat_resource) {
    auto* seat = manager.data<SeatState>();
    if (seat_resource == nullptr || seat_resource->client != &client ||
        seat_resource->interface != &protocol::wl_seat_interface || seat_resource->data<SeatState>() != seat) {
      manager.post_error(protocol::ZWWM_VIRTUAL_KEYBOARD_MANAGER_V1_ERROR_INVALID_SEAT, "unknown seat");
      return;
    }
    auto* resource = client.create_resource(&protocol::zwwm_virtual_keyboard_v1_interface, id, 1);
    auto* keyboard = resource == nullptr ? nullptr : new (std::nothrow) VirtualKeyboard{seat, seat->next_keyboard_source++};
    if (keyboard == nullptr) {
      if (resource != nullptr) resource->destroy();
      client.post_no_memory();
      return;
    }
    resource->set_data(keyboard);
    resource->set_handler(protocol::zwwm_virtual_keyboard_v1_handler(KeyboardHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& resource) {
      auto* keyboard = resource.data<VirtualKeyboard>();
      std::erase(keyboard->seat->virtual_keyboards, &resource);
      keyboard->seat->keyboard_source_clients.erase(keyboard->source);
      keyboard->seat->compositor->release_keyboard_source(keyboard->source);
      delete keyboard;
    });
    seat->virtual_keyboards.push_back(resource);
    seat->keyboard_source_clients[keyboard->source] = &client;
    send_keymap(*resource, seat);
    publish_virtual_modifiers(seat);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

// zwp_virtual_keyboard_v1 carries its own keymap; matching ones join the seat's key path so binds still apply.
struct VirtualKeyboardV1 {
  SeatState* seat;
  std::uint64_t source;
  std::string keymap;
  std::array<std::uint32_t, 4> modifiers{};
};

bool seat_keymap(const VirtualKeyboardV1& keyboard) { return keyboard.keymap == keyboard.seat->keymap; }

void send_foreign_modifiers(VirtualKeyboardV1& keyboard) {
  auto& seat = *keyboard.seat;
  if (seat.keyboard_focus == nullptr) return;
  auto* client = seat.keyboard_focus->resource->client;
  use_keyboard_keymap(seat, client, &keyboard, keyboard.keymap);
  const auto serial = seat.display->next_serial();
  for (auto* resource : seat.keyboards) if (resource->client == client)
    protocol::wl_keyboard_send_modifiers(*resource, serial, keyboard.modifiers[0], keyboard.modifiers[1],
                                         keyboard.modifiers[2], keyboard.modifiers[3]);
}

struct KeyboardV1Handler {
  static VirtualKeyboardV1* state(zwayland::server::Resource& resource) { return resource.data<VirtualKeyboardV1>(); }
  void keymap(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t format, int fd,
              std::uint32_t size) {
    UniqueFd owned(fd);
    auto* keyboard = state(resource);
    if (format != protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) return;
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, owned.get(), 0);
    if (mapped == MAP_FAILED) return;
    keyboard->keymap.assign(static_cast<const char*>(mapped), strnlen(static_cast<const char*>(mapped), size));
    munmap(mapped, size);
  }
  void key(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t time, std::uint32_t key,
           std::uint32_t key_state) {
    auto* keyboard = state(resource);
    if (keyboard->keymap.empty()) {
      resource.post_error(protocol::ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, "key sent before keymap");
      return;
    }
    auto& seat = *keyboard->seat;
    if (seat.session_lock != nullptr || key_state > protocol::WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (seat_keymap(*keyboard)) { seat.compositor->keyboard_key_from_source(keyboard->source, time, key, key_state); return; }
    if (seat.keyboard_focus == nullptr) return;
    auto* client = seat.keyboard_focus->resource->client;
    use_keyboard_keymap(seat, client, keyboard, keyboard->keymap);
    const auto serial = seat.display->next_serial();
    for (auto* target : seat.keyboards) if (target->client == client) protocol::wl_keyboard_send_key(*target, serial, time, key, key_state);
  }
  void modifiers(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t depressed,
                 std::uint32_t latched, std::uint32_t locked, std::uint32_t group) {
    auto* keyboard = state(resource);
    keyboard->modifiers = {depressed, latched, locked, group};
    if (keyboard->keymap.empty() || keyboard->seat->session_lock != nullptr) return;
    if (seat_keymap(*keyboard)) keyboard->seat->compositor->keyboard_modifiers(depressed, latched, locked, group);
    else send_foreign_modifiers(*keyboard);
  }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

struct ManagerV1Handler {
  void create_virtual_keyboard(zwayland::server::Client& client, zwayland::server::Resource& manager,
                               zwayland::server::Resource* seat_resource, std::uint32_t id) {
    auto* seat = manager.data<SeatState>();
    if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat) {
      manager.post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown seat");
      return;
    }
    auto* resource = client.create_resource(&protocol::zwp_virtual_keyboard_v1_interface, id, 1);
    auto* keyboard = resource == nullptr ? nullptr : new (std::nothrow) VirtualKeyboardV1{seat, seat->next_keyboard_source++, {}};
    if (keyboard == nullptr) {
      if (resource != nullptr) resource->destroy();
      client.post_no_memory();
      return;
    }
    seat->keyboard_source_clients[keyboard->source] = &client;
    resource->set_data(keyboard);
    resource->set_handler(protocol::zwp_virtual_keyboard_v1_handler(KeyboardV1Handler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) {
      auto* keyboard = destroyed.data<VirtualKeyboardV1>();
      auto& seat = *keyboard->seat;
      seat.compositor->release_keyboard_source(keyboard->source);
      seat.keyboard_source_clients.erase(keyboard->source);
      for (auto it = seat.keyboard_keymaps.begin(); it != seat.keyboard_keymaps.end();)
        it = it->second == keyboard ? seat.keyboard_keymaps.erase(it) : std::next(it);
      delete keyboard;
    });
  }
};
}  // namespace

void publish_virtual_keymap(SeatState* seat) {
  for (auto* resource : seat->virtual_keyboards) send_keymap(*resource, seat);
  publish_virtual_modifiers(seat);
}

void publish_virtual_modifiers(SeatState* seat) {
  if (seat->xkb_state_handle == nullptr) return;
  for (auto* resource : seat->virtual_keyboards)
    protocol::zwwm_virtual_keyboard_v1_send_modifiers(*resource,
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(seat->xkb_state_handle, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(seat->xkb_state_handle, XKB_STATE_LAYOUT_EFFECTIVE));
}

void reset_virtual_keyboards(SeatState* seat) {
  for (auto* resource : seat->virtual_keyboards) {
    seat->compositor->release_keyboard_source(resource->data<VirtualKeyboard>()->source);
    protocol::zwwm_virtual_keyboard_v1_send_reset(*resource);
  }
}

void bind_virtual_keyboard_manager(zwayland::server::Client* client, void* data, std::uint32_t,
                                   std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwwm_virtual_keyboard_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwwm_virtual_keyboard_manager_v1_handler(ManagerHandler{}));
}
void bind_virtual_keyboard_v1_manager(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwp_virtual_keyboard_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwp_virtual_keyboard_manager_v1_handler(ManagerV1Handler{}));
}
}  // namespace zwwm::detail
