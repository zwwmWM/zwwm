#pragma once


#include <zwayland/server/display.hpp>
#include "compositor_server_internal.hpp"

#ifdef ZWWM_XWAYLAND

#include <xwayland-shell-zwayland-server.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <chrono>
#include <optional>

#include "zwwm/xwayland_spawn.hpp"

namespace zwwm::detail {

// Groups of X11 properties, so a change only re-reads the group it touched.
enum XwaylandProperty : std::uint32_t {
  xprop_title = 1U << 0U, xprop_class = 1U << 1U, xprop_protocols = 1U << 2U,
  xprop_transient = 1U << 3U, xprop_type = 1U << 4U, xprop_role = 1U << 5U,
  xprop_state = 1U << 6U, xprop_normal_hints = 1U << 7U, xprop_hints = 1U << 8U,
  xprop_all = (1U << 9U) - 1U
};
inline constexpr std::uint32_t kXwaylandWindowEvents = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE;

struct XwaylandRuntime;
struct XwaylandWindow {
  xcb_window_t id = XCB_WINDOW_NONE;
  std::uint64_t serial = 0;
  std::int16_t x = 0, y = 0;
  std::uint16_t width = 1, height = 1;
  Rect requested_bounds{0, 0, 1, 1};
  bool mapped = false;
  bool ever_mapped = false;
  bool override_redirect = false;
  bool position_specified = false;
  bool supports_delete = false;
  bool supports_take_focus = false;
  bool accepts_input = true;
  bool modal = false;
  std::uint32_t dirty = 0;
  std::vector<xcb_atom_t> extra_states;
  // Survive a remap, which gives the same X11 window a new wl_surface.
  std::optional<bool> remembered_floating;
  Rect remembered_bounds{0, 0, 0, 0};
  xcb_window_t transient_for = XCB_WINDOW_NONE;
  std::vector<xcb_atom_t> window_types;
  std::string window_role;
  std::int32_t min_width = 0, min_height = 0;
  std::int32_t max_width = 0, max_height = 0;
  std::int32_t base_width = 0, base_height = 0;
  std::string title;
  std::string app_id;
  XwaylandSurfaceState* surface = nullptr;
};
struct XwaylandSurfaceState {
  XwaylandRuntime* runtime = nullptr;
  SurfaceState* surface = nullptr;
  zwayland::server::Resource* resource = nullptr;
  std::uint64_t pending_serial = 0;
  std::uint64_t serial = 0;
  XwaylandWindow* window = nullptr;
  OutputId output;
  Rect tile_bounds{0, 0, 0, 0};
  Rect content_bounds{0, 0, 0, 0};
  Rect floating_bounds{0, 0, 0, 0};
  CanvasBounds canvas_bounds;
  std::uint8_t tag = 1;
  std::optional<bool> floating_override;
  bool floating = false;
  bool fullscreen = false;
  bool hidden = false;
  bool associated = false;
};
struct XwaylandSelection { XwaylandRuntime* runtime = nullptr; xcb_atom_t atom = XCB_ATOM_NONE; xcb_window_t window = XCB_WINDOW_NONE; xcb_window_t owner = XCB_WINDOW_NONE; xcb_timestamp_t timestamp = XCB_CURRENT_TIME; DataSourceState* source = nullptr; bool primary = false; bool notify_on_focus = false; };
struct XwaylandReceiveTransfer { XwaylandSelection* selection = nullptr; xcb_window_t window = XCB_WINDOW_NONE; int fd = -1; int event_source = -1; std::vector<std::uint8_t> data; std::size_t offset = 0; bool incremental = false; };
struct XwaylandSendTransfer { XwaylandSelection* selection = nullptr; xcb_selection_request_event_t request{}; int fd = -1; int event_source = -1; std::vector<std::uint8_t> data; std::size_t offset = 0; bool incremental = false; };

struct XwaylandRuntime {
  zwayland::server::Display* display = nullptr; zwayland::server::EventLoop* loop = nullptr; std::uint32_t shell = 0; zwayland::server::Client* client = nullptr;
  int ready_source = -1; int xwm_source = -1; int restart_source = -1;
  zwayland::server::Listener display_destroy{}; zwayland::server::Listener client_destroy{}; XwaylandDisplay xdisplay; pid_t pid = -1;
  int ready_fd = -1; int wm_fd = -1; xcb_connection_t* connection = nullptr; xcb_screen_t* screen = nullptr;
  const xcb_query_extension_reply_t* xfixes = nullptr;
  xcb_atom_t wl_surface_serial = XCB_ATOM_NONE, net_active_window = XCB_ATOM_NONE, net_client_list = XCB_ATOM_NONE;
  xcb_atom_t net_supported = XCB_ATOM_NONE, net_supporting_wm_check = XCB_ATOM_NONE, net_wm_name = XCB_ATOM_NONE;
  xcb_atom_t utf8_string = XCB_ATOM_NONE, wm_name = XCB_ATOM_NONE, wm_class = XCB_ATOM_NONE;
  xcb_atom_t wm_protocols = XCB_ATOM_NONE, wm_take_focus = XCB_ATOM_NONE, wm_delete_window = XCB_ATOM_NONE;
  xcb_atom_t wm_transient_for = XCB_ATOM_NONE, net_wm_state = XCB_ATOM_NONE, net_wm_state_fullscreen = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_modal = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_maximized_vert = XCB_ATOM_NONE, net_wm_state_maximized_horz = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_hidden = XCB_ATOM_NONE, wm_s0 = XCB_ATOM_NONE, net_wm_cm_s0 = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type = XCB_ATOM_NONE, net_wm_window_type_normal = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_dialog = XCB_ATOM_NONE, net_wm_window_type_splash = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_toolbar = XCB_ATOM_NONE, net_wm_window_type_utility = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_tooltip = XCB_ATOM_NONE, net_wm_window_type_popup_menu = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_dock = XCB_ATOM_NONE, net_wm_window_type_dropdown_menu = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_menu = XCB_ATOM_NONE, kde_net_wm_window_type_override = XCB_ATOM_NONE;
  xcb_atom_t wm_window_role = XCB_ATOM_NONE, wm_normal_hints = XCB_ATOM_NONE, wm_size_hints = XCB_ATOM_NONE;
  xcb_atom_t clipboard_atom = XCB_ATOM_NONE, primary_atom = XCB_ATOM_NONE, targets_atom = XCB_ATOM_NONE;
  xcb_atom_t timestamp_atom = XCB_ATOM_NONE, wl_selection_atom = XCB_ATOM_NONE, text_atom = XCB_ATOM_NONE;
  xcb_atom_t incr_atom = XCB_ATOM_NONE; xcb_window_t wm_window = XCB_WINDOW_NONE; Observer* observer = nullptr;
  xcb_atom_t wm_state = XCB_ATOM_NONE, wm_hints = XCB_ATOM_NONE, net_frame_extents = XCB_ATOM_NONE;
  xcb_atom_t net_request_frame_extents = XCB_ATOM_NONE, net_close_window = XCB_ATOM_NONE;
  std::chrono::steady_clock::time_point started_at{}; std::uint32_t restart_attempts = 0;
  std::vector<pid_t> exiting_children;
  XwaylandSelection clipboard; XwaylandSelection primary;
  std::vector<XwaylandReceiveTransfer*> receives; std::vector<XwaylandSendTransfer*> sends;
  std::vector<XwaylandWindow*> windows; std::vector<XwaylandSurfaceState*> surfaces;

  static xcb_atom_t intern(xcb_connection_t* connection, const char* name);
  XwaylandWindow* window(xcb_window_t id) const;
  bool is_own(xcb_window_t id) const;
  XwaylandWindow* track(xcb_window_t id, std::int16_t x, std::int16_t y, std::uint16_t width,
                        std::uint16_t height, bool override_redirect);
  void forget(XwaylandWindow* item);
  void publish_clients() const;
  void publish_wm_state(xcb_window_t id, std::uint32_t state) const;
  void publish_frame_extents(xcb_window_t id) const;
  std::uint32_t property_field(xcb_atom_t atom) const;
  bool wm_owned_state(xcb_atom_t atom) const;
  void read_window_properties(XwaylandWindow* item, std::uint32_t fields = xprop_all);
  bool refresh_dirty();
  bool automatic_floating(const XwaylandWindow* item) const;
  bool reclassify(XwaylandWindow* item);
  void publish_state(XwaylandSurfaceState* role) const;
  void close_window(XwaylandWindow* item) const;
  void focus(XwaylandWindow* item);
  void apply_geometry(XwaylandWindow* item) const;
  void try_associate(XwaylandSurfaceState* role);
  void try_associate(XwaylandWindow* item);
  void remove_role(XwaylandSurfaceState* role);
  void process_event(xcb_generic_event_t* generic);
  int dispatch(std::uint32_t mask);
  void reset_server();
  void schedule_restart();
  void reap_children();
  bool initialize_xwm();
  bool start();
  static void surface_resource_destroyed(zwayland::server::Resource* resource);
  static void get_xwayland_surface(zwayland::server::Client*, zwayland::server::Resource*, std::uint32_t, zwayland::server::Resource*);
  static void bind_shell(zwayland::server::Client*, void*, std::uint32_t, std::uint32_t);
  XwaylandRuntime(zwayland::server::Display* display, zwayland::server::EventLoop* loop, Observer* observer);
  ~XwaylandRuntime();
};

extern XwaylandRuntime* active_xwayland;
bool handle_xwayland_selection_event(XwaylandRuntime* runtime, xcb_generic_event_t* event);
void xwayland_receive_selection(DataSourceState* source, const char* mime, int fd);
void xwayland_wayland_selection_changed(SeatState* seat, bool primary);
void xwayland_keyboard_focus_changed(XwaylandRuntime* runtime);
void xwayland_reset_selections(XwaylandRuntime* runtime);
void commit_xwayland_surface(XwaylandSurfaceState* role);
void detach_xwayland_role(XwaylandSurfaceState* role);
void focus_xwayland_surface(SurfaceState* surface);
void focus_xwayland_if_needed(SurfaceState* surface);
void set_xwayland_floating(XwaylandSurfaceState* role, bool floating);

}  // namespace zwwm::detail

#endif
