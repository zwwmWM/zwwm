#include "compositor_server_internal.hpp"

#include <input-method-unstable-v2-zwayland-server.h>
#include <text-input-v3-zwayland-server.h>

#include <algorithm>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {

bool focused(const TextInputState* input) { return input->focus != nullptr && input->focus_alive && *input->focus_alive; }

void send_popup_rectangles(InputMethodState* method) {
  if (method->active == nullptr) return;
  const auto& rect = method->active->current.cursor_rectangle;
  for (auto* popup : method->popups)
    protocol::zwp_input_popup_surface_v2_send_text_input_rectangle(*popup, rect.x, rect.y, rect.width, rect.height);
}

void deactivate(InputMethodState* method) {
  if (method == nullptr || method->active == nullptr) return;
  method->active = nullptr;
  protocol::zwp_input_method_v2_send_deactivate(*method->resource);
  protocol::zwp_input_method_v2_send_done(*method->resource);
}

void relay(TextInputState* input) {
  auto* method = input->seat->input_method;
  if (method == nullptr) return;
  if (!input->current.enabled || !focused(input)) {
    if (method->active == input) deactivate(method);
    return;
  }
  if (method->active != input) {
    if (method->active != nullptr) protocol::zwp_input_method_v2_send_deactivate(*method->resource);
    method->active = input;
    protocol::zwp_input_method_v2_send_activate(*method->resource);
  }
  const auto& state = input->current;
  if (state.surrounding_text)
    protocol::zwp_input_method_v2_send_surrounding_text(*method->resource, *state.surrounding_text, state.cursor, state.anchor);
  protocol::zwp_input_method_v2_send_text_change_cause(*method->resource, state.cause);
  protocol::zwp_input_method_v2_send_content_type(*method->resource, state.hint, state.purpose);
  protocol::zwp_input_method_v2_send_done(*method->resource);
  send_popup_rectangles(method);
}

void focus(TextInputState* input, SurfaceState* surface) {
  if (focused(input) && input->focus != surface) {
    protocol::zwp_text_input_v3_send_leave(*input->resource, input->focus->resource);
    input->focus = nullptr;
    input->focus_alive.reset();
    input->current.enabled = false;
    relay(input);
  }
  if (surface == nullptr || input->focus == surface || surface->resource->client != input->resource->client) return;
  input->focus = surface;
  input->focus_alive = surface->alive;
  protocol::zwp_text_input_v3_send_enter(*input->resource, surface->resource);
}

struct TextInputHandler {
  static TextInputState* state(zwayland::server::Resource& resource) { return resource.data<TextInputState>(); }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void enable(zwayland::server::Client&, zwayland::server::Resource& resource) {
    state(resource)->pending = TextInputContent{};
    state(resource)->pending.enabled = true;
  }
  void disable(zwayland::server::Client&, zwayland::server::Resource& resource) { state(resource)->pending.enabled = false; }
  void set_surrounding_text(zwayland::server::Client&, zwayland::server::Resource& resource, const std::string& text,
                            std::int32_t cursor, std::int32_t anchor) {
    auto& pending = state(resource)->pending;
    pending.surrounding_text = text;
    pending.cursor = static_cast<std::uint32_t>(std::max(cursor, 0));
    pending.anchor = static_cast<std::uint32_t>(std::max(anchor, 0));
  }
  void set_text_change_cause(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t cause) {
    state(resource)->pending.cause = cause;
  }
  void set_content_type(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t hint, std::uint32_t purpose) {
    state(resource)->pending.hint = hint;
    state(resource)->pending.purpose = purpose;
  }
  void set_cursor_rectangle(zwayland::server::Client&, zwayland::server::Resource& resource, std::int32_t x, std::int32_t y,
                            std::int32_t width, std::int32_t height) {
    state(resource)->pending.cursor_rectangle = {x, y, width, height};
  }
  void set_available_actions(zwayland::server::Client&, zwayland::server::Resource&, std::span<const std::byte>) {}
  void show_input_panel(zwayland::server::Client&, zwayland::server::Resource&) {}
  void hide_input_panel(zwayland::server::Client&, zwayland::server::Resource&) {}
  void commit(zwayland::server::Client&, zwayland::server::Resource& resource) {
    auto* input = state(resource);
    ++input->commits;
    input->current = input->pending;
    input->pending.cause = 0;
    relay(input);
  }
};

void text_input_destroyed(zwayland::server::Resource& resource) {
  auto* input = resource.data<TextInputState>();
  if (input == nullptr) return;
  if (input->seat->input_method != nullptr && input->seat->input_method->active == input) deactivate(input->seat->input_method);
  std::erase(input->seat->text_inputs, input);
  delete input;
}

struct TextInputManagerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_text_input(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id,
                      zwayland::server::Resource* seat_resource) {
    auto* seat = manager.data<SeatState>();
    if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat) {
      manager.post_error(protocol::WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown seat");
      return;
    }
    auto* resource = client.create_resource(&protocol::zwp_text_input_v3_interface, id, manager.version);
    auto* input = resource == nullptr ? nullptr : new (std::nothrow) TextInputState;
    if (input == nullptr) {
      if (resource != nullptr) resource->destroy();
      client.post_no_memory();
      return;
    }
    input->seat = seat;
    input->resource = resource;
    seat->text_inputs.push_back(input);
    resource->set_data(input);
    resource->set_handler(protocol::zwp_text_input_v3_handler(TextInputHandler{}));
    resource->set_destroy_handler(text_input_destroyed);
    focus(input, seat->keyboard_focus);
  }
};

struct PopupHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

struct GrabHandler {
  void release(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
};

struct InputMethodHandler {
  static InputMethodState* state(zwayland::server::Resource& resource) { return resource.data<InputMethodState>(); }
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void commit_string(zwayland::server::Client&, zwayland::server::Resource& resource, const std::string& text) {
    if (auto* method = state(resource)) method->commit_string = text;
  }
  void set_preedit_string(zwayland::server::Client&, zwayland::server::Resource& resource, const std::string& text,
                          std::int32_t begin, std::int32_t end) {
    if (auto* method = state(resource)) { method->preedit = text; method->preedit_begin = begin; method->preedit_end = end; }
  }
  void delete_surrounding_text(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t before,
                               std::uint32_t after) {
    if (auto* method = state(resource)) method->delete_surrounding = std::pair{before, after};
  }
  void commit(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t) {
    auto* method = state(resource);
    if (method == nullptr) return;
    if (auto* input = method->active; input != nullptr && focused(input)) {
      if (method->delete_surrounding)
        protocol::zwp_text_input_v3_send_delete_surrounding_text(*input->resource, method->delete_surrounding->first,
                                                                 method->delete_surrounding->second);
      if (method->commit_string) protocol::zwp_text_input_v3_send_commit_string(*input->resource, std::string_view(*method->commit_string));
      if (method->preedit)
        protocol::zwp_text_input_v3_send_preedit_string(*input->resource, std::string_view(*method->preedit), method->preedit_begin,
                                                        method->preedit_end);
      protocol::zwp_text_input_v3_send_done(*input->resource, input->commits);
    }
    method->commit_string.reset();
    method->preedit.reset();
    method->delete_surrounding.reset();
  }
  void get_input_popup_surface(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id,
                               zwayland::server::Resource*) {
    auto* popup = client.create_resource(&protocol::zwp_input_popup_surface_v2_interface, id, 1);
    if (popup == nullptr) { client.post_no_memory(); return; }
    auto* method = state(resource);
    popup->set_data(method);
    popup->set_handler(protocol::zwp_input_popup_surface_v2_handler(PopupHandler{}));
    popup->set_destroy_handler([](zwayland::server::Resource& destroyed) {
      if (auto* owner = destroyed.data<InputMethodState>()) std::erase(owner->popups, &destroyed);
    });
    if (method == nullptr) return;
    method->popups.push_back(popup);
    send_popup_rectangles(method);
  }
  void grab_keyboard(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    auto* method = state(resource);
    auto* grab = client.create_resource(&protocol::zwp_input_method_keyboard_grab_v2_interface, id, 1);
    if (grab == nullptr) { client.post_no_memory(); return; }
    grab->set_data(method);
    grab->set_handler(protocol::zwp_input_method_keyboard_grab_v2_handler(GrabHandler{}));
    grab->set_destroy_handler([](zwayland::server::Resource& destroyed) {
      if (auto* owner = destroyed.data<InputMethodState>(); owner != nullptr && owner->keyboard_grab == &destroyed)
        owner->keyboard_grab = nullptr;
    });
    if (method == nullptr || method->keyboard_grab != nullptr) return;
    method->keyboard_grab = grab;
    auto& seat = *method->seat;
    if (const auto fd = keymap_fd(seat))
      protocol::zwp_input_method_keyboard_grab_v2_send_keymap(*grab, protocol::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd.get(),
                                                              static_cast<std::uint32_t>(seat.keymap.size() + 1));
    protocol::zwp_input_method_keyboard_grab_v2_send_repeat_info(*grab, seat.repeat_rate, seat.repeat_delay);
    input_method_grab_modifiers(seat);
  }
};

void input_method_destroyed(zwayland::server::Resource& resource) {
  auto* method = resource.data<InputMethodState>();
  if (method == nullptr) return;
  for (auto* popup : method->popups) popup->set_data(nullptr);
  if (method->keyboard_grab != nullptr) method->keyboard_grab->set_data(nullptr);
  if (method->seat->input_method == method) method->seat->input_method = nullptr;
  delete method;
}

struct InputMethodManagerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_input_method(zwayland::server::Client& client, zwayland::server::Resource& manager,
                        zwayland::server::Resource* seat_resource, std::uint32_t id) {
    auto* seat = manager.data<SeatState>();
    auto* resource = client.create_resource(&protocol::zwp_input_method_v2_interface, id, 1);
    if (resource == nullptr) { client.post_no_memory(); return; }
    resource->set_handler(protocol::zwp_input_method_v2_handler(InputMethodHandler{}));
    if (seat_resource == nullptr || seat_resource->data<SeatState>() != seat || seat->input_method != nullptr) {
      protocol::zwp_input_method_v2_send_unavailable(*resource);
      return;
    }
    auto* method = new (std::nothrow) InputMethodState;
    if (method == nullptr) { resource->destroy(); client.post_no_memory(); return; }
    method->seat = seat;
    method->resource = resource;
    seat->input_method = method;
    resource->set_data(method);
    resource->set_destroy_handler(input_method_destroyed);
    for (auto* input : seat->text_inputs) if (input->current.enabled && focused(input)) { relay(input); break; }
  }
};

}  // namespace

void text_input_focus_changed(SeatState* seat, SurfaceState* next) {
  for (auto* input : seat->text_inputs) focus(input, next);
}

bool input_method_grab_key(SeatState& seat, std::uint64_t source, std::uint32_t time, std::uint32_t key, std::uint32_t state) {
  auto* method = seat.input_method;
  if (method == nullptr || method->keyboard_grab == nullptr) return false;
  // The input method's own virtual keyboard must reach the client, not loop back.
  if (const auto owner = seat.keyboard_source_clients.find(source);
      owner != seat.keyboard_source_clients.end() && owner->second == method->resource->client) return false;
  protocol::zwp_input_method_keyboard_grab_v2_send_key(*method->keyboard_grab, seat.display->next_serial(), time, key, state);
  return true;
}

void input_method_grab_modifiers(SeatState& seat) {
  auto* method = seat.input_method;
  if (method == nullptr || method->keyboard_grab == nullptr || seat.xkb_state_handle == nullptr) return;
  protocol::zwp_input_method_keyboard_grab_v2_send_modifiers(*method->keyboard_grab, seat.display->next_serial(),
      xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_DEPRESSED),
      xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_LATCHED),
      xkb_state_serialize_mods(seat.xkb_state_handle, XKB_STATE_MODS_LOCKED),
      xkb_state_serialize_layout(seat.xkb_state_handle, XKB_STATE_LAYOUT_EFFECTIVE));
}

void bind_text_input_manager(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwp_text_input_manager_v3_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwp_text_input_manager_v3_handler(TextInputManagerHandler{}));
}

void bind_input_method_manager(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwp_input_method_manager_v2_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwp_input_method_manager_v2_handler(InputMethodManagerHandler{}));
}

}  // namespace zwwm::detail
