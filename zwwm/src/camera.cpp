#include "zwwm/camera.hpp"

#include <algorithm>
#include <cmath>

namespace zwwm {
namespace {

constexpr double kZoomFriction = 12.5;
constexpr double kVelocityStackLimit = 6.0;
constexpr double kVelocityEpsilon = 0.02;
constexpr double kPanDragRate = 12.0;
constexpr double kPanFriction = 12.0;
constexpr double kPanEpsilon = 0.05;
constexpr double kPanStopSpeed = 4.0;
constexpr double kPanMaximumSpeed = 3500.0;

}  // namespace

Camera::Camera(const CameraConfig& config) {
  set_config(config);
}

void Camera::set_config(const CameraConfig& config) {
  config_.min_zoom = std::max(0.01, config.min_zoom);
  config_.max_zoom = std::max(config_.min_zoom, config.max_zoom);
  config_.zoom_step = std::max(1.001, config.zoom_step);
}

bool Camera::zoom_by_steps(layout::CanvasViewport& viewport, std::int32_t width,
                           std::int32_t height, int steps, std::uint64_t now_ms) {
  if (steps == 0) return false;

  if (pan_coasting_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
    pan_velocity_x_ = pan_velocity_y_ = 0.0;
    pan_coasting_ = false;
  }

  const double impulse = kZoomFriction * std::log(config_.zoom_step);
  const double limit = impulse * kVelocityStackLimit;
  if (!animating_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
  }
  zoom_velocity_ = std::clamp(zoom_velocity_ + steps * impulse, -limit, limit);
  animating_ = true;
  if (last_update_ms_ == 0) last_update_ms_ = now_ms > 16 ? now_ms - 16 : 0;
  return tick(viewport, width, height, now_ms);
}

bool Camera::pan_by(layout::CanvasViewport& viewport, std::int32_t width,
                    std::int32_t height, double dx, double dy, std::uint64_t now_ms) {
  if (dx == 0.0 && dy == 0.0) return false;
  // Grabbing a coasting canvas catches it where it is.
  if (pan_coasting_) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
    pan_velocity_x_ = pan_velocity_y_ = 0.0;
    pan_coasting_ = false;
  }
  const bool starting = !animating_;
  if (starting) {
    target_center_x_ = viewport.x + width / (2.0 * viewport.scale);
    target_center_y_ = viewport.y + height / (2.0 * viewport.scale);
  }
  target_center_x_ -= dx / viewport.scale;
  target_center_y_ -= dy / viewport.scale;
  animating_ = true;
  if (!starting) return false;
  last_update_ms_ = now_ms > 16 ? now_ms - 16 : 0;
  return tick(viewport, width, height, now_ms);
}

bool Camera::finish_pan(layout::CanvasViewport& viewport, std::int32_t width,
                        std::int32_t height) {
  if (!animating_ || pan_coasting_) return animating_;
  // Continue with the drag's current velocity so release has no seam.
  const double center_x = viewport.x + width / (2.0 * viewport.scale);
  const double center_y = viewport.y + height / (2.0 * viewport.scale);
  pan_velocity_x_ = (target_center_x_ - center_x) * kPanDragRate;
  pan_velocity_y_ = (target_center_y_ - center_y) * kPanDragRate;
  const double speed = std::hypot(pan_velocity_x_, pan_velocity_y_) * viewport.scale;
  if (speed > kPanMaximumSpeed) {
    pan_velocity_x_ *= kPanMaximumSpeed / speed;
    pan_velocity_y_ *= kPanMaximumSpeed / speed;
  }
  pan_coasting_ = speed >= kPanStopSpeed;
  if (!pan_coasting_) pan_velocity_x_ = pan_velocity_y_ = 0.0;
  return animating_;
}

bool Camera::tick(layout::CanvasViewport& viewport, std::int32_t width,
                  std::int32_t height, std::uint64_t now_ms) {
  if (!animating_) return false;

  const double elapsed = now_ms >= last_update_ms_
      ? static_cast<double>(now_ms - last_update_ms_) / 1000.0
      : 1.0 / 60.0;
  const double dt = std::clamp(elapsed, 1.0 / 240.0, 1.0 / 20.0);
  last_update_ms_ = now_ms;

  const double old_scale = std::clamp(viewport.scale, config_.min_zoom, config_.max_zoom);
  const double requested_scale = old_scale * std::exp(zoom_velocity_ * dt);
  const double new_scale = std::clamp(requested_scale, config_.min_zoom, config_.max_zoom);
  const bool hit_bound = new_scale != requested_scale;
  const double center_x = viewport.x + static_cast<double>(width) / (2.0 * old_scale);
  const double center_y = viewport.y + static_cast<double>(height) / (2.0 * old_scale);
  double live_x = center_x, live_y = center_y;
  if (pan_coasting_) {
    // Exact decay integral, independent of frame rate.
    const double decay = std::exp(-kPanFriction * dt);
    live_x += pan_velocity_x_ * (1.0 - decay) / kPanFriction;
    live_y += pan_velocity_y_ * (1.0 - decay) / kPanFriction;
    pan_velocity_x_ *= decay;
    pan_velocity_y_ *= decay;
    if (std::hypot(pan_velocity_x_, pan_velocity_y_) * new_scale < kPanStopSpeed) {
      pan_velocity_x_ = pan_velocity_y_ = 0.0;
      pan_coasting_ = false;
    }
    target_center_x_ = live_x;
    target_center_y_ = live_y;
  } else {
    const double pan_alpha = 1.0 - std::exp(-kPanDragRate * dt);
    const double next_x = center_x + (target_center_x_ - center_x) * pan_alpha;
    const double next_y = center_y + (target_center_y_ - center_y) * pan_alpha;
    live_x = std::abs(target_center_x_ - next_x) < kPanEpsilon ? target_center_x_ : next_x;
    live_y = std::abs(target_center_y_ - next_y) < kPanEpsilon ? target_center_y_ : next_y;
  }
  const bool changed = new_scale != viewport.scale || live_x != center_x || live_y != center_y;
  viewport.scale = new_scale;
  viewport.x = live_x - static_cast<double>(width) / (2.0 * new_scale);
  viewport.y = live_y - static_cast<double>(height) / (2.0 * new_scale);

  zoom_velocity_ *= std::exp(-kZoomFriction * dt);
  if (hit_bound || std::abs(zoom_velocity_) <= kVelocityEpsilon) zoom_velocity_ = 0.0;
  if (zoom_velocity_ == 0.0 && !pan_coasting_ &&
      live_x == target_center_x_ && live_y == target_center_y_) {
    animating_ = false;
    last_update_ms_ = 0;
    pan_coasting_ = false;
  }
  return changed;
}

void Camera::stop() {
  zoom_velocity_ = 0.0;
  pan_velocity_x_ = pan_velocity_y_ = 0.0;
  last_update_ms_ = 0;
  pan_coasting_ = false;
  animating_ = false;
}

bool Camera::is_animating() const {
  return animating_;
}

}  // namespace zwwm
