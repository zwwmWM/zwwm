#include "zwwm/runtime_backend.hpp"
#include "backend/scene.hpp"
#include "zwwm/compositor_server.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm/animation.hpp"
#include "zwwm/output_capture.hpp"
#include "zwwm/unique_fd.hpp"
#include "zwwm/renderer/vulkan.hpp"

#include <zwwm-vlk/device.hpp>
#include <zwwm-vlk/image.hpp>

#include <zwayland/server/display.hpp>
#include <wayland-zwayland-server.h>
#include <X11/Xcursor/Xcursor.h>

extern "C" {
#include <gbm.h>
#include <libinput.h>
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm {
namespace {

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

using backend_scene::animated_content;
using backend_scene::animated_decoration_scale;
using backend_scene::assigned_tile;
using backend_scene::source_uv;
using backend_scene::window_geometry;

// ZWWM_DRM_DEVICES is a colon-separated allow-list of cards; symlinks such as /dev/dri/by-path/* resolve.
bool drm_device_allowed(const char* path) {
  const char* list = std::getenv("ZWWM_DRM_DEVICES");
  if (list == nullptr || *list == '\0') return true;
  std::error_code error;
  const auto target = std::filesystem::weakly_canonical(path, error);
  for (const auto entry : std::views::split(std::string_view(list), ':')) {
    const std::string allowed(entry.begin(), entry.end());
    if (allowed.empty()) continue;
    if (allowed == path) return true;
    const auto resolved = std::filesystem::weakly_canonical(allowed, error);
    if (!error && resolved == target) return true;
  }
  return false;
}

bool is_drm_card(const char* path) {
  if (path == nullptr) {
    return false;
  }
  const std::string_view node(path);
  const std::size_t slash = node.rfind('/');
  const std::string_view name = node.substr(slash == std::string_view::npos ? 0 : slash + 1);
  return name.starts_with("card") && name.size() > 4 &&
         std::all_of(name.begin() + 4, name.end(), [](char character) { return character >= '0' && character <= '9'; });
}

struct KmsProperty {
  std::uint32_t id = 0;
  std::uint64_t value = 0;
  std::uint64_t minimum = 0, maximum = 0;
};

KmsProperty kms_property(int fd, std::uint32_t object, std::uint32_t type,
                         const char* name) {
  KmsProperty result;
  auto* properties = drmModeObjectGetProperties(fd, object, type);
  if (properties == nullptr) return result;
  for (std::uint32_t index = 0; index < properties->count_props; ++index) {
    auto* property = drmModeGetProperty(fd, properties->props[index]);
    if (property == nullptr) continue;
    if (std::strcmp(property->name, name) == 0) {
      result.id = property->prop_id;
      result.value = properties->prop_values[index];
      if ((property->flags & DRM_MODE_PROP_RANGE) != 0 && property->count_values >= 2) {
        result.minimum = property->values[0];
        result.maximum = property->values[1];
      }
    }
    drmModeFreeProperty(property);
    if (result.id != 0) break;
  }
  drmModeFreeObjectProperties(properties);
  return result;
}

const libseat_seat_listener kSeatListener{
    .enable_seat = RuntimeBackend::enable_seat,
    .disable_seat = RuntimeBackend::disable_seat,
};

const libinput_interface kLibinputInterface{
    .open_restricted = RuntimeBackend::open_restricted,
    .close_restricted = RuntimeBackend::close_restricted,
};

std::uint32_t mode_refresh_millihz(const drmModeModeInfo& mode) {
  if (mode.htotal == 0 || mode.vtotal == 0) return 0;
  std::uint64_t refresh = static_cast<std::uint64_t>(mode.clock) * 1000000U / mode.htotal / mode.vtotal;
  if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0) refresh *= 2;
  if ((mode.flags & DRM_MODE_FLAG_DBLSCAN) != 0) refresh /= 2;
  if (mode.vscan > 1) refresh /= mode.vscan;
  return static_cast<std::uint32_t>(refresh);
}

bool same_drm_mode(const drmModeModeInfo& left, const drmModeModeInfo& right) {
  return left.clock == right.clock && left.hdisplay == right.hdisplay &&
         left.hsync_start == right.hsync_start && left.hsync_end == right.hsync_end &&
         left.htotal == right.htotal && left.hskew == right.hskew &&
         left.vdisplay == right.vdisplay && left.vsync_start == right.vsync_start &&
         left.vsync_end == right.vsync_end && left.vtotal == right.vtotal &&
         left.vscan == right.vscan && left.flags == right.flags;
}

OutputId stable_output_id(std::string_view card, std::string_view connector) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto part : {card, connector}) for (const unsigned char byte : part) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return OutputId{hash == 0 ? 1 : hash};
}

void transform_frame(renderer::FramePlan& frame, const OutputConfig& output, renderer::Size physical,
                     renderer::Point logical_origin = {}) {
  const auto scale = output.scale_per_mille / 1000.0F;
  for (auto& draw : frame.draws) {
    backend_scene::transform_geometry(draw, output, physical, logical_origin);
    draw.bounds.origin.x -= logical_origin.x;
    draw.bounds.origin.y -= logical_origin.y;
    if (draw.clip_bounds) {
      draw.clip_bounds->origin.x -= logical_origin.x;
      draw.clip_bounds->origin.y -= logical_origin.y;
    }
    if (draw.texture_bounds) {
      draw.texture_bounds->origin.x -= logical_origin.x;
      draw.texture_bounds->origin.y -= logical_origin.y;
    }
    const renderer::Rect output_clip{{0, 0}, output.logical_size(physical)};
    draw.clip_bounds = draw.clip_bounds ? draw.clip_bounds : std::optional<renderer::Rect>{output_clip};
    draw.bounds = output.physical_bounds(draw.bounds, physical);
    if (!draw.toplevel_bounds.empty()) {
      draw.toplevel_bounds.origin.x -= logical_origin.x;
      draw.toplevel_bounds.origin.y -= logical_origin.y;
      draw.toplevel_bounds = output.physical_bounds(draw.toplevel_bounds, physical);
    }
    if (draw.clip_bounds) draw.clip_bounds = output.physical_bounds(*draw.clip_bounds, physical);
    if (draw.texture_bounds) draw.texture_bounds = output.physical_bounds(*draw.texture_bounds, physical);
    draw.corner_radius *= scale;
    draw.texture_corner_radius *= scale;
    draw.border_width *= scale;
    draw.clip_radius *= scale;
    draw.background_blur_radius *= scale;
    draw.glass_refraction *= scale;
    draw.glass_dispersion *= scale;
    draw.texture_transform = static_cast<std::uint32_t>(output.transform);
  }
}

}  // namespace

struct RuntimeBackend::DrmOutput {
  struct ShmTexture {
    std::uint64_t id = 0;
    std::uint64_t root_id = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t buffer_width = 0;
    std::int32_t buffer_height = 0;
    float source_left = 0.0F, source_top = 0.0F, source_right = 1.0F, source_bottom = 1.0F;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t geometry_x = 0;
    std::int32_t geometry_y = 0;
    std::int32_t geometry_width = 0;
    std::int32_t geometry_height = 0;
    std::int32_t assigned_tile_x = 0, assigned_tile_y = 0, assigned_tile_width = 0, assigned_tile_height = 0;
    std::int32_t assigned_content_x = 0, assigned_content_y = 0, assigned_content_width = 0, assigned_content_height = 0;
    std::uint32_t format = 0;
    std::uint32_t stack_index = 0;
    std::uint64_t scene_order = 0;
    std::uint32_t scene_rank = 2;
    std::int32_t layer_priority = 0;
    std::uint64_t root_order = 0, tree_order = 0;
    float compositor_opacity = 1.0F;
    std::uint32_t background_blur_radius = 0;
    float background_blur_ignore_alpha = 0.0F;
    bool glass = false;
    bool layer_surface = false;
    bool toplevel = false;
    bool popup = false;
    bool fullscreen = false;
    bool focused = false;
    bool opaque = false;
    bool content_ready = false;
    bool suppress_geometry_animation = false;
    bool track_geometry_animation = false;
    bool camera_motion = false;
    bool restore_scene = false;
    double camera_scale = 1.0;
    std::int32_t camera_center_x = 0, camera_center_y = 0;
    double camera_offset_x = 0.0, camera_offset_y = 0.0;
    std::int64_t camera_world_x = 0, camera_world_y = 0;
    bool canvas_camera = false;
    renderer::TextureRef texture;
    bool owns_texture = false;
    std::uint32_t overview_tag = 0;
    std::uint64_t shm_generation = 0;
    bool removed = false;
    bool tag_outgoing = false;
    std::string window_shader;
    std::string border_shader;
    std::vector<std::uint32_t> pixels;
    std::optional<renderer::DmabufAttributes> dmabuf;
    std::uint64_t buffer_id = 0;
  };

  struct ScanoutBuffer {
    gbm_bo* bo = nullptr;
    std::uint32_t framebuffer = 0;
    renderer::TextureRef image;
  };

  DrmDevice* device = nullptr;
  std::string connector_key;
  std::vector<ScanoutBuffer> scanout;
  std::vector<ShmTexture> shm_textures;
  bool tag_transition_preparing = false;
  bool camera_frame_preparing = false;
  bool overview = false;
  std::uint32_t overview_first_tag = 0, overview_active_tag = 1;
  double overview_progress = 0, overview_scroll = 0;
  std::uint32_t connector_id = 0;
  OutputInfo output;
  std::uint32_t crtc_id = 0;
  std::uint32_t crtc_index = 0;
  std::uint32_t plane_id = 0;
  std::uint32_t mode_blob = 0;
  struct AtomicProperties {
    std::uint32_t connector_crtc = 0, max_bpc = 0;
    std::uint32_t crtc_mode = 0, crtc_active = 0;
    std::uint32_t plane_fb = 0, plane_crtc = 0;
    std::uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    std::uint32_t crtc_x = 0, crtc_y = 0, crtc_w = 0, crtc_h = 0;
  } atomic;
  std::uint64_t max_bpc_value = 0;
  bool needs_modeset = true;
  drmModeModeInfo mode{};
  int front_buffer = -1;
  int pending_buffer = -1;
  bool flip_pending = false;
  bool needs_repaint = false;
  std::uint32_t scanout_bit_depth = 8;
  AnimationSystem animations;
  renderer::TextureRef cursor_texture;
  renderer::TextureRef capture_texture;
  renderer::TextureRef capture_cursor_texture;
  std::int32_t cursor_width = 0, cursor_height = 0, cursor_hotspot_x = 0, cursor_hotspot_y = 0;
  bool capture_ready = false;
  bool retired = false;
  bool powered_off = false;
  // A client buffer shown on the primary plane without composition.
  struct DirectFrame { std::uint32_t framebuffer = 0; std::uint64_t buffer_id = 0; };
  std::optional<DirectFrame> direct_front, direct_pending;
  std::uint64_t direct_failed_buffer = 0;

  explicit DrmOutput(AnimationConfig config) : animations(config) {}
};

struct RuntimeBackend::DrmDevice {
  struct DmabufTexture {
    std::uint64_t generation = 0;
    renderer::TextureRef texture;
    bool failed = false;
  };
  RuntimeBackend* backend = nullptr;
  std::string path;
  int fd = -1;
  int seat_device_id = -1;
  gbm_device* gbm = nullptr;
  std::shared_ptr<vlk::Device> vulkan;
  std::uint32_t scanout_format = DRM_FORMAT_XRGB8888;
  std::uint32_t scanout_bit_depth = 8;
  std::unique_ptr<renderer::VulkanRenderer> renderer;
  renderer::TextureRef error_popup_texture;
  std::uint64_t error_popup_generation = 0;
  // Windows on hidden tags, kept only for toplevel capture.
  std::vector<DrmOutput::ShmTexture> capture_surfaces;
  std::unordered_map<std::uint64_t, DmabufTexture> dmabuf_textures;
  std::vector<std::unique_ptr<DrmOutput>> outputs;
  std::vector<std::unique_ptr<DrmOutput>> retired_outputs;
  int source = -1;
};

RuntimeBackend::RuntimeBackend(zwayland::server::EventLoop* event_loop, std::shared_ptr<const RuntimeConfig> config)
    : event_loop_(event_loop), config_(std::move(config)) {
  if (const char* theme = std::getenv("XCURSOR_THEME"); theme != nullptr && *theme != '\0')
    cursor_theme_ = theme;
  if (const char* size = std::getenv("XCURSOR_SIZE"); size != nullptr)
    cursor_size_ = static_cast<std::uint32_t>(std::max(1, std::atoi(size)));
}
RuntimeBackend::~RuntimeBackend() { stop(); }

bool RuntimeBackend::start() {
  std::fprintf(stderr, "zwwm: direct backend: opening libseat session\n");
  active_ = false;
  libinput_suspended_ = false;
  if (event_loop_ == nullptr) {
    last_error_ = "Wayland event loop is unavailable";
    return false;
  }
  seat_ = libseat_open_seat(&kSeatListener, this);
  if (seat_ == nullptr) {
    last_error_ = std::string("libseat could not open a compositor seat: ") + std::strerror(errno);
    return false;
  }
  std::fprintf(stderr, "zwwm: direct backend: libseat session opened\n");
  const int seat_fd = libseat_get_fd(seat_);
  if (seat_fd < 0) {
    last_error_ = std::string("libseat could not provide a seat event fd: ") + std::strerror(errno);
    stop();
    return false;
  }
  try {
    seat_source_ = event_loop_->add_fd(seat_fd, EPOLLIN, [this](int fd, int mask) {
      on_seat_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register libseat event source";
    stop();
    return false;
  }

  udev_ = udev_new();
  udev_monitor_ = udev_ == nullptr ? nullptr : udev_monitor_new_from_netlink(udev_, "udev");
  if (udev_monitor_ == nullptr || udev_monitor_filter_add_match_subsystem_devtype(udev_monitor_, "drm", nullptr) < 0 ||
      udev_monitor_enable_receiving(udev_monitor_) < 0) {
    last_error_ = "could not initialize the udev DRM monitor";
    stop();
    return false;
  }
  try {
    udev_source_ = event_loop_->add_fd(udev_monitor_get_fd(udev_monitor_), EPOLLIN, [this](int fd, int mask) {
      on_udev_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register udev event source";
    stop();
    return false;
  }

  const char* seat_name = libseat_seat_name(seat_);
  if (seat_name == nullptr || *seat_name == '\0') seat_name = "seat0";
  libinput_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
  if (libinput_ == nullptr || libinput_udev_assign_seat(libinput_, seat_name) != 0) {
    last_error_ = std::string("could not initialize libinput for ") + seat_name;
    stop();
    return false;
  }
  std::fprintf(stderr, "zwwm: direct backend: input initialized for %s\n", seat_name);
  try {
    libinput_source_ = event_loop_->add_fd(libinput_get_fd(libinput_), EPOLLIN, [this](int fd, int mask) {
      on_libinput_fd(fd, static_cast<std::uint32_t>(mask), this);
      return true;
    });
  } catch (const std::exception&) {
    last_error_ = "could not register libinput event source";
    stop();
    return false;
  }
  // libseat's logind backend sends the initial enable on the first dispatch, readable fd or not.
  if (libseat_dispatch(seat_, 0) < 0) {
    last_error_ = std::string("initial libseat dispatch failed: ") + std::strerror(errno);
    stop();
    return false;
  }
  if (!active_)
    std::fprintf(stderr, "zwwm: seat is inactive; waiting for the login session to become active\n");
  discover_drm_devices();
  schedule_output_probe();
  return true;
}

void RuntimeBackend::stop() {
  if (error_timer_ >= 0) {
    event_loop_->remove(error_timer_);
    error_timer_ = -1;
  }
  if (probe_timer_ >= 0) {
    event_loop_->remove(probe_timer_);
    probe_timer_ = -1;
  }
  for (auto& device : devices_) {
    close_drm_device(*device);
  }
  devices_.clear();
  if (libinput_source_ >= 0) {
    event_loop_->remove(libinput_source_);
    libinput_source_ = -1;
  }
  if (libinput_ != nullptr) {
    libinput_unref(libinput_);
    libinput_ = nullptr;
  }
  if (udev_source_ >= 0) {
    event_loop_->remove(udev_source_);
    udev_source_ = -1;
  }
  if (udev_monitor_ != nullptr) {
    udev_monitor_unref(udev_monitor_);
    udev_monitor_ = nullptr;
  }
  if (udev_ != nullptr) {
    udev_unref(udev_);
    udev_ = nullptr;
  }
  if (seat_source_ >= 0) {
    event_loop_->remove(seat_source_);
    seat_source_ = -1;
  }
  if (seat_ != nullptr) {
    libseat_close_seat(seat_);
    seat_ = nullptr;
  }
  restricted_devices_.clear();
  active_ = false;
  libinput_suspended_ = false;
}

bool RuntimeBackend::active() const { return active_; }
const std::string& RuntimeBackend::last_error() const { return last_error_; }
void RuntimeBackend::set_config(std::shared_ptr<const RuntimeConfig> config) {
  if (config == nullptr) return;
  config_ = std::move(config);
  const std::uint64_t now = monotonic_ms();
  for (auto& device : devices_) for (auto& output : device->outputs) {
    output->capture_ready = false;
    output->animations.set_config(config_->animations, now);
    const auto& configured = config_->output_for(output->output.connector);
    const auto logical = configured.logical_size({static_cast<std::uint32_t>(output->mode.hdisplay),
                                                   static_cast<std::uint32_t>(output->mode.vdisplay)});
    output->output.logical_width = logical.width;
    output->output.logical_height = logical.height;
    output->output.scale_per_mille = configured.scale_per_mille;
    output->output.transform = configured.transform;
    output->output.refresh_millihz = mode_refresh_millihz(output->mode);
    repaint(*output);
  }
  publish_outputs();
}
bool RuntimeBackend::rebuild_switch_shaders(std::string* error) {
  if (!prepare_config_shaders(*config_, error)) return false;
  commit_config_shaders();
  return true;
}
bool RuntimeBackend::prepare_config_shaders(const RuntimeConfig& config, std::string* error) {
  renderer::ShaderSources sources;
  if (!load_shader_sources(config, &sources, error)) return false;
  std::vector<DrmDevice*> prepared;
  for (auto& device : devices_) {
    if (device->renderer == nullptr || device->outputs.empty()) continue;
    if (!device->renderer->prepare_shaders(sources)) {
      if (error != nullptr && error->empty()) *error = device->renderer->last_error();
      for (auto* ready : prepared) ready->renderer->discard_shaders();
      return false;
    }
    prepared.push_back(device.get());
  }
  if (prepared.empty()) {
    if (error != nullptr) *error = "no active Vulkan renderer";
    return false;
  }
  return true;
}
void RuntimeBackend::commit_config_shaders() {
  for (auto& device : devices_) {
    if (device->renderer == nullptr || device->outputs.empty()) continue;
    device->renderer->commit_shaders();
    for (auto& item : device->outputs) repaint(*item);
  }
}
void RuntimeBackend::discard_config_shaders() {
  for (auto& device : devices_)
    if (device->renderer != nullptr) device->renderer->discard_shaders();
}
void RuntimeBackend::show_error(std::string message) {
  auto popup = render_error_popup(message);
  if (!popup) return;
  error_popup_ = std::move(popup);
  error_popup_visible_ = true;
  ++error_popup_generation_;
  if (error_timer_ < 0) error_timer_ = event_loop_->add_timer([this] { on_error_timeout(this); });
  event_loop_->update_timer(error_timer_, 7000);
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}
void RuntimeBackend::clear_error() {
  if (!error_popup_visible_) return;
  error_popup_visible_ = false;
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}
int RuntimeBackend::on_error_timeout(void* data) {
  static_cast<RuntimeBackend*>(data)->clear_error();
  return 0;
}
void RuntimeBackend::set_input_target(CompositorServer* compositor) { input_target_ = compositor; }
void RuntimeBackend::set_gesture_observer(GestureObserver observer, void* data) {
  gesture_observer_ = observer;
  gesture_data_ = data;
}
void RuntimeBackend::set_dmabuf_target(CompositorServer* compositor) {
  dmabuf_target_ = compositor;
  publish_dmabuf_importer();
}
void RuntimeBackend::set_cursor_shape(const char* xcursor_name) {
  if (xcursor_name == nullptr) {
    cursor_visible_ = false;
    for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
    return;
  }
  cursor_visible_ = true;
  cursor_shape_ = xcursor_name;
  for (auto& device : devices_) for (auto& output : device->outputs) {
    if (device->renderer == nullptr) continue;
    XcursorImages* cursor = XcursorLibraryLoadImages(xcursor_name, cursor_theme_.c_str(), cursor_size_);
    if (cursor == nullptr || cursor->nimage == 0 || cursor->images[0] == nullptr) {
      if (cursor != nullptr) XcursorImagesDestroy(cursor);
      continue;
    }
    const XcursorImage* image = cursor->images[0];
    auto texture = device->renderer->upload(nullptr, image->pixels, image->width, image->height, image->width);
    if (texture != nullptr) {
      output->cursor_texture = std::move(texture);
      output->cursor_width = static_cast<std::int32_t>(image->width);
      output->cursor_height = static_cast<std::int32_t>(image->height);
      output->cursor_hotspot_x = static_cast<std::int32_t>(image->xhot);
      output->cursor_hotspot_y = static_cast<std::int32_t>(image->yhot);
    }
    XcursorImagesDestroy(cursor);
    repaint(*output);
  }
}
bool RuntimeBackend::set_cursor_theme(const std::string& theme, std::uint32_t size,
                                      std::string* error) {
  if (theme.empty()) {
    if (error != nullptr) *error = "cursor theme must not be empty";
    return false;
  }
  if (size == 0 || size > 1024) {
    if (error != nullptr) *error = "cursor size must be between 1 and 1024";
    return false;
  }
  XcursorImages* cursor = XcursorLibraryLoadImages(cursor_shape_.c_str(), theme.c_str(), size);
  if (cursor == nullptr || cursor->nimage == 0 || cursor->images[0] == nullptr) {
    if (cursor != nullptr) XcursorImagesDestroy(cursor);
    if (error != nullptr) *error = "could not load cursor theme '" + theme + "'";
    return false;
  }
  XcursorImagesDestroy(cursor);
  cursor_theme_ = theme;
  cursor_size_ = size;
  if (cursor_visible_) set_cursor_shape(cursor_shape_.c_str());
  return true;
}
void RuntimeBackend::set_cursor_position(std::int32_t x, std::int32_t y) {
  cursor_x_ = x;
  cursor_y_ = y;
  for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
}

bool RuntimeBackend::capture_output(OutputCapture& capture, bool overlay_cursor) {
  // Direct scanout bypasses the capture texture, so capturing switches back to composition.
  last_capture_ms_ = monotonic_ms();
  for (auto& device : devices_) for (auto& output : device->outputs)
    if (output->direct_front || output->direct_pending) repaint(*output);
  const auto requested = capture.output;
  DrmOutput* selected = nullptr;
  for (const auto& device : devices_) for (const auto& output : device->outputs)
    if ((!requested || output->output.id == requested) && output->capture_ready &&
        output->capture_texture != nullptr && output->mode.hdisplay > 0 && output->mode.vdisplay > 0) {
      if (selected == nullptr || output->output.connector < selected->output.connector) selected = output.get();
    }
  if (selected == nullptr || selected->device->renderer == nullptr) return false;
  auto& renderer = *selected->device->renderer;
  capture.output = selected->output.id;
  capture.width = static_cast<std::uint32_t>(selected->mode.hdisplay);
  capture.height = static_cast<std::uint32_t>(selected->mode.vdisplay);
  renderer::TextureRef capture_texture = selected->capture_texture;
  if (overlay_cursor && cursor_visible_) {
    if (selected->cursor_texture == nullptr) return false;
    auto& composed = selected->capture_cursor_texture;
    if (composed == nullptr || composed->width() != capture.width || composed->height() != capture.height)
      composed = renderer.create_target({capture.width, capture.height});
    if (composed == nullptr) return false;
    renderer::FramePlan cursor_frame;
    cursor_frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                                  {{static_cast<std::int32_t>(std::lround(cursor_x_)) - selected->cursor_hotspot_x,
                                    static_cast<std::int32_t>(std::lround(cursor_y_)) - selected->cursor_hotspot_y},
                                   {static_cast<std::uint32_t>(selected->cursor_width),
                                    static_cast<std::uint32_t>(selected->cursor_height)}},
                                  1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, selected->cursor_texture});
    transform_frame(cursor_frame, config_->output_for(selected->output.connector),
                    {capture.width, capture.height},
                    {selected->output.logical_x, selected->output.logical_y});
    if (!renderer.begin() || !renderer.copy(selected->capture_texture, composed) ||
        !renderer.render(cursor_frame, composed) || !renderer.submit()) {
      if (renderer.recording()) (void)renderer.submit();
      return false;
    }
    capture_texture = composed;
  }
  if (!renderer.read_pixels(capture_texture, &capture.pixels)) {
    capture = {};
    return false;
  }
  for (auto& pixel : capture.pixels) pixel |= 0xff000000U;
  return true;
}
void RuntimeBackend::store_capture_surface(const ShmBufferView& buffer) {
  const auto requested = capture_requests_.find(buffer.root_surface_id);
  if (requested == capture_requests_.end() || monotonic_ms() - requested->second > 5000U) return;
  if ((buffer.pixels == nullptr && buffer.dmabuf == nullptr) || buffer.width <= 0 || buffer.height <= 0) return;
  DrmDevice* device = nullptr;
  for (auto& candidate : devices_) {
    if (candidate->renderer == nullptr) continue;
    if (device == nullptr) device = candidate.get();
    if (std::any_of(candidate->outputs.begin(), candidate->outputs.end(),
                    [&](const auto& output) { return output->output.id == buffer.output; })) { device = candidate.get(); break; }
  }
  if (device == nullptr) return;
  auto& list = device->capture_surfaces;
  auto entry = std::find_if(list.begin(), list.end(), [&](const auto& item) { return item.id == buffer.surface_id; });
  if (entry == list.end()) entry = list.insert(list.end(), DrmOutput::ShmTexture{});
  auto& item = *entry;
  item.id = buffer.surface_id;
  item.root_id = buffer.root_surface_id;
  item.toplevel = buffer.toplevel;
  item.popup = buffer.popup;
  item.x = buffer.x;
  item.y = buffer.y;
  item.width = buffer.logical_width;
  item.height = buffer.logical_height;
  item.source_left = buffer.source_left; item.source_top = buffer.source_top;
  item.source_right = buffer.source_right; item.source_bottom = buffer.source_bottom;
  item.opaque = buffer.opaque;
  item.content_ready = true;
  item.removed = false;
  if (buffer.dmabuf != nullptr) {
    if (item.texture == nullptr || item.owns_texture || item.shm_generation != buffer.buffer_generation) {
      item.texture = device->renderer->import_dmabuf(*buffer.dmabuf);
      item.owns_texture = false;
      item.pixels.clear();
    }
  } else {
    item.pixels.resize(static_cast<std::size_t>(buffer.width) * buffer.height);
    for (int y = 0; y < buffer.height; ++y) {
      auto* row = item.pixels.data() + static_cast<std::size_t>(y) * buffer.width;
      std::memcpy(row, buffer.pixels + static_cast<std::size_t>(y) * buffer.stride,
                  static_cast<std::size_t>(buffer.width) * sizeof(std::uint32_t));
      if (buffer.format == protocol::WL_SHM_FORMAT_XRGB8888)
        for (int x = 0; x < buffer.width; ++x) row[x] |= 0xff000000U;
    }
    item.texture = device->renderer->upload(item.owns_texture ? item.texture : nullptr, item.pixels.data(),
                                            static_cast<std::uint32_t>(buffer.width), static_cast<std::uint32_t>(buffer.height),
                                            static_cast<std::uint32_t>(buffer.width));
    item.owns_texture = item.texture != nullptr;
  }
  item.shm_generation = buffer.buffer_generation;
  if (list.size() > 64) list.erase(list.begin());
  // Flips pace both the hidden window's frame callbacks and the portal's captures.
  if (!device->outputs.empty()) repaint(*device->outputs.front());
}

bool RuntimeBackend::capture_toplevel(std::uint64_t id, OutputCapture& capture, bool overlay_cursor) {
  capture_requests_[id] = monotonic_ms();
  DrmOutput* selected = nullptr;
  const DrmOutput::ShmTexture* root_surface = nullptr;
  const std::vector<DrmOutput::ShmTexture>* surfaces = nullptr;
  const auto find_root = [id](const std::vector<DrmOutput::ShmTexture>& list) {
    return std::find_if(list.begin(), list.end(), [id](const auto& surface) {
      return surface.id == id && surface.root_id == id && surface.toplevel && !surface.removed &&
             surface.content_ready && surface.texture != nullptr && surface.width > 0 && surface.height > 0;
    });
  };
  for (const auto& device : devices_) for (const auto& output : device->outputs) {
    const auto root = find_root(output->shm_textures);
    if (root != output->shm_textures.end()) { selected = output.get(); root_surface = &*root; surfaces = &output->shm_textures; break; }
  }
  // Fall back to a window on a hidden tag; the cursor is not over it.
  for (const auto& device : devices_) if (selected == nullptr && !device->outputs.empty()) {
    const auto root = find_root(device->capture_surfaces);
    if (root == device->capture_surfaces.end()) continue;
    selected = device->outputs.front().get(); root_surface = &*root; surfaces = &device->capture_surfaces;
    overlay_cursor = false;
  }
  if (selected == nullptr) for (auto& device : devices_) if (!device->outputs.empty()) { repaint(*device->outputs.front()); break; }
  if (selected == nullptr || root_surface == nullptr || selected->device->renderer == nullptr) return false;

  capture.output = selected->output.id;
  capture.width = static_cast<std::uint32_t>(root_surface->width);
  capture.height = static_cast<std::uint32_t>(root_surface->height);
  renderer::FramePlan frame;
  for (const auto& surface : *surfaces) {
    if (surface.root_id != id || surface.removed || surface.popup || surface.texture == nullptr ||
        surface.width <= 0 || surface.height <= 0) continue;
    renderer::DrawCall draw{surface.id,
        {{surface.x - root_surface->x, surface.y - root_surface->y},
         {static_cast<std::uint32_t>(surface.width), static_cast<std::uint32_t>(surface.height)}},
        1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture, 0.0F, 0.0F,
        {surface.source_left, surface.source_top, surface.source_right, surface.source_bottom}};
    draw.opaque = surface.opaque;
    frame.draws.push_back(draw);
  }
  if (overlay_cursor && cursor_visible_ && selected->cursor_texture != nullptr &&
      root_surface->assigned_content_width > 0 && root_surface->assigned_content_height > 0) {
    const double scale_x = static_cast<double>(root_surface->width) / root_surface->assigned_content_width;
    const double scale_y = static_cast<double>(root_surface->height) / root_surface->assigned_content_height;
    const auto cursor_x = static_cast<std::int32_t>(std::lround(
        (cursor_x_ - root_surface->assigned_content_x) * scale_x - selected->cursor_hotspot_x));
    const auto cursor_y = static_cast<std::int32_t>(std::lround(
        (cursor_y_ - root_surface->assigned_content_y) * scale_y - selected->cursor_hotspot_y));
    frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                           {{cursor_x, cursor_y},
                            {static_cast<std::uint32_t>(selected->cursor_width),
                             static_cast<std::uint32_t>(selected->cursor_height)}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, selected->cursor_texture});
  }
  if (frame.draws.empty() || !selected->device->renderer->render_pixels(
          frame, {capture.width, capture.height}, &capture.pixels)) {
    capture = {};
    return false;
  }
  for (auto& pixel : capture.pixels) pixel |= 0xff000000U;
  return true;
}
bool RuntimeBackend::supports_embedded_cursor() const {
  // Capability is known before asynchronous DRM output setup completes.
  return true;
}
void RuntimeBackend::publish_dmabuf_importer() {
  if (dmabuf_target_ == nullptr) return;
  std::vector<DrmDevice*> candidates;
  for (const auto& device : devices_)
    if (device->renderer != nullptr && !device->renderer->dmabuf_formats().empty()) candidates.push_back(device.get());
  std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) { return left->path < right->path; });
  for (auto* device : candidates) {
    std::optional<dev_t> main_device;
    drmDevicePtr drm_device = nullptr;
    const bool got_device = drmGetDevice2(device->fd, 0, &drm_device) == 0 && drm_device != nullptr;
    const bool has_render_node = got_device && (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) != 0 &&
                                 drm_device->nodes[DRM_NODE_RENDER] != nullptr;
    struct stat status {};
    if (has_render_node) {
      if (stat(drm_device->nodes[DRM_NODE_RENDER], &status) == 0) main_device = status.st_rdev;
    } else if (fstat(device->fd, &status) == 0) {
      main_device = status.st_rdev;
    }
    if (drm_device != nullptr) drmFreeDevice(&drm_device);
    dmabuf_target_->set_dmabuf_feedback(device->renderer->dmabuf_formats(), main_device);
    return;
  }
  dmabuf_target_->set_dmabuf_feedback({}, std::nullopt);
}
void RuntimeBackend::publish_outputs() {
  std::vector<OutputInfo> outputs;
  for (const auto& device : devices_) for (const auto& output : device->outputs)
    if (output->output.enabled) outputs.push_back(output->output);
  std::stable_sort(outputs.begin(), outputs.end(), [](const OutputInfo& left, const OutputInfo& right) {
    return left.connector < right.connector;
  });
  std::int32_t x = 0;
  std::uint32_t height = 0;
  for (auto& output : outputs) {
    output.logical_x = x;
    for (auto& device : devices_) for (auto& candidate : device->outputs)
      if (candidate->output.id == output.id) candidate->output.logical_x = x;
    x += static_cast<std::int32_t>(output.logical_width);
    height = std::max(height, output.logical_height);
  }
  output_width_ = static_cast<std::uint32_t>(std::max(1, x));
  output_height_ = std::max(1U, height);
  if (!outputs.empty()) {
    physical_output_width_ = outputs.front().physical_width;
    physical_output_height_ = outputs.front().physical_height;
  }
  if (input_target_ != nullptr) input_target_->set_outputs(std::move(outputs));
}
void RuntimeBackend::present(const ShmBufferView& buffer) {
  if (buffer.capture_only) {
    store_capture_surface(buffer);
    return;
  }
  if (buffer.pixels != nullptr || buffer.dmabuf != nullptr)
    for (auto& device : devices_)
      std::erase_if(device->capture_surfaces, [&](const auto& item) { return item.id == buffer.surface_id; });
  for (auto& device : devices_) for (auto& output_ptr : device->outputs) {
    auto* card = output_ptr.get();
    if (buffer.output && buffer.output != card->output.id) continue;
    if (device->renderer == nullptr || card->scanout.empty()) continue;
    if (buffer.overview_changed) {
      card->overview = buffer.overview;
      card->overview_first_tag = buffer.overview_first_tag;
      card->overview_active_tag = buffer.overview_active_tag;
      card->overview_progress = buffer.overview_progress;
      card->overview_scroll = buffer.overview_scroll;
      continue;
    }
    if (buffer.camera_frame) {
      card->camera_frame_preparing = !buffer.camera_frame_ready;
      if (buffer.camera_frame_ready) repaint(*card);
      continue;
    }
    if (buffer.tag_transition) {
      if (buffer.tag_transition_ready) {
        card->tag_transition_preparing = false;
        card->animations.start_tag_transition(buffer.tag_transition_direction, monotonic_ms());
        repaint(*card);
        continue;
      }
      for (auto& item : card->shm_textures) {
        const auto root = std::find_if(card->shm_textures.begin(), card->shm_textures.end(), [&item](const auto& candidate) { return candidate.id == item.root_id; });
        if (root != card->shm_textures.end() && root->toplevel) {
          item.tag_outgoing = true;
        }
      }
      card->tag_transition_preparing = true;
      continue;
    }
    if (buffer.scene_reset) {
      const auto root_id = buffer.root_surface_id;
      card->animations.remove(root_id);
      for (auto& item : card->shm_textures)
        if (item.root_id == root_id || item.id == root_id) item.removed = true;
      if (!card->camera_frame_preparing) repaint(*card);
      continue;
    }
    auto surface = std::find_if(card->shm_textures.begin(), card->shm_textures.end(), [&buffer](const auto& item) {
      return item.id == buffer.surface_id;
    });
    if (buffer.pixels == nullptr && buffer.dmabuf == nullptr) {
      if (surface != card->shm_textures.end() && surface->removed) continue;
      if (buffer.toplevel && buffer.assigned_content_width > 0 && buffer.assigned_content_height > 0) {
        DrmOutput::ShmTexture updated = surface == card->shm_textures.end() ? DrmOutput::ShmTexture{} : std::move(*surface);
        const bool retain_content = surface != card->shm_textures.end() && updated.texture != nullptr;
        const bool closing = retain_content && buffer.unmapped;
        updated.id = buffer.surface_id;
        updated.root_id = buffer.root_surface_id;
        updated.assigned_tile_x = buffer.assigned_tile_x; updated.assigned_tile_y = buffer.assigned_tile_y;
        updated.assigned_tile_width = buffer.assigned_tile_width; updated.assigned_tile_height = buffer.assigned_tile_height;
        updated.assigned_content_x = buffer.assigned_content_x; updated.assigned_content_y = buffer.assigned_content_y;
        updated.assigned_content_width = buffer.assigned_content_width; updated.assigned_content_height = buffer.assigned_content_height;
        if (!retain_content) {
          updated.width = buffer.assigned_content_width;
          updated.height = buffer.assigned_content_height;
        }
        updated.x = buffer.x;
         updated.y = buffer.y;
         if (!retain_content) {
           updated.geometry_x = buffer.window_geometry_x;
           updated.geometry_y = buffer.window_geometry_y;
           updated.geometry_width = buffer.window_geometry_width;
           updated.geometry_height = buffer.window_geometry_height;
         }
        updated.stack_index = buffer.stack_index;
        updated.scene_order = buffer.scene_order;
        updated.background_blur_radius = buffer.background_blur_radius;
        updated.glass = buffer.glass;
        updated.toplevel = true;
        updated.overview_tag = buffer.overview_tag;
        updated.popup = buffer.popup;
        updated.focused = buffer.focused;
        updated.fullscreen = buffer.fullscreen;
        updated.compositor_opacity = buffer.compositor_opacity;
        updated.window_shader = buffer.window_shader;
        updated.border_shader = buffer.border_shader;
        if (!retain_content) updated.content_ready = buffer.content_ready;
        updated.removed = closing; updated.tag_outgoing = false;
        updated.suppress_geometry_animation = buffer.suppress_geometry_animation;
        updated.track_geometry_animation = buffer.track_geometry_animation;
        updated.camera_motion = buffer.camera_motion;
        updated.restore_scene = buffer.restore_scene;
        updated.camera_scale = buffer.camera_scale;
        updated.camera_center_x = buffer.camera_center_x;
        updated.camera_center_y = buffer.camera_center_y;
        updated.camera_offset_x = buffer.camera_offset_x;
        updated.camera_offset_y = buffer.camera_offset_y;
        updated.camera_world_x = buffer.camera_world_x;
        updated.camera_world_y = buffer.camera_world_y;
        updated.canvas_camera = buffer.canvas_camera;
        if (!retain_content) {
          updated.texture.reset();
          updated.owns_texture = false;
          updated.pixels.clear();
        }
        if (surface == card->shm_textures.end()) card->shm_textures.push_back(std::move(updated)); else *surface = std::move(updated);
        if (closing) {
          for (auto& item : card->shm_textures) if (item.root_id == buffer.surface_id) {
            if (item.texture != nullptr && !item.owns_texture) {
              item.texture = device->renderer->snapshot(item.texture);
              item.owns_texture = item.texture != nullptr;
              device->dmabuf_textures.erase(item.id);
            }
            item.content_ready = true;
            item.removed = true;
          }
        }
      } else if (surface != card->shm_textures.end()) {
        const auto retained_root = std::find_if(card->shm_textures.begin(), card->shm_textures.end(),
                                                [&](const auto& item) { return item.id == surface->root_id; });
        if (surface->tag_outgoing || (retained_root != card->shm_textures.end() && retained_root->removed)) {
          // Keep this scene intact until the output-level slide completes.
        } else if (surface->toplevel && config_->animations.enabled && config_->animations.close) {
          for (auto& item : card->shm_textures) if (item.root_id == surface->id) {
            if (item.texture != nullptr && !item.owns_texture) {
              item.texture = device->renderer->snapshot(item.texture);
              item.owns_texture = item.texture != nullptr;
              device->dmabuf_textures.erase(item.id);
            }
            item.removed = true;
          }
        } else {
          card->shm_textures.erase(surface);
        }
      }
      if (!card->tag_transition_preparing && !card->camera_frame_preparing) repaint(*card);
      continue;
    }
    if (buffer.width <= 0 || buffer.height <= 0 || buffer.width > std::numeric_limits<std::int32_t>::max() / 4 ||
        (buffer.pixels != nullptr &&
         ((buffer.format != protocol::WL_SHM_FORMAT_ARGB8888 && buffer.format != protocol::WL_SHM_FORMAT_XRGB8888) ||
          buffer.stride < buffer.width * 4))) {
      continue;
    }
    DrmOutput::ShmTexture updated = surface == card->shm_textures.end() ? DrmOutput::ShmTexture{} : std::move(*surface);
    if (buffer.pixels != nullptr) device->dmabuf_textures.erase(buffer.surface_id);
    const bool resized = updated.buffer_width != buffer.width || updated.buffer_height != buffer.height;
    const bool full_upload = buffer.pixels != nullptr && (resized || updated.pixels.empty() || updated.texture == nullptr ||
        !updated.owns_texture || updated.shm_generation != buffer.buffer_generation);
    updated.id = buffer.surface_id;
    updated.root_id = buffer.root_surface_id;
    updated.assigned_tile_x = buffer.assigned_tile_x; updated.assigned_tile_y = buffer.assigned_tile_y;
    updated.assigned_tile_width = buffer.assigned_tile_width; updated.assigned_tile_height = buffer.assigned_tile_height;
    updated.assigned_content_x = buffer.assigned_content_x; updated.assigned_content_y = buffer.assigned_content_y;
    updated.assigned_content_width = buffer.assigned_content_width; updated.assigned_content_height = buffer.assigned_content_height;
    updated.width = buffer.logical_width;
    updated.height = buffer.logical_height;
    updated.buffer_width = buffer.width;
    updated.buffer_height = buffer.height;
    updated.source_left = buffer.source_left; updated.source_top = buffer.source_top;
    updated.source_right = buffer.source_right; updated.source_bottom = buffer.source_bottom;
    updated.x = buffer.x;
    updated.y = buffer.y;
    updated.geometry_x = buffer.window_geometry_x;
    updated.geometry_y = buffer.window_geometry_y;
    updated.geometry_width = buffer.window_geometry_width;
    updated.geometry_height = buffer.window_geometry_height;
    updated.format = buffer.format;
    updated.stack_index = buffer.stack_index;
    updated.scene_order = buffer.scene_order;
    updated.scene_rank = buffer.scene_rank; updated.layer_priority = buffer.layer_priority;
    updated.root_order = buffer.root_order; updated.tree_order = buffer.tree_order;
    updated.compositor_opacity = buffer.compositor_opacity;
    updated.background_blur_radius = buffer.background_blur_radius;
    updated.background_blur_ignore_alpha = buffer.background_blur_ignore_alpha;
    updated.glass = buffer.glass;
    updated.layer_surface = buffer.layer_surface;
    updated.toplevel = buffer.toplevel;
    updated.overview_tag = buffer.overview_tag;
    updated.popup = buffer.popup;
    updated.fullscreen = buffer.fullscreen;
    updated.suppress_geometry_animation = buffer.suppress_geometry_animation;
    updated.track_geometry_animation = buffer.track_geometry_animation;
    updated.camera_motion = buffer.camera_motion;
    updated.restore_scene = buffer.restore_scene;
    updated.camera_scale = buffer.camera_scale;
    updated.camera_center_x = buffer.camera_center_x;
    updated.camera_center_y = buffer.camera_center_y;
    updated.camera_offset_x = buffer.camera_offset_x;
    updated.camera_offset_y = buffer.camera_offset_y;
    updated.camera_world_x = buffer.camera_world_x;
    updated.camera_world_y = buffer.camera_world_y;
    updated.canvas_camera = buffer.canvas_camera;
    updated.focused = buffer.focused;
    updated.window_shader = buffer.window_shader;
    updated.border_shader = buffer.border_shader;
    updated.opaque = buffer.opaque;
    updated.content_ready = buffer.content_ready;
    updated.removed = false; updated.tag_outgoing = false;
    if (buffer.pixels != nullptr && (resized || updated.pixels.empty())) updated.pixels.resize(static_cast<std::size_t>(buffer.width) * buffer.height);
    const int left = std::clamp(full_upload ? 0 : buffer.damage_x, 0, buffer.width);
    const int top = std::clamp(full_upload ? 0 : buffer.damage_y, 0, buffer.height);
    const int right = std::clamp(full_upload ? buffer.width : buffer.damage_x + buffer.damage_width, left, buffer.width);
    const int bottom = std::clamp(full_upload ? buffer.height : buffer.damage_y + buffer.damage_height, top, buffer.height);
    for (int y = top; buffer.pixels != nullptr && y < bottom; ++y) {
      std::memcpy(updated.pixels.data() + static_cast<std::size_t>(y) * buffer.width + left,
                  buffer.pixels + static_cast<std::size_t>(y) * buffer.stride + left * sizeof(std::uint32_t),
                  static_cast<std::size_t>(right - left) * sizeof(std::uint32_t));
      if (buffer.format == protocol::WL_SHM_FORMAT_XRGB8888) {
        auto* row = updated.pixels.data() + static_cast<std::size_t>(y) * buffer.width;
        for (int x = left; x < right; ++x) row[x] |= 0xff000000U;
      }
    }
    if (buffer.pixels != nullptr) {
      updated.shm_generation = buffer.buffer_generation;
      if (full_upload || (right > left && bottom > top)) {
        if (!updated.owns_texture) updated.texture.reset();
        const std::optional<renderer::Rect> damage = full_upload ? std::nullopt : std::optional<renderer::Rect>{
            {{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}}};
        auto uploaded = device->renderer->upload(updated.texture, updated.pixels.data(),
                                                 static_cast<std::uint32_t>(updated.buffer_width),
                                                 static_cast<std::uint32_t>(updated.buffer_height),
                                                 static_cast<std::uint32_t>(updated.buffer_width), damage);
        if (uploaded == nullptr)
          std::fprintf(stderr, "zwwm: DRM %s: shm upload failed for surface %llu: %s\n", device->path.c_str(),
                       static_cast<unsigned long long>(buffer.surface_id), device->renderer->last_error().c_str());
        updated.texture = std::move(uploaded);
        updated.owns_texture = updated.texture != nullptr;
      }
    } else {
      auto& imported = device->dmabuf_textures[buffer.surface_id];
      if (imported.generation != buffer.buffer_generation) {
        imported = DrmDevice::DmabufTexture{};
        imported.generation = buffer.buffer_generation;
        imported.texture = device->renderer->import_dmabuf(*buffer.dmabuf);
        imported.failed = imported.texture == nullptr;
        if (imported.failed) {
          std::fprintf(stderr, "zwwm: DRM %s: DMA-BUF import failed for surface %llu generation %llu: %s; skipping it on this device\n",
                       device->path.c_str(), static_cast<unsigned long long>(buffer.surface_id),
                       static_cast<unsigned long long>(buffer.buffer_generation),
                       device->renderer->last_error().c_str());
        }
      }
      updated.texture = imported.texture;
      updated.owns_texture = false;
      updated.pixels.clear();
      updated.dmabuf = *buffer.dmabuf;
      updated.buffer_id = buffer.buffer_id;
    }
    if (buffer.pixels != nullptr) {
      updated.dmabuf.reset();
      updated.buffer_id = 0;
    }
    if (surface == card->shm_textures.end()) card->shm_textures.push_back(std::move(updated)); else *surface = std::move(updated);
    std::stable_sort(card->shm_textures.begin(), card->shm_textures.end(), [](const auto& left, const auto& right) {
      return std::tie(left.scene_rank, left.layer_priority, left.root_order, left.tree_order) <
             std::tie(right.scene_rank, right.layer_priority, right.root_order, right.tree_order);
    });
    if (!card->tag_transition_preparing && !card->camera_frame_preparing) repaint(*card);
  }
  if (buffer.pixels == nullptr && buffer.dmabuf == nullptr && !buffer.toplevel) {
    for (auto& device : devices_) {
      const auto imported = device->dmabuf_textures.find(buffer.surface_id);
      if (imported == device->dmabuf_textures.end()) continue;
      const auto retained_by_transition = [&](const auto& outputs) {
        return std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
          return std::any_of(output->shm_textures.begin(), output->shm_textures.end(),
                             [&](const auto& surface) {
                               return surface.id == buffer.surface_id && (surface.tag_outgoing || surface.removed);
                             });
        });
      };
      if (retained_by_transition(device->outputs) || retained_by_transition(device->retired_outputs)) continue;
      device->dmabuf_textures.erase(imported);
    }
  }
}

int RuntimeBackend::on_seat_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_seat();
  return 0;
}

int RuntimeBackend::on_udev_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_udev();
  return 0;
}

int RuntimeBackend::on_libinput_fd(int, std::uint32_t, void* data) {
  static_cast<RuntimeBackend*>(data)->dispatch_libinput();
  return 0;
}

int RuntimeBackend::on_drm_fd(int, std::uint32_t, void* data) {
  auto* device = static_cast<DrmDevice*>(data);
  device->backend->dispatch_drm(*device);
  return 0;
}

void RuntimeBackend::enable_seat(libseat*, void* data) {
  auto* backend = static_cast<RuntimeBackend*>(data);
  std::fprintf(stderr, "zwwm: direct backend: seat activated, discovering DRM outputs\n");
  if (backend->libinput_ != nullptr && backend->libinput_suspended_) {
    if (libinput_resume(backend->libinput_) == 0) {
      backend->libinput_suspended_ = false;
    } else {
      backend->last_error_ = "libinput could not resume after seat activation";
    }
  }
  backend->active_ = true;
  backend->resume_drm_devices();
  if (backend->udev_ != nullptr) {
    backend->discover_drm_devices();
  }
  backend->probe_attempts_ = 0;
  backend->schedule_output_probe();
}

void RuntimeBackend::disable_seat(libseat* seat, void* data) {
  auto* backend = static_cast<RuntimeBackend*>(data);
  backend->active_ = false;
  backend->left_control_pressed_ = false;
  backend->right_control_pressed_ = false;
  backend->left_alt_pressed_ = false;
  backend->right_alt_pressed_ = false;
  backend->consumed_vt_key_ = 0;
  backend->touch_outputs_.clear();
  if (backend->input_target_ != nullptr) backend->input_target_->input_reset(true);
  if (backend->libinput_ != nullptr && !backend->libinput_suspended_) {
    libinput_suspend(backend->libinput_);
    backend->libinput_suspended_ = true;
  }
  backend->suspend_drm_devices();
  libseat_disable_seat(seat);
}

int RuntimeBackend::open_restricted(const char* path, int flags, void* data) {
  return static_cast<RuntimeBackend*>(data)->open_device(path, flags);
}

void RuntimeBackend::close_restricted(int fd, void* data) { static_cast<RuntimeBackend*>(data)->close_device(fd); }

void RuntimeBackend::dispatch_seat() {
  if (libseat_dispatch(seat_, 0) < 0) {
    last_error_ = "libseat event dispatch failed";
  }
}

void RuntimeBackend::dispatch_udev() {
  while (udev_device* device = udev_monitor_receive_device(udev_monitor_)) {
    const char* path = udev_device_get_devnode(device);
    const char* action = udev_device_get_action(device);
    if (action != nullptr) {
      udev_device* parent = is_drm_card(path) ? nullptr :
          udev_device_get_parent_with_subsystem_devtype(device, "drm", "drm_minor");
      const char* card_path = is_drm_card(path) ? path : parent == nullptr ? nullptr : udev_device_get_devnode(parent);
      const auto rescan = [&] {
        const auto drm_device = std::find_if(devices_.begin(), devices_.end(), [card_path](const auto& item) {
          return item->path == card_path;
        });
        if (drm_device == devices_.end()) { add_drm_device(card_path); return; }
        rescan_drm_device(**drm_device);
        publish_outputs();
        publish_dmabuf_importer();
        for (auto& output : (*drm_device)->outputs) repaint(*output);
      };
      if (std::strcmp(action, "add") == 0 && active_ && is_drm_card(card_path) && is_drm_card(path)) {
        add_drm_device(card_path);
      } else if (std::strcmp(action, "remove") == 0 && is_drm_card(card_path) && is_drm_card(path)) {
        remove_drm_device(card_path);
      } else if (active_ && is_drm_card(card_path) &&
                 (std::strcmp(action, "add") == 0 || std::strcmp(action, "remove") == 0 ||
                  std::strcmp(action, "change") == 0)) rescan();
      else if (active_ && !is_drm_card(card_path) &&
               (std::strcmp(action, "add") == 0 || std::strcmp(action, "remove") == 0 ||
                std::strcmp(action, "change") == 0)) {
        for (auto& drm_device : devices_) rescan_drm_device(*drm_device);
        publish_outputs();
        publish_dmabuf_importer();
        for (auto& drm_device : devices_) for (auto& output : drm_device->outputs) repaint(*output);
      }
    }
    udev_device_unref(device);
  }
}

void RuntimeBackend::dispatch_libinput() {
  if (libinput_dispatch(libinput_) != 0) {
    last_error_ = "libinput event dispatch failed";
    return;
  }
  while (libinput_event* event = libinput_get_event(libinput_)) {
    if (input_target_ != nullptr) {
      const auto type = libinput_event_get_type(event);
      if (type == LIBINPUT_EVENT_POINTER_MOTION) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const double relative_dx = libinput_event_pointer_get_dx(pointer);
        const double relative_dy = libinput_event_pointer_get_dy(pointer);
        const double relative_dx_unaccelerated = libinput_event_pointer_get_dx_unaccelerated(pointer);
        const double relative_dy_unaccelerated = libinput_event_pointer_get_dy_unaccelerated(pointer);
        const OutputConfig* pointer_config = &config_->output;
        for (const auto& device : devices_) for (const auto& output : device->outputs) {
          if (cursor_x_ >= output->output.logical_x && cursor_y_ >= output->output.logical_y &&
              cursor_x_ < output->output.logical_x + output->output.logical_width &&
              cursor_y_ < output->output.logical_y + output->output.logical_height)
            pointer_config = &config_->output_for(output->output.connector);
        }
        double dx = relative_dx * 1000.0 / pointer_config->scale_per_mille;
        double dy = relative_dy * 1000.0 / pointer_config->scale_per_mille;
        double dx_unaccelerated = relative_dx_unaccelerated * 1000.0 / pointer_config->scale_per_mille;
        double dy_unaccelerated = relative_dy_unaccelerated * 1000.0 / pointer_config->scale_per_mille;
        const auto transform_delta = [pointer_config](double& x, double& y) {
          if (pointer_config->transform == OutputTransform::rotate_90) { const double next = y; y = -x; x = next; }
          else if (pointer_config->transform == OutputTransform::rotate_180) { x = -x; y = -y; }
          else if (pointer_config->transform == OutputTransform::rotate_270) { const double next = -y; y = x; x = next; }
        };
        transform_delta(dx, dy);
        transform_delta(dx_unaccelerated, dy_unaccelerated);
        input_target_->pointer_relative_motion(libinput_event_pointer_get_time_usec(pointer), relative_dx, relative_dy,
                                               relative_dx_unaccelerated, relative_dy_unaccelerated);
        const bool panning = input_target_->pointer_pan_motion(dx, dy);
        if (!panning) {
          cursor_x_ = std::clamp(cursor_x_ + dx, 0.0, static_cast<double>(output_width_ - 1));
          cursor_y_ = std::clamp(cursor_y_ + dy, 0.0, static_cast<double>(output_height_ - 1));
        }
        const bool inside = std::any_of(devices_.begin(), devices_.end(), [&](const auto& device) {
          return std::any_of(device->outputs.begin(), device->outputs.end(), [&](const auto& card) {
          const auto& output = card->output;
          return cursor_x_ >= output.logical_x && cursor_y_ >= output.logical_y &&
                 cursor_x_ < output.logical_x + output.logical_width &&
                 cursor_y_ < output.logical_y + output.logical_height;
          });
        });
        std::vector<DrmOutput*> outputs;
        for (auto& device : devices_) for (auto& output : device->outputs) outputs.push_back(output.get());
        if (!inside && !outputs.empty()) {
          const auto nearest = std::min_element(outputs.begin(), outputs.end(), [&](const auto* left, const auto* right) {
            const auto distance = [&](const auto& card) {
              const auto& output = card->output;
              const double x = std::clamp(cursor_x_, static_cast<double>(output.logical_x),
                                          static_cast<double>(output.logical_x + output.logical_width - 1));
              const double y = std::clamp(cursor_y_, static_cast<double>(output.logical_y),
                                          static_cast<double>(output.logical_y + output.logical_height - 1));
              return std::abs(cursor_x_ - x) + std::abs(cursor_y_ - y);
            };
            return distance(left) < distance(right);
          });
          const auto& output = (*nearest)->output;
          cursor_x_ = std::clamp(cursor_x_, static_cast<double>(output.logical_x),
                                 static_cast<double>(output.logical_x + output.logical_width - 1));
          cursor_y_ = std::clamp(cursor_y_, static_cast<double>(output.logical_y),
                                 static_cast<double>(output.logical_y + output.logical_height - 1));
        }
        if (!panning)
          input_target_->pointer_motion_global(libinput_event_pointer_get_time(pointer), static_cast<std::int32_t>(std::lround(cursor_x_)), static_cast<std::int32_t>(std::lround(cursor_y_)));
        for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
      } else if (type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const renderer::Point physical{
            static_cast<std::int32_t>(libinput_event_pointer_get_absolute_x_transformed(pointer, physical_output_width_)),
            static_cast<std::int32_t>(libinput_event_pointer_get_absolute_y_transformed(pointer, physical_output_height_))};
        const auto logical = config_->output.physical_to_logical(physical, {physical_output_width_, physical_output_height_});
        cursor_x_ = std::clamp(static_cast<double>(logical.x), 0.0, static_cast<double>(output_width_ - 1));
        cursor_y_ = std::clamp(static_cast<double>(logical.y), 0.0, static_cast<double>(output_height_ - 1));
        input_target_->pointer_motion_global(libinput_event_pointer_get_time(pointer), static_cast<std::int32_t>(std::lround(cursor_x_)), static_cast<std::int32_t>(std::lround(cursor_y_)));
        for (auto& device : devices_) for (auto& output : device->outputs) repaint(*output);
      } else if (type == LIBINPUT_EVENT_TOUCH_FRAME) {
        input_target_->touch_frame();
      } else if (type == LIBINPUT_EVENT_TOUCH_CANCEL) {
        input_target_->touch_cancel();
        touch_outputs_.clear();
      } else if (type == LIBINPUT_EVENT_TOUCH_UP) {
        auto* touch = libinput_event_get_touch_event(event);
        const auto slot = libinput_event_touch_get_seat_slot(touch);
        input_target_->touch_up(libinput_event_touch_get_time(touch), slot);
        touch_outputs_.erase(slot);
      } else if (type == LIBINPUT_EVENT_TOUCH_DOWN || type == LIBINPUT_EVENT_TOUCH_MOTION) {
        auto* touch = libinput_event_get_touch_event(event);
        const auto slot = libinput_event_touch_get_seat_slot(touch);
        DrmOutput* selected = nullptr;
        const auto assigned = touch_outputs_.find(slot);
        for (const auto& device : devices_) for (const auto& output : device->outputs) {
          if (assigned != touch_outputs_.end()) {
            if (output->output.id == assigned->second) selected = output.get();
          } else if (type == LIBINPUT_EVENT_TOUCH_DOWN &&
                     (selected == nullptr || output->output.connector < selected->output.connector)) {
            selected = output.get();
          }
        }
        if (selected != nullptr) {
          const auto& info = selected->output;
          const renderer::Point physical{
              std::clamp(static_cast<std::int32_t>(std::lround(
                  libinput_event_touch_get_x_transformed(touch, info.physical_width))),
                  0, static_cast<std::int32_t>(info.physical_width) - 1),
              std::clamp(static_cast<std::int32_t>(std::lround(
                  libinput_event_touch_get_y_transformed(touch, info.physical_height))),
                  0, static_cast<std::int32_t>(info.physical_height) - 1)};
          const auto logical = config_->output_for(info.connector).physical_to_logical(
              physical, {info.physical_width, info.physical_height});
          const auto time = libinput_event_touch_get_time(touch);
          if (type == LIBINPUT_EVENT_TOUCH_DOWN) {
            touch_outputs_[slot] = info.id;
            input_target_->touch_down(time, slot, info.logical_x + logical.x, info.logical_y + logical.y);
          } else input_target_->touch_motion(time, slot, info.logical_x + logical.x, info.logical_y + logical.y);
        } else if (assigned != touch_outputs_.end()) {
          input_target_->touch_cancel();
          touch_outputs_.clear();
        }
      } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
        auto* pointer = libinput_event_get_pointer_event(event);
        input_target_->pointer_button(libinput_event_pointer_get_time(pointer), libinput_event_pointer_get_button(pointer), libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED ? protocol::WL_POINTER_BUTTON_STATE_PRESSED : protocol::WL_POINTER_BUTTON_STATE_RELEASED);
      } else if (type == LIBINPUT_EVENT_POINTER_AXIS) {
        auto* pointer = libinput_event_get_pointer_event(event);
        const auto source = libinput_event_pointer_get_axis_source(pointer);
        const auto wayland_source = source == LIBINPUT_POINTER_AXIS_SOURCE_FINGER ? protocol::WL_POINTER_AXIS_SOURCE_FINGER : source == LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS ? protocol::WL_POINTER_AXIS_SOURCE_CONTINUOUS : source == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT ? protocol::WL_POINTER_AXIS_SOURCE_WHEEL_TILT : protocol::WL_POINTER_AXIS_SOURCE_WHEEL;
        for (const auto axis : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
          if (!libinput_event_pointer_has_axis(pointer, axis)) continue;
          const auto wl_axis = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? protocol::WL_POINTER_AXIS_VERTICAL_SCROLL : protocol::WL_POINTER_AXIS_HORIZONTAL_SCROLL;
          const auto value = libinput_event_pointer_get_axis_value(pointer, axis);
          const auto discrete = libinput_event_pointer_get_axis_value_discrete(pointer, axis);
          const auto value120 = discrete * 120;
          input_target_->pointer_axis(libinput_event_pointer_get_time(pointer), wl_axis, value, wayland_source, discrete, value120, value == 0.0);
        }
      } else if (gesture_observer_ != nullptr &&
                 (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                  type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ||
                  type == LIBINPUT_EVENT_GESTURE_SWIPE_END ||
                  type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN ||
                  type == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE ||
                  type == LIBINPUT_EVENT_GESTURE_PINCH_END ||
                  type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN ||
                  type == LIBINPUT_EVENT_GESTURE_HOLD_END)) {
        auto* gesture = libinput_event_get_gesture_event(event);
        ZwwmPluginGesture input{};
        input.kind = type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                     type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ||
                     type == LIBINPUT_EVENT_GESTURE_SWIPE_END ? ZwwmGestureKind::swipe :
                     type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN ||
                     type == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE ||
                     type == LIBINPUT_EVENT_GESTURE_PINCH_END ? ZwwmGestureKind::pinch :
                     ZwwmGestureKind::hold;
        input.phase = type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                      type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN ||
                      type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN ? ZwwmGesturePhase::begin :
                      type == LIBINPUT_EVENT_GESTURE_SWIPE_END ||
                      type == LIBINPUT_EVENT_GESTURE_PINCH_END ||
                      type == LIBINPUT_EVENT_GESTURE_HOLD_END ? ZwwmGesturePhase::end :
                      ZwwmGesturePhase::update;
        input.fingers = libinput_event_gesture_get_finger_count(gesture);
        input.time_usec = libinput_event_gesture_get_time_usec(gesture);
        input.scale = 1.0;
        input.output = "";
        input.device = libinput_device_get_sysname(libinput_event_get_device(event));
        const OutputConfig* output_config = &config_->output;
        for (const auto& device : devices_) for (const auto& output : device->outputs) {
          const auto& info = output->output;
          if (cursor_x_ >= info.logical_x && cursor_y_ >= info.logical_y &&
              cursor_x_ < info.logical_x + info.logical_width &&
              cursor_y_ < info.logical_y + info.logical_height) {
            input.output = info.connector.c_str();
            output_config = &config_->output_for(info.connector);
          }
        }
        if (input.phase == ZwwmGesturePhase::update) {
          input.dx = libinput_event_gesture_get_dx(gesture) * 1000.0 / output_config->scale_per_mille;
          input.dy = libinput_event_gesture_get_dy(gesture) * 1000.0 / output_config->scale_per_mille;
          if (output_config->transform == OutputTransform::rotate_90) {
            const double x = input.dy; input.dy = -input.dx; input.dx = x;
          } else if (output_config->transform == OutputTransform::rotate_180) {
            input.dx = -input.dx; input.dy = -input.dy;
          } else if (output_config->transform == OutputTransform::rotate_270) {
            const double x = -input.dy; input.dy = input.dx; input.dx = x;
          }
          if (input.kind == ZwwmGestureKind::pinch) {
            input.scale = libinput_event_gesture_get_scale(gesture);
            input.angle_delta = libinput_event_gesture_get_angle_delta(gesture);
          }
        }
        if (input.phase == ZwwmGesturePhase::end)
          input.cancelled = libinput_event_gesture_get_cancelled(gesture) != 0;
        gesture_observer_(gesture_data_, input);
      } else if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
        auto* keyboard = libinput_event_get_keyboard_event(event);
        const auto key = libinput_event_keyboard_get_key(keyboard);
        const bool pressed = libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED;
        if (key == KEY_LEFTCTRL) left_control_pressed_ = pressed;
        else if (key == KEY_RIGHTCTRL) right_control_pressed_ = pressed;
        else if (key == KEY_LEFTALT) left_alt_pressed_ = pressed;
        else if (key == KEY_RIGHTALT) right_alt_pressed_ = pressed;

        bool consumed = key == consumed_vt_key_;
        if (consumed && !pressed) consumed_vt_key_ = 0;
        if (!consumed && pressed && (left_control_pressed_ || right_control_pressed_) &&
            (left_alt_pressed_ || right_alt_pressed_)) {
          int vt = 0;
          if (key >= KEY_F1 && key <= KEY_F12) vt = static_cast<int>(key - KEY_F1 + 1);
          else if (key >= KEY_1 && key <= KEY_9) vt = static_cast<int>(key - KEY_1 + 1);
          else if (key == KEY_0) vt = 10;
          if (vt != 0 && libseat_switch_session(seat_, vt) == 0) {
            consumed_vt_key_ = key;
            consumed = true;
          }
        }
        if (!consumed)
          input_target_->keyboard_key(libinput_event_keyboard_get_time(keyboard), key,
                                      pressed ? protocol::WL_KEYBOARD_KEY_STATE_PRESSED : protocol::WL_KEYBOARD_KEY_STATE_RELEASED);
      }
    }
    libinput_event_destroy(event);
  }
}

void RuntimeBackend::dispatch_drm(DrmDevice& device) {
  drmEventContext events{};
  events.version = DRM_EVENT_CONTEXT_VERSION;
  events.page_flip_handler = [](int, unsigned int, unsigned int, unsigned int, void* data) {
    auto* output = static_cast<DrmOutput*>(data);
    output->device->backend->page_flip_complete(*output);
  };
  if (drmHandleEvent(device.fd, &events) != 0) {
    std::fprintf(stderr, "zwwm: failed to dispatch DRM event for %s\n", device.path.c_str());
  }
}

void RuntimeBackend::repaint(DrmOutput& card) {
  if (!active_ || card.retired || card.powered_off) return;
  if (card.camera_frame_preparing || card.tag_transition_preparing) { card.needs_repaint = true; return; }
  auto& device = *card.device;
  const auto& output_config = config_->output_for(card.output.connector);
  if (card.flip_pending) { card.needs_repaint = true; return; }
  if (card.crtc_id == 0 || card.connector_id == 0 || card.scanout.empty() || device.renderer == nullptr) return;
  renderer::FramePlan frame;
  frame.draw_background = true;
  frame.damage.push_back({{0, 0}, {static_cast<std::uint32_t>(card.mode.hdisplay), static_cast<std::uint32_t>(card.mode.vdisplay)}});
  const renderer::Size physical{static_cast<std::uint32_t>(card.mode.hdisplay), static_cast<std::uint32_t>(card.mode.vdisplay)};
  const std::uint64_t now = monotonic_ms();
  frame.shader_time = static_cast<float>(now % 1000000U) / 1000.0F;
  frame.overview_backdrop = card.overview;
  frame.overview_backdrop_opacity = static_cast<float>(card.overview_progress);
  frame.overview_backdrop_blur_radius = config_->overview_backdrop_blur_radius;
  frame.overview_backdrop_blur_passes = config_->overview_backdrop_blur_passes;
  if (card.overview) backend_scene::overview_scenes(frame,
      {{card.output.logical_x, card.output.logical_y}, {card.output.logical_width, card.output.logical_height}},
      card.overview_scroll, card.overview_active_tag, card.overview_progress, config_->background_shader, config_->overview_scene_radius,
      config_->overview_stack_master, config_->overview_master_ratio / 100.0);
  const auto tag = card.overview ? std::nullopt : card.animations.tag_transition(now);
  if (!tag) {
    for (auto& surface : card.shm_textures) {
      if (!surface.tag_outgoing) continue;
      card.animations.remove(surface.root_id);
      surface.removed = true;
    }
  }
  std::vector<AnimationTarget> animation_targets;
  animation_targets.reserve(card.shm_textures.size());
  for (const auto& surface : card.shm_textures) {
    const auto bounds = assigned_tile(surface);
    const renderer::Rect viewport{{card.output.logical_x, card.output.logical_y},
                                  {card.output.logical_width, card.output.logical_height}};
    if (tag && surface.toplevel && !backend_scene::intersects(bounds, viewport)) {
      card.animations.remove(surface.id);
      continue;
    }
    if (surface.toplevel && !surface.removed && surface.content_ready && surface.texture != nullptr)
      animation_targets.push_back(
          {surface.id, bounds, !surface.suppress_geometry_animation,
            !surface.restore_scene && (!tag || surface.tag_outgoing), surface.track_geometry_animation,
            surface.camera_motion || surface.restore_scene});
  }
  card.animations.update(animation_targets, now);
  for (auto& surface : card.shm_textures) {
    surface.track_geometry_animation = false;
    surface.camera_motion = false;
    surface.restore_scene = false;
  }
  for (auto it = card.shm_textures.begin(); it != card.shm_textures.end();) {
    if (it->removed && !card.animations.retains(it->root_id)) {
      card.device->dmabuf_textures.erase(it->id);
      it = card.shm_textures.erase(it);
    } else ++it;
  }
  std::vector<layout::Placement> placements;
  for (const auto id : card.animations.retained_ids()) {
    const auto sample = card.animations.sample(id, now);
    if (sample) placements.push_back({id, sample->bounds});
  }
  const auto apply_blur_parameters = [&](renderer::DrawCall& draw, std::string_view shader = {}) {
    draw.background_blur_passes = static_cast<std::uint32_t>(std::clamp(
        config_->shader_integer(shader, "blur_passes", 3), 1, 8));
    draw.background_blur_brightness = std::clamp(
        config_->shader_integer(shader, "blur_brightness", 100), 0, 200) / 100.0F;
    draw.background_blur_contrast = std::clamp(
        config_->shader_integer(shader, "blur_contrast", 100), 0, 200) / 100.0F;
    draw.background_blur_saturation = std::clamp(
        config_->shader_integer(shader, "blur_saturation", 100), 0, 200) / 100.0F;
    draw.background_blur_noise = std::clamp(
        config_->shader_integer(shader, "blur_noise", 0), 0, 100) / 100.0F;
  };
  // Tiled shadows go under all tiled windows.
  std::optional<std::size_t> tiled_shadows_at;
  std::vector<renderer::DrawCall> tiled_shadows;
  for (const auto& surface : card.shm_textures) {
    const auto placement = std::find_if(placements.begin(), placements.end(), [&surface](const auto& item) { return item.id == surface.root_id; });
    const auto root = std::find_if(card.shm_textures.begin(), card.shm_textures.end(), [&surface](const auto& item) { return item.id == surface.root_id; });
    if (root == card.shm_textures.end() || !root->content_ready || surface.texture == nullptr || root->width <= 0 || root->height <= 0) continue;
    if (!root->toplevel) {
      frame.draws.push_back({surface.id, {{surface.x, surface.y}, {static_cast<std::uint32_t>(surface.width), static_cast<std::uint32_t>(surface.height)}}, surface.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture, 0.0F, 0.0F, {surface.source_left, surface.source_top, surface.source_right, surface.source_bottom}});
      frame.draws.back().opaque = surface.opaque && surface.compositor_opacity == 1.0F;
      frame.draws.back().background_blur_radius = static_cast<float>(surface.background_blur_radius);
      frame.draws.back().background_mask_texture = surface.layer_surface;
      frame.draws.back().background_mask_alpha_threshold = surface.background_blur_ignore_alpha;
      apply_blur_parameters(frame.draws.back());
      if (card.overview && root->layer_surface) {
        auto draw = std::move(frame.draws.back());
        frame.draws.pop_back();
        const renderer::Rect area{{card.output.logical_x, card.output.logical_y}, {card.output.logical_width, card.output.logical_height}};
        for (unsigned scene = 1; scene <= 9; ++scene) {
          auto projected = backend_scene::overview_layer(draw, area, card.overview_scroll, card.overview_active_tag, card.overview_progress, scene,
              config_->overview_scene_radius, config_->overview_stack_master, config_->overview_master_ratio / 100.0);
          if (backend_scene::overview_draw_visible(projected, area)) frame.draws.push_back(std::move(projected));
        }
      }
      continue;
    }
    renderer::Rect tag_bounds;
    if (placement != placements.end()) tag_bounds = placement->bounds;
    else continue;
    const auto tag_root = tag ? transform_tag_root(tag_bounds, card.output.logical_width, card.output.logical_height, *tag, root->tag_outgoing)
                               : TagRootSample{tag_bounds, 1.0F};
    tag_bounds = tag_root.bounds;
    const float decoration_scale = animated_decoration_scale(*root, tag_bounds);
    const float border_width = config_->border_width() * decoration_scale;
    const float inner_radius = config_->radius() * decoration_scale;
    const float outer_radius = inner_radius + border_width;
    const auto content = animated_content(*root, tag_bounds);
    const int content_width = static_cast<int>(content.size.width);
    const int content_height = static_cast<int>(content.size.height);
    if (content_width <= 0 || content_height <= 0) continue;
    const auto animation = card.animations.sample(surface.root_id, now);
    const auto geometry = surface.toplevel ? window_geometry(surface) : window_geometry(*root);
    const int goal_width = root->assigned_content_width > 0 ? root->assigned_content_width : geometry.width;
    const int goal_height = root->assigned_content_height > 0 ? root->assigned_content_height : geometry.height;
    const renderer::Rect root_texture_bounds{
        content.origin,
        {static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.width) * content_width / goal_width)),
         static_cast<std::uint32_t>(std::max<std::int64_t>(
             1, static_cast<std::int64_t>(geometry.height) * content_height / goal_height))}};
    const int relative_x = surface.x - root->x;
    const int relative_y = surface.y - root->y;
    const renderer::Rect bounds = surface.toplevel
                                        ? tag_bounds
                                       : renderer::Rect{{content.origin.x + (relative_x - geometry.x) * content_width / goal_width,
                                                          content.origin.y + (relative_y - geometry.y) * content_height / goal_height},
                                                        {static_cast<std::uint32_t>(std::max(1, surface.width * content_width / goal_width)),
                                                          static_cast<std::uint32_t>(std::max(1, surface.height * content_height / goal_height))}};
    const bool root_surface = surface.toplevel;
    if (root_surface && !root->fullscreen && !card.overview) {
      const std::uint32_t state = surface.focused ? 1U : 0U;
      if (auto shadow = backend_scene::shadow_draw(*config_, surface.id, tag_bounds, outer_radius, decoration_scale,
              (animation ? animation->opacity : 1.0F) * tag_root.opacity * surface.compositor_opacity, state,
              frame.shader_time)) {
        backend_scene::camera_geometry(*shadow, *root, surface, tag_bounds);
        if (root->layer_priority == 0) {
          if (!tiled_shadows_at) tiled_shadows_at = frame.draws.size();
          tiled_shadows.push_back(std::move(*shadow));
        } else {
          frame.draws.push_back(std::move(*shadow));
        }
      }
    }
    frame.draws.push_back({surface.id, bounds,
                                 (animation ? animation->opacity : 1.0F) * tag_root.opacity * surface.compositor_opacity, {1.0F, 1.0F, 1.0F, 1.0F}, surface.texture,
                                  root_surface && !root->fullscreen ? outer_radius : 0.0F,
                              0.0F, surface.toplevel ? source_uv(geometry, surface) : std::array<float, 4>{surface.source_left, surface.source_top, surface.source_right, surface.source_bottom},
                                surface.popup ? std::nullopt : std::optional<renderer::Rect>{content},
                                  (surface.popup || root->fullscreen) ? 0.0F : inner_radius});
    frame.draws.back().opaque = surface.opaque;
    frame.draws.back().shader_role = renderer::ShaderRole::window;
    frame.draws.back().shader_name = surface.window_shader;
    frame.draws.back().toplevel_id = surface.root_id;
    frame.draws.back().toplevel_bounds = tag_bounds;
    frame.draws.back().toplevel_state = (surface.focused ? 1U : 0U) |
        (root->fullscreen ? 2U : 0U) | (surface.popup ? 4U : 0U);
    frame.draws.back().shader_time = frame.shader_time;
    frame.draws.back().shared_backdrop = root_surface && root->layer_priority == 0 && !root->fullscreen;
    if (root_surface) {
      frame.draws.back().texture_bounds = root_texture_bounds;
      frame.draws.back().texture_corner_radius = root->fullscreen ? 0.0F : inner_radius;
    }
    frame.draws.back().background_blur_radius = static_cast<float>(surface.background_blur_radius);
    apply_blur_parameters(frame.draws.back(), surface.window_shader);
    backend_scene::camera_geometry(frame.draws.back(), *root, surface, tag_bounds);
    if (root_surface && !root->fullscreen && std::lround(border_width) > 0) backend_scene::extend_under_border(frame.draws.back());
    if (card.overview && !backend_scene::overview_draw_visible(frame.draws.back(),
        {{card.output.logical_x, card.output.logical_y}, {card.output.logical_width, card.output.logical_height}})) frame.draws.pop_back();
    if (surface.toplevel && !root->fullscreen) {
      const auto border_animation = card.animations.sample(surface.id, now);
      if (auto border = backend_scene::border_draw(surface.id, content, tag_bounds, border_width, inner_radius,
              (border_animation ? border_animation->opacity : 1.0F) * tag_root.opacity, surface.focused ? 1U : 0U,
              frame.shader_time)) {
        border->shader_name = surface.border_shader;
        border->toplevel_id = surface.root_id;
        backend_scene::camera_geometry(*border, *root, surface, tag_bounds);
        frame.draws.push_back(std::move(*border));
      }
    }
  }
  if (tiled_shadows_at)
    frame.draws.insert(frame.draws.begin() + static_cast<std::ptrdiff_t>(*tiled_shadows_at),
                       std::make_move_iterator(tiled_shadows.begin()), std::make_move_iterator(tiled_shadows.end()));
  if (error_popup_visible_ && error_popup_) {
    if (device.error_popup_generation != error_popup_generation_ || device.error_popup_texture == nullptr) {
      device.error_popup_texture = device.renderer->upload(nullptr, error_popup_.pixels.data(), error_popup_.width,
                                                           error_popup_.height, error_popup_.width);
      device.error_popup_generation = error_popup_generation_;
    }
    const auto popup_x = card.output.logical_x + std::max(8, (static_cast<int>(card.output.logical_width) -
                                                              static_cast<int>(error_popup_.width)) / 2);
    if (device.error_popup_texture != nullptr)
      frame.draws.push_back({std::numeric_limits<std::uint64_t>::max() - 1,
                             {{popup_x, card.output.logical_y + 24}, {error_popup_.width, error_popup_.height}},
                             1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, device.error_popup_texture});
  }
  renderer::FramePlan cursor_frame;
  if (cursor_visible_ && card.cursor_texture != nullptr && card.cursor_width > 0 && card.cursor_height > 0) {
    cursor_frame.draws.push_back({std::numeric_limits<std::uint64_t>::max(),
                           {{static_cast<std::int32_t>(std::lround(cursor_x_)) - card.cursor_hotspot_x,
                             static_cast<std::int32_t>(std::lround(cursor_y_)) - card.cursor_hotspot_y},
                            {static_cast<std::uint32_t>(card.cursor_width), static_cast<std::uint32_t>(card.cursor_height)}},
                           1.0F, {1.0F, 1.0F, 1.0F, 1.0F}, card.cursor_texture});
  }
  const bool continue_animation = card.animations.active(now);
  const renderer::Point logical_origin{card.output.logical_x, card.output.logical_y};
  transform_frame(frame, output_config, physical, logical_origin);
  transform_frame(cursor_frame, output_config, physical, logical_origin);

  if (!card.overview && !tag && !continue_animation && !error_popup_visible_ && cursor_frame.draws.empty() &&
      !card.needs_modeset && now - last_capture_ms_ > 2000U && try_direct_scanout(card)) return;
  int next = -1;
  for (int index = 0; index < static_cast<int>(card.scanout.size()); ++index)
    if (index != card.front_buffer && index != card.pending_buffer) { next = index; break; }
  if (next < 0) { card.needs_repaint = true; return; }
  auto& buffer = card.scanout[static_cast<std::size_t>(next)];
  auto& renderer = *device.renderer;
  if (card.capture_texture == nullptr || card.capture_texture->width() != physical.width ||
      card.capture_texture->height() != physical.height)
    card.capture_texture = renderer.create_target(physical);
  if (card.capture_texture == nullptr) {
    last_error_ = "could not allocate the offscreen frame: " + renderer.last_error();
    std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str());
    return;
  }
  const bool recorded = renderer.begin() &&
      renderer.render(frame, card.capture_texture, std::array<float, 4>{0.125F, 0.141F, 0.169F, 1.0F}) &&
      renderer.copy(card.capture_texture, buffer.image) &&
      (cursor_frame.draws.empty() || renderer.render(cursor_frame, buffer.image));
  // Submit even partial work so imported buffers return to their owners.
  const bool submitted = renderer.recording() && renderer.submit();
  if (!recorded || !submitted) {
    last_error_ = "could not render direct frame: " + renderer.last_error();
    std::fprintf(stderr, "zwwm: %s\n", last_error_.c_str());
    return;
  }
  card.capture_ready = true;
  if (card.needs_modeset) {
    if (!commit_atomic_output(card, buffer.framebuffer, true)) return;
    card.front_buffer = next;
    card.needs_modeset = false;
    card.needs_repaint = false;
    if (input_target_ != nullptr) input_target_->notify_frame_presented(card.output.id);
    if (continue_animation) repaint(card);
    return;
  }
  card.pending_buffer = next;
  card.flip_pending = true;
  card.needs_repaint = continue_animation;
  if (!commit_atomic_output(card, buffer.framebuffer, false)) {
    card.flip_pending = false;
    card.pending_buffer = -1;
  }
}

void RuntimeBackend::page_flip_complete(DrmOutput& card) {
  auto& device = *card.device;
  if (!card.flip_pending) return;
  // The frame that just left the screen can go back to its client.
  if (card.direct_front) release_direct_frame(card, card.direct_front->framebuffer, card.direct_front->buffer_id);
  card.direct_front = card.direct_pending;
  card.direct_pending.reset();
  card.front_buffer = card.pending_buffer;
  card.pending_buffer = -1;
  card.flip_pending = false;
  if (card.retired) {
    auto* retired = &card;
    close_drm_output(card);
    std::erase_if(device.retired_outputs, [retired](const auto& output) { return output.get() == retired; });
    rescan_drm_device(device);
    publish_outputs();
    publish_dmabuf_importer();
    for (auto& output : device.outputs) repaint(*output);
    return;
  }
  if (card.powered_off) {
    if (!disable_atomic_output(card))
      std::fprintf(stderr, "zwwm: DRM %s: could not power off output %u: %s\n", device.path.c_str(), card.connector_id,
                   std::strerror(errno));
    return;
  }
  if (input_target_ != nullptr) input_target_->notify_frame_presented(card.output.id);
  if (card.needs_repaint) repaint(card);
}

void RuntimeBackend::release_direct_frame(DrmOutput& card, std::uint32_t framebuffer, std::uint64_t buffer_id) {
  if (framebuffer != 0) drmModeRmFB(card.device->fd, framebuffer);
  if (input_target_ != nullptr) input_target_->release_buffer_hold(buffer_id);
}

// A fullscreen opaque DMA-BUF that exactly fills the mode goes straight to the primary plane.
bool RuntimeBackend::try_direct_scanout(DrmOutput& card) {
  if (input_target_ == nullptr || card.flip_pending || card.output.transform != OutputTransform::normal) return false;
  const DrmOutput::ShmTexture* candidate = nullptr;
  for (auto it = card.shm_textures.rbegin(); it != card.shm_textures.rend(); ++it) {
    if (it->removed || it->texture == nullptr || !it->content_ready) continue;
    candidate = &*it;
    break;
  }
  if (candidate == nullptr || !candidate->toplevel || !candidate->fullscreen || !candidate->opaque || !candidate->dmabuf ||
      candidate->buffer_id == 0 || candidate->buffer_id == card.direct_failed_buffer ||
      candidate->compositor_opacity < 1.0F || candidate->glass || candidate->background_blur_radius != 0 ||
      candidate->source_left != 0.0F || candidate->source_top != 0.0F || candidate->source_right != 1.0F ||
      candidate->source_bottom != 1.0F) return false;
  const auto& attributes = *candidate->dmabuf;
  if (attributes.width != static_cast<std::uint32_t>(card.mode.hdisplay) ||
      attributes.height != static_cast<std::uint32_t>(card.mode.vdisplay) || attributes.planes.empty() ||
      attributes.planes.size() > 4) return false;
  // Nothing new to flip; composition keeps frame callbacks paced by vblank.
  if (card.direct_front && card.direct_front->buffer_id == candidate->buffer_id) return false;
  // Alpha is ignored on the primary plane, so opaque ARGB content scans out as XRGB.
  std::uint32_t format = attributes.format;
  if (format == DRM_FORMAT_ARGB8888) format = DRM_FORMAT_XRGB8888;
  else if (format == DRM_FORMAT_ABGR8888) format = DRM_FORMAT_XBGR8888;
  else if (format == DRM_FORMAT_ARGB2101010) format = DRM_FORMAT_XRGB2101010;
  else if (format == DRM_FORMAT_ABGR2101010) format = DRM_FORMAT_XBGR2101010;
  if (!input_target_->hold_buffer(candidate->buffer_id)) return false;
  auto& device = *card.device;
  // GBM shares RADV's refcounted GEM handles; closing them by hand loses the device.
  gbm_import_fd_modifier_data import{};
  import.width = attributes.width;
  import.height = attributes.height;
  import.format = format;
  import.num_fds = static_cast<std::uint32_t>(attributes.planes.size());
  import.modifier = attributes.planes.front().modifier;
  for (std::size_t index = 0; index < attributes.planes.size(); ++index) {
    import.fds[index] = attributes.planes[index].fd;
    import.strides[index] = static_cast<int>(attributes.planes[index].stride);
    import.offsets[index] = static_cast<int>(attributes.planes[index].offset);
  }
  gbm_bo* bo = gbm_bo_import(device.gbm, GBM_BO_IMPORT_FD_MODIFIER, &import, GBM_BO_USE_SCANOUT);
  const std::uint32_t framebuffer = bo == nullptr ? 0 : add_framebuffer(device, bo);
  // The framebuffer holds its own reference to the buffer.
  if (bo != nullptr) gbm_bo_destroy(bo);
  const bool imported = framebuffer != 0;
  if (imported && commit_atomic_output(card, framebuffer, false)) {
    card.direct_pending = DrmOutput::DirectFrame{framebuffer, candidate->buffer_id};
    card.pending_buffer = -1;
    card.flip_pending = true;
    card.needs_repaint = false;
    card.capture_ready = false;
    return true;
  }
  card.direct_failed_buffer = candidate->buffer_id;
  release_direct_frame(card, framebuffer, candidate->buffer_id);
  return false;
}

bool RuntimeBackend::set_output_power(OutputId id, bool on) {
  for (auto& device : devices_) for (auto& output : device->outputs) {
    if (output->output.id != id) continue;
    if (output->powered_off == !on) return true;
    output->powered_off = !on;
    output->needs_modeset = true;
    if (on) {
      repaint(*output);
      return true;
    }
    // A pending flip powers the output off once it lands.
    if (output->flip_pending || !active_) return true;
    if (disable_atomic_output(*output)) return true;
    output->powered_off = false;
    return false;
  }
  return false;
}

void RuntimeBackend::discover_drm_devices() {
  udev_enumerate* enumeration = udev_enumerate_new(udev_);
  if (enumeration == nullptr) {
    return;
  }
  udev_enumerate_add_match_subsystem(enumeration, "drm");
  udev_enumerate_scan_devices(enumeration);
  udev_list_entry* entry = nullptr;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumeration)) {
    udev_device* device = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(entry));
    if (device != nullptr) {
      if (active_) {
        add_drm_device(udev_device_get_devnode(device));
      }
      udev_device_unref(device);
    }
  }
  udev_enumerate_unref(enumeration);
}

void RuntimeBackend::probe_outputs() {
  discover_drm_devices();
  for (auto& drm_device : devices_) rescan_drm_device(*drm_device);
  publish_outputs();
  publish_dmabuf_importer();
  for (auto& drm_device : devices_) for (auto& output : drm_device->outputs) repaint(*output);
}

// Retries with backoff while the seat is active but no output has appeared yet.
void RuntimeBackend::schedule_output_probe() {
  const bool lit = std::any_of(devices_.begin(), devices_.end(), [](const auto& device) { return !device->outputs.empty(); });
  if (lit || !active_ || probe_timer_ >= 0 || probe_attempts_ >= 20) {
    if (lit) probe_attempts_ = 0;
    return;
  }
  const auto delay = std::min<std::uint32_t>(2000U, 100U << std::min<std::uint32_t>(probe_attempts_, 4U));
  ++probe_attempts_;
  probe_timer_ = event_loop_->add_timer(delay, [this] {
    probe_timer_ = -1;
    if (!active_) return;
    probe_outputs();
    schedule_output_probe();
  });
}

void RuntimeBackend::add_drm_device(const char* path) {
  if (!is_drm_card(path) || std::any_of(devices_.begin(), devices_.end(), [path](const auto& device) { return device->path == path; })) {
    return;
  }
  if (!drm_device_allowed(path)) {
    std::fprintf(stderr, "zwwm: DRM %s: skipped, not listed in ZWWM_DRM_DEVICES\n", path);
    return;
  }
  auto device = std::make_unique<DrmDevice>();
  device->backend = this;
  device->path = path;
  device->fd = open_device(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (device->fd < 0) {
    std::fprintf(stderr, "zwwm: DRM %s: libseat could not open card\n", path);
    return;
  }
  device->seat_device_id = restricted_devices_.back().second;
  if (drmSetClientCap(device->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
      drmSetClientCap(device->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    std::fprintf(stderr, "zwwm: DRM %s: atomic KMS is unavailable: %s\n", path, std::strerror(errno));
    close_drm_device(*device);
    return;
  }
  device->gbm = gbm_create_device(device->fd);
  if (device->gbm == nullptr) {
    std::fprintf(stderr, "zwwm: DRM %s: gbm_create_device failed\n", path);
    close_drm_device(*device);
    return;
  }
  struct stat node {};
  std::string vulkan_error;
  if (fstat(device->fd, &node) == 0)
    device->vulkan = vlk::Device::create({.drm_device = node.st_rdev, .require_dmabuf = true}, &vulkan_error);
  if (device->vulkan == nullptr) {
    std::fprintf(stderr, "zwwm: DRM %s: Vulkan initialization failed: %s\n", path,
                 vulkan_error.empty() ? "could not stat the DRM node" : vulkan_error.c_str());
    close_drm_device(*device);
    return;
  }
  const bool wants_ten_bit = config_->output.bit_depth == 10 ||
      std::any_of(config_->outputs.begin(), config_->outputs.end(), [](const auto& configured) {
        return configured.second.bit_depth == 10;
      });
  if (wants_ten_bit && !device->vulkan->render_modifiers(DRM_FORMAT_XRGB2101010).empty()) {
    device->scanout_format = DRM_FORMAT_XRGB2101010;
    device->scanout_bit_depth = 10;
  } else if (!device->vulkan->render_modifiers(DRM_FORMAT_XRGB8888).empty()) {
    device->scanout_format = DRM_FORMAT_XRGB8888;
    device->scanout_bit_depth = 8;
  } else {
    std::fprintf(stderr, "zwwm: DRM %s: %s cannot render to any scanout format\n", path,
                 device->vulkan->name().c_str());
    close_drm_device(*device);
    return;
  }
  device->renderer = std::make_unique<renderer::VulkanRenderer>();
  if (!device->renderer->initialize(device->vulkan)) {
    std::fprintf(stderr, "zwwm: DRM %s: Vulkan renderer initialization failed: %s\n", path,
                 device->renderer->last_error().c_str());
    close_drm_device(*device);
    return;
  }
  {
    renderer::ShaderSources sources;
    std::string shader_error;
    if (load_shader_sources(*config_, &sources, &shader_error) && device->renderer->prepare_shaders(sources))
      device->renderer->commit_shaders();
    else
      last_error_ = shader_error.empty() ? device->renderer->last_error() : shader_error;
  }
  try {
    auto* monitored = device.get();
    device->source = event_loop_->add_fd(device->fd, EPOLLIN, [monitored](int fd, int mask) {
      on_drm_fd(fd, static_cast<std::uint32_t>(mask), monitored);
      return true;
    });
  } catch (const std::exception&) {
    std::fprintf(stderr, "zwwm: DRM %s: could not monitor DRM events\n", path);
    close_drm_device(*device);
    return;
  }
  devices_.push_back(std::move(device));
  rescan_drm_device(*devices_.back());
  std::fprintf(stderr, "zwwm: activated DRM device %s with %zu output(s), %u-bit using Vulkan on %s\n",
               path, devices_.back()->outputs.size(), devices_.back()->scanout_bit_depth,
               devices_.back()->vulkan->name().c_str());
  publish_outputs();
  publish_dmabuf_importer();
  for (auto& output : devices_.back()->outputs) repaint(*output);
}

bool RuntimeBackend::initialize_atomic_output(DrmOutput& output,
                                             const std::vector<std::uint32_t>& reserved_planes) {
  auto& device = *output.device;
  auto& p = output.atomic;
  p.connector_crtc = kms_property(device.fd, output.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID").id;
  p.crtc_mode = kms_property(device.fd, output.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID").id;
  p.crtc_active = kms_property(device.fd, output.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE").id;
  const auto bpc = kms_property(device.fd, output.connector_id, DRM_MODE_OBJECT_CONNECTOR, "max bpc");
  if (bpc.id != 0 && bpc.maximum >= bpc.minimum && bpc.maximum != 0) {
    p.max_bpc = bpc.id;
    output.max_bpc_value = std::clamp<std::uint64_t>(device.scanout_bit_depth, bpc.minimum, bpc.maximum);
  }
  auto* planes = drmModeGetPlaneResources(device.fd);
  if (planes != nullptr) {
    for (std::uint32_t index = 0; index < planes->count_planes; ++index) {
      const auto id = planes->planes[index];
      if (std::find(reserved_planes.begin(), reserved_planes.end(), id) != reserved_planes.end()) continue;
      const auto busy = [id, &output](const auto& item) {
        return item != nullptr && item->plane_id == id &&
               (item->flip_pending || item->crtc_id != output.crtc_id);
      };
      if (std::any_of(device.outputs.begin(), device.outputs.end(), busy) ||
          std::any_of(device.retired_outputs.begin(), device.retired_outputs.end(), busy)) continue;
      auto* plane = drmModeGetPlane(device.fd, id);
      if (plane == nullptr) continue;
      const bool compatible = output.crtc_index < 32 &&
          (plane->possible_crtcs & (1U << output.crtc_index)) != 0 &&
          std::find(plane->formats, plane->formats + plane->count_formats, device.scanout_format) !=
              plane->formats + plane->count_formats;
      drmModeFreePlane(plane);
      const auto type = kms_property(device.fd, id, DRM_MODE_OBJECT_PLANE, "type");
      if (!compatible || type.id == 0 || type.value != DRM_PLANE_TYPE_PRIMARY) continue;
      output.plane_id = id;
      break;
    }
    drmModeFreePlaneResources(planes);
  }
  const auto plane_property = [&](const char* name) {
    return kms_property(device.fd, output.plane_id, DRM_MODE_OBJECT_PLANE, name).id;
  };
  if (output.plane_id != 0) {
    p.plane_fb = plane_property("FB_ID"); p.plane_crtc = plane_property("CRTC_ID");
    p.src_x = plane_property("SRC_X"); p.src_y = plane_property("SRC_Y");
    p.src_w = plane_property("SRC_W"); p.src_h = plane_property("SRC_H");
    p.crtc_x = plane_property("CRTC_X"); p.crtc_y = plane_property("CRTC_Y");
    p.crtc_w = plane_property("CRTC_W"); p.crtc_h = plane_property("CRTC_H");
  }
  if (p.connector_crtc == 0 || p.crtc_mode == 0 || p.crtc_active == 0 ||
      p.plane_fb == 0 || p.plane_crtc == 0 || p.src_x == 0 || p.src_y == 0 ||
      p.src_w == 0 || p.src_h == 0 || p.crtc_x == 0 || p.crtc_y == 0 ||
      p.crtc_w == 0 || p.crtc_h == 0) {
    std::fprintf(stderr, "zwwm: DRM %s: no compatible atomic primary plane/properties for connector %u\n",
                 device.path.c_str(), output.connector_id);
    return false;
  }
  if (drmModeCreatePropertyBlob(device.fd, &output.mode, sizeof(output.mode), &output.mode_blob) != 0) {
    std::fprintf(stderr, "zwwm: DRM %s: could not create mode blob: %s\n",
                 device.path.c_str(), std::strerror(errno));
    return false;
  }
  return true;
}

std::uint32_t RuntimeBackend::add_framebuffer(DrmDevice& device, gbm_bo* bo) {
  std::uint32_t fb = 0;
  std::uint32_t handles[4]{}, strides[4]{}, offsets[4]{};
  std::uint64_t modifiers[4]{};
  const auto modifier = gbm_bo_get_modifier(bo);
  const int planes = gbm_bo_get_plane_count(bo);
  if (planes < 1 || planes > 4) return 0;
  for (int index = 0; index < planes; ++index) {
    handles[index] = gbm_bo_get_handle_for_plane(bo, index).u32;
    strides[index] = gbm_bo_get_stride_for_plane(bo, index);
    offsets[index] = gbm_bo_get_offset(bo, index);
    modifiers[index] = modifier;
  }
  std::uint64_t modifier_support = 0;
  (void)drmGetCap(device.fd, DRM_CAP_ADDFB2_MODIFIERS, &modifier_support);
  const bool explicit_modifier = modifier != DRM_FORMAT_MOD_INVALID && modifier_support != 0;
  int added = explicit_modifier ?
      drmModeAddFB2WithModifiers(device.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                                gbm_bo_get_format(bo), handles, strides, offsets, modifiers,
                                &fb, DRM_MODE_FB_MODIFIERS) :
      drmModeAddFB2(device.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                    handles, strides, offsets, &fb, 0);
  if (added != 0 && explicit_modifier && modifier == DRM_FORMAT_MOD_LINEAR)
    added = drmModeAddFB2(device.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
                         handles, strides, offsets, &fb, 0);
  return added == 0 ? fb : 0;
}

bool RuntimeBackend::create_scanout(DrmOutput& output) {
  auto& device = *output.device;
  if (device.renderer == nullptr || device.vulkan == nullptr) return false;
  const auto width = static_cast<std::uint32_t>(output.mode.hdisplay);
  const auto height = static_cast<std::uint32_t>(output.mode.vdisplay);
  std::vector<std::uint64_t> modifiers = device.vulkan->render_modifiers(device.scanout_format);
  const auto in_formats = kms_property(device.fd, output.plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS");
  if (in_formats.id != 0 && in_formats.value != 0) {
    if (auto* blob = drmModeGetPropertyBlob(device.fd, static_cast<std::uint32_t>(in_formats.value))) {
      std::vector<std::uint64_t> scanout_modifiers;
      drmModeFormatModifierIterator iterator{};
      while (drmModeFormatModifierBlobIterNext(blob, &iterator))
        if (iterator.fmt == device.scanout_format) scanout_modifiers.push_back(iterator.mod);
      drmModeFreePropertyBlob(blob);
      std::erase_if(modifiers, [&](std::uint64_t modifier) {
        return std::find(scanout_modifiers.begin(), scanout_modifiers.end(), modifier) == scanout_modifiers.end();
      });
    }
  }
  const auto renderable = device.vulkan->render_modifiers(device.scanout_format);
  const bool linear = std::find(renderable.begin(), renderable.end(), DRM_FORMAT_MOD_LINEAR) != renderable.end();
  constexpr std::size_t kScanoutBuffers = 3;
  for (std::size_t index = 0; index < kScanoutBuffers; ++index) {
    DrmOutput::ScanoutBuffer buffer;
    if (!modifiers.empty())
      buffer.bo = gbm_bo_create_with_modifiers2(device.gbm, width, height, device.scanout_format, modifiers.data(),
                                                static_cast<unsigned>(modifiers.size()),
                                                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (buffer.bo == nullptr && linear)
      buffer.bo = gbm_bo_create(device.gbm, width, height, device.scanout_format,
                                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (buffer.bo == nullptr) {
      std::fprintf(stderr, "zwwm: DRM %s: could not allocate a %ux%u scanout buffer: %s\n", device.path.c_str(),
                   width, height, std::strerror(errno));
      return false;
    }
    renderer::DmabufAttributes attributes{width, height, device.scanout_format, {}};
    std::vector<UniqueFd> fds;
    const auto modifier = gbm_bo_get_modifier(buffer.bo);
    for (int plane = 0; plane < gbm_bo_get_plane_count(buffer.bo); ++plane) {
      fds.emplace_back(gbm_bo_get_fd_for_plane(buffer.bo, plane));
      attributes.planes.push_back({fds.back().get(), gbm_bo_get_stride_for_plane(buffer.bo, plane),
                                   gbm_bo_get_offset(buffer.bo, plane), modifier});
    }
    std::string error;
    buffer.image = vlk::Image::import_dmabuf(device.vulkan, attributes, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &error);
    buffer.framebuffer = buffer.image == nullptr ? 0 : add_framebuffer(device, buffer.bo);
    if (buffer.image == nullptr || buffer.framebuffer == 0) {
      std::fprintf(stderr, "zwwm: DRM %s: scanout buffer setup failed: %s\n", device.path.c_str(),
                   buffer.image == nullptr ? error.c_str() : std::strerror(errno));
      buffer.image.reset();
      gbm_bo_destroy(buffer.bo);
      return false;
    }
    output.scanout.push_back(std::move(buffer));
  }
  return true;
}

bool RuntimeBackend::commit_atomic_output(DrmOutput& output, std::uint32_t framebuffer, bool modeset) {
  auto& device = *output.device;
  const auto& p = output.atomic;
  auto* request = drmModeAtomicAlloc();
  if (request == nullptr) return false;
  bool valid = true;
  const auto add = [&](std::uint32_t object, std::uint32_t property, std::uint64_t value) {
    if (property == 0 || drmModeAtomicAddProperty(request, object, property, value) < 0) valid = false;
  };
  add(output.plane_id, p.plane_fb, framebuffer);
  add(output.plane_id, p.plane_crtc, output.crtc_id);
  if (modeset) {
    add(output.connector_id, p.connector_crtc, output.crtc_id);
    if (p.max_bpc != 0) add(output.connector_id, p.max_bpc, output.max_bpc_value);
    add(output.crtc_id, p.crtc_mode, output.mode_blob);
    add(output.crtc_id, p.crtc_active, 1);
    add(output.plane_id, p.src_x, 0); add(output.plane_id, p.src_y, 0);
    add(output.plane_id, p.src_w, static_cast<std::uint64_t>(output.mode.hdisplay) << 16U);
    add(output.plane_id, p.src_h, static_cast<std::uint64_t>(output.mode.vdisplay) << 16U);
    add(output.plane_id, p.crtc_x, 0); add(output.plane_id, p.crtc_y, 0);
    add(output.plane_id, p.crtc_w, output.mode.hdisplay);
    add(output.plane_id, p.crtc_h, output.mode.vdisplay);
    const auto rotation = kms_property(device.fd, output.plane_id, DRM_MODE_OBJECT_PLANE, "rotation");
    if (rotation.id != 0) add(output.plane_id, rotation.id, DRM_MODE_ROTATE_0);
    // Clear planes a previous seat owner left on this CRTC; zwwm composes into the primary.
    auto* planes = drmModeGetPlaneResources(device.fd);
    if (planes != nullptr) {
      for (std::uint32_t index = 0; index < planes->count_planes; ++index) {
        const auto id = planes->planes[index];
        if (id == output.plane_id) continue;
        const auto crtc = kms_property(device.fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        if (crtc.value != output.crtc_id) continue;
        add(id, crtc.id, 0);
        add(id, kms_property(device.fd, id, DRM_MODE_OBJECT_PLANE, "FB_ID").id, 0);
      }
      drmModeFreePlaneResources(planes);
    }
  }
  const std::uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET :
      DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
  bool success = valid;
  if (success && modeset)
    success = drmModeAtomicCommit(device.fd, request, DRM_MODE_ATOMIC_TEST_ONLY | flags, nullptr) == 0;
  if (success) success = drmModeAtomicCommit(device.fd, request, flags, modeset ? nullptr : &output) == 0;
  const int saved_errno = errno;
  drmModeAtomicFree(request);
  if (!success) {
    last_error_ = std::string(modeset ? "atomic modeset failed: " : "atomic page flip failed: ") +
                  std::strerror(saved_errno);
    std::fprintf(stderr, "zwwm: %s (%s connector %u)\n", last_error_.c_str(),
                 device.path.c_str(), output.connector_id);
  }
  return success;
}

bool RuntimeBackend::disable_atomic_output(DrmOutput& output) {
  auto& device = *output.device;
  const auto& p = output.atomic;
  auto* request = drmModeAtomicAlloc();
  if (request == nullptr) return false;
  bool valid = true;
  const auto add = [&](std::uint32_t object, std::uint32_t property) {
    if (property == 0 || drmModeAtomicAddProperty(request, object, property, 0) < 0) valid = false;
  };
  add(output.plane_id, p.plane_fb); add(output.plane_id, p.plane_crtc);
  add(output.connector_id, p.connector_crtc);
  add(output.crtc_id, p.crtc_active); add(output.crtc_id, p.crtc_mode);
  const bool success = valid && drmModeAtomicCommit(device.fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
  drmModeAtomicFree(request);
  return success;
}

void RuntimeBackend::rescan_drm_device(DrmDevice& device) {
  struct Candidate {
    std::string key;
    std::string name;
    std::uint32_t connector_id = 0;
    std::uint32_t possible_crtcs = 0;
    std::uint32_t current_crtc = 0;
    drmModeModeInfo mode{};
    std::vector<OutputInfo::Mode> modes;
    std::uint32_t crtc_index = std::numeric_limits<std::uint32_t>::max();
  };
  drmModeRes* resources = drmModeGetResources(device.fd);
  if (resources == nullptr) return;
  std::uint32_t primary_crtcs = 0;
  auto* primary_planes = drmModeGetPlaneResources(device.fd);
  if (primary_planes != nullptr) {
    for (std::uint32_t index = 0; index < primary_planes->count_planes; ++index) {
      const auto id = primary_planes->planes[index];
      const auto type = kms_property(device.fd, id, DRM_MODE_OBJECT_PLANE, "type");
      if (type.id == 0 || type.value != DRM_PLANE_TYPE_PRIMARY) continue;
      auto* plane = drmModeGetPlane(device.fd, id);
      if (plane == nullptr) continue;
      if (std::find(plane->formats, plane->formats + plane->count_formats, device.scanout_format) !=
          plane->formats + plane->count_formats) primary_crtcs |= plane->possible_crtcs;
      drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(primary_planes);
  }
  const auto device_name = device.path.substr(device.path.rfind('/') == std::string::npos ? 0 : device.path.rfind('/') + 1);
  std::vector<Candidate> candidates;
  for (int connector_index = 0; connector_index < resources->count_connectors; ++connector_index) {
    drmModeConnector* connector = drmModeGetConnector(device.fd, resources->connectors[connector_index]);
    if (connector == nullptr) continue;
    // Built-in panels often report an unknown status until the kernel's next hotplug probe.
    const bool usable = connector->connection != DRM_MODE_DISCONNECTED && connector->count_modes > 0 &&
                        connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK;
    if (!usable) { drmModeFreeConnector(connector); continue; }
    const char* type = drmModeGetConnectorTypeName(connector->connector_type);
    Candidate candidate;
    candidate.name = device_name + "-" + (type == nullptr ? "UNKNOWN" : type) + "-" +
                     std::to_string(connector->connector_type_id);
    candidate.key = device.path + ":" + candidate.name;
    candidate.connector_id = connector->connector_id;
    candidate.modes.reserve(static_cast<std::size_t>(connector->count_modes));
    for (int index = 0; index < connector->count_modes; ++index) {
      const auto& available = connector->modes[index];
      candidate.modes.push_back({static_cast<std::uint32_t>(available.hdisplay),
                                 static_cast<std::uint32_t>(available.vdisplay),
                                 mode_refresh_millihz(available),
                                 (available.type & DRM_MODE_TYPE_PREFERRED) != 0});
    }
    const drmModeModeInfo* mode = &connector->modes[0];
    for (int index = 0; index < connector->count_modes; ++index)
      if ((connector->modes[index].type & DRM_MODE_TYPE_PREFERRED) != 0) { mode = &connector->modes[index]; break; }
    const auto& configured = config_->output_for(candidate.name);
    if (!configured.mode.preferred) {
      const auto requested = configured.mode;
      const auto exact = std::find_if(connector->modes, connector->modes + connector->count_modes,
          [requested](const drmModeModeInfo& item) {
            const auto refresh = mode_refresh_millihz(item);
            return item.hdisplay == requested.width && item.vdisplay == requested.height &&
                   refresh + 500U >= requested.refresh_millihz && refresh <= requested.refresh_millihz + 500U;
          });
      if (exact != connector->modes + connector->count_modes) mode = exact;
    }
    candidate.mode = *mode;
    for (int index = 0; index < connector->count_encoders; ++index) {
      drmModeEncoder* encoder = drmModeGetEncoder(device.fd, connector->encoders[index]);
      if (encoder == nullptr) continue;
      candidate.possible_crtcs |= encoder->possible_crtcs;
      if (connector->encoder_id == encoder->encoder_id) candidate.current_crtc = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
    }
    candidate.possible_crtcs &= primary_crtcs;
    if (candidate.possible_crtcs != 0) candidates.push_back(std::move(candidate));
    drmModeFreeConnector(connector);
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    return left.key < right.key;
  });
  const std::size_t crtc_count = static_cast<std::size_t>(resources->count_crtcs);
  std::vector<int> crtc_owner(crtc_count, -1);
  std::vector<bool> crtc_locked(crtc_count, false);
  for (const auto& output : device.retired_outputs) if (output->flip_pending && output->crtc_index < crtc_count)
    crtc_locked[output->crtc_index] = true;
  for (const auto& output : device.outputs) if (output->flip_pending && output->crtc_index < crtc_count) {
    const auto candidate = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& item) {
      return item.key == output->connector_key && item.connector_id == output->connector_id &&
             same_drm_mode(item.mode, output->mode);
    });
    if (candidate == candidates.end()) crtc_locked[output->crtc_index] = true;
  }
  // Preserve valid CRTC assignments during rescans.
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    auto& candidate = candidates[index];
    const auto existing = std::find_if(device.outputs.begin(), device.outputs.end(), [&](const auto& output) {
      return output->connector_key == candidate.key && output->connector_id == candidate.connector_id &&
             same_drm_mode(output->mode, candidate.mode);
    });
    if (existing == device.outputs.end() || (*existing)->crtc_index >= crtc_count || (*existing)->crtc_index >= 32 ||
        (candidate.possible_crtcs & (1U << (*existing)->crtc_index)) == 0 || crtc_locked[(*existing)->crtc_index] ||
        crtc_owner[(*existing)->crtc_index] != -1) continue;
    candidate.crtc_index = (*existing)->crtc_index;
    crtc_owner[candidate.crtc_index] = static_cast<int>(index);
    crtc_locked[candidate.crtc_index] = true;
  }
  std::function<bool(std::size_t, std::vector<bool>&)> assign_crtc;
  assign_crtc = [&](std::size_t candidate_index, std::vector<bool>& visited) {
    auto& candidate = candidates[candidate_index];
    std::vector<std::uint32_t> order;
    for (int index = 0; index < resources->count_crtcs && index < 32; ++index)
      if (resources->crtcs[index] == candidate.current_crtc) order.push_back(static_cast<std::uint32_t>(index));
    for (int index = 0; index < resources->count_crtcs && index < 32; ++index)
      if (order.empty() || order.front() != static_cast<std::uint32_t>(index)) order.push_back(static_cast<std::uint32_t>(index));
    for (const auto crtc : order) {
      if ((candidate.possible_crtcs & (1U << crtc)) == 0 || visited[crtc] || crtc_locked[crtc]) continue;
      visited[crtc] = true;
      const int owner = crtc_owner[crtc];
      if (owner >= 0 && !assign_crtc(static_cast<std::size_t>(owner), visited)) continue;
      crtc_owner[crtc] = static_cast<int>(candidate_index);
      candidate.crtc_index = crtc;
      return true;
    }
    return false;
  };
  for (std::size_t index = 0; index < candidates.size(); ++index) if (candidates[index].crtc_index == std::numeric_limits<std::uint32_t>::max()) {
    std::vector<bool> visited(crtc_count, false);
    (void)assign_crtc(index, visited);
  }
  std::vector<std::unique_ptr<DrmOutput>> next;
  std::vector<std::uint32_t> reserved_planes;
  for (auto& candidate : candidates) {
    if (candidate.crtc_index == std::numeric_limits<std::uint32_t>::max()) continue;
    const auto existing = std::find_if(device.outputs.begin(), device.outputs.end(), [&](const auto& output) {
      return output != nullptr && output->connector_key == candidate.key && output->connector_id == candidate.connector_id &&
             output->crtc_index == candidate.crtc_index && same_drm_mode(output->mode, candidate.mode);
    });
    if (existing != device.outputs.end()) {
      (*existing)->output.modes = candidate.modes;
      (*existing)->output.bit_depth = device.scanout_bit_depth;
      reserved_planes.push_back((*existing)->plane_id);
      next.push_back(std::move(*existing));
      continue;
    }
    auto output = std::make_unique<DrmOutput>(config_->animations);
    output->device = &device;
    output->connector_key = candidate.key;
    output->connector_id = candidate.connector_id;
    output->crtc_index = candidate.crtc_index;
    output->crtc_id = resources->crtcs[candidate.crtc_index];
    output->mode = candidate.mode;
    output->scanout_bit_depth = device.scanout_bit_depth;
    if (!initialize_atomic_output(*output, reserved_planes)) continue;
    if (!create_scanout(*output)) { close_drm_output(*output); continue; }
    const auto& configured = config_->output_for(candidate.name);
    const auto logical = configured.logical_size({static_cast<std::uint32_t>(candidate.mode.hdisplay),
                                                   static_cast<std::uint32_t>(candidate.mode.vdisplay)});
    output->output = {.id = stable_output_id(device.path, candidate.key), .connector = candidate.name,
        .logical_width = logical.width, .logical_height = logical.height,
        .physical_width = static_cast<std::uint32_t>(candidate.mode.hdisplay),
        .physical_height = static_cast<std::uint32_t>(candidate.mode.vdisplay),
        .refresh_millihz = mode_refresh_millihz(candidate.mode),
        .scale_per_mille = configured.scale_per_mille, .transform = configured.transform,
        .bit_depth = device.scanout_bit_depth, .modes = std::move(candidate.modes)};
    reserved_planes.push_back(output->plane_id);
    next.push_back(std::move(output));
  }
  if (next.empty() && !device.outputs.empty()) device.dmabuf_textures.clear();
  for (auto& output : device.outputs) if (output != nullptr) {
    output->retired = true;
    output->output.enabled = false;
    if (output->flip_pending) device.retired_outputs.push_back(std::move(output));
    else close_drm_output(*output);
  }
  device.outputs = std::move(next);
  drmModeFreeResources(resources);
  XcursorImages* cursor = XcursorLibraryLoadImages(cursor_shape_.c_str(), cursor_theme_.c_str(), cursor_size_);
  for (auto& output : device.outputs) if (output->cursor_texture == nullptr && device.renderer != nullptr &&
      cursor != nullptr && cursor->nimage > 0 && cursor->images[0] != nullptr) {
    const auto* image = cursor->images[0];
    output->cursor_texture = device.renderer->upload(nullptr, image->pixels, image->width, image->height, image->width);
    output->cursor_width = static_cast<std::int32_t>(image->width);
    output->cursor_height = static_cast<std::int32_t>(image->height);
    output->cursor_hotspot_x = static_cast<std::int32_t>(image->xhot);
    output->cursor_hotspot_y = static_cast<std::int32_t>(image->yhot);
  }
  if (cursor != nullptr) XcursorImagesDestroy(cursor);
}

void RuntimeBackend::remove_drm_device(const char* path) {
  const auto device = std::find_if(devices_.begin(), devices_.end(), [path](const auto& item) { return item->path == path; });
  if (device != devices_.end()) {
    close_drm_device(**device);
    devices_.erase(device);
    publish_outputs();
    publish_dmabuf_importer();
  }
}

void RuntimeBackend::suspend_drm_devices() {
  for (auto& device : devices_) {
    if (device->source >= 0) {
      event_loop_->remove(device->source);
      device->source = -1;
    }
  }
}

void RuntimeBackend::resume_drm_devices() {
  for (auto& device : devices_) {
    if (device->source < 0) {
      auto* resumed = device.get();
      try {
        device->source = event_loop_->add_fd(device->fd, EPOLLIN, [resumed](int fd, int mask) {
          on_drm_fd(fd, static_cast<std::uint32_t>(mask), resumed);
          return true;
        });
      } catch (const std::exception&) {
        device->source = -1;
      }
    }
    for (auto& output : device->outputs) {
      output->needs_modeset = true;
      repaint(*output);
    }
  }
}

void RuntimeBackend::close_drm_output(DrmOutput& output) {
  auto& device = *output.device;
  if (active_ && device.fd >= 0 && (output.front_buffer >= 0 || output.pending_buffer >= 0)) {
    if (!disable_atomic_output(output))
      std::fprintf(stderr, "zwwm: DRM %s: could not disable atomic output %u: %s\n",
                   device.path.c_str(), output.connector_id, std::strerror(errno));
  }
  for (auto& buffer : output.scanout) {
    if (buffer.framebuffer != 0) drmModeRmFB(device.fd, buffer.framebuffer);
    if (buffer.bo != nullptr) gbm_bo_destroy(buffer.bo);
  }
  output.scanout.clear();
  output.front_buffer = output.pending_buffer = -1;
  if (output.direct_front) release_direct_frame(output, output.direct_front->framebuffer, output.direct_front->buffer_id);
  if (output.direct_pending) release_direct_frame(output, output.direct_pending->framebuffer, output.direct_pending->buffer_id);
  output.direct_front.reset();
  output.direct_pending.reset();
  output.shm_textures.clear();
  output.cursor_texture.reset();
  output.capture_texture.reset();
  output.capture_cursor_texture.reset();
  if (output.mode_blob != 0) drmModeDestroyPropertyBlob(device.fd, output.mode_blob);
  output.mode_blob = 0;
}

void RuntimeBackend::close_drm_device(DrmDevice& device) {
  if (device.source >= 0) {
    event_loop_->remove(device.source);
    device.source = -1;
  }
  if (device.vulkan != nullptr) device.vulkan->wait_idle();
  device.dmabuf_textures.clear();
  device.error_popup_texture.reset();
  for (auto& output : device.outputs) close_drm_output(*output);
  for (auto& output : device.retired_outputs) close_drm_output(*output);
  device.outputs.clear();
  device.retired_outputs.clear();
  if (device.renderer != nullptr) device.renderer->shutdown();
  device.renderer.reset();
  if (device.vulkan != nullptr) device.vulkan->wait_idle();
  device.vulkan.reset();
  if (device.gbm != nullptr) {
    gbm_device_destroy(device.gbm);
    device.gbm = nullptr;
  }
  if (device.fd >= 0) {
    close_device(device.fd);
    device.fd = -1;
  }
}

int RuntimeBackend::open_device(const char* path, int flags) {
  int fd = -1;
  const int device_id = libseat_open_device(seat_, path, &fd);
  if (device_id < 0) {
    std::fprintf(stderr, "zwwm: libseat could not open %s: %s\n", path, std::strerror(-device_id));
    return -1;
  }
  if ((flags & O_NONBLOCK) != 0) {
    const int current_flags = fcntl(fd, F_GETFL);
    if (current_flags >= 0) {
      fcntl(fd, F_SETFL, current_flags | O_NONBLOCK);
    }
  }
  restricted_devices_.push_back({fd, device_id});
  return fd;
}

void RuntimeBackend::close_device(int fd) {
  const auto device = std::find_if(restricted_devices_.begin(), restricted_devices_.end(), [fd](const auto& item) { return item.first == fd; });
  if (device != restricted_devices_.end()) {
    libseat_close_device(seat_, device->second);
    restricted_devices_.erase(device);
  }
}

}  // namespace zwwm
