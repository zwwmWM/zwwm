#include "zwwm/animation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace zwwm {
namespace {

bool same_rect(const renderer::Rect& left, const renderer::Rect& right) {
  return left.origin.x == right.origin.x && left.origin.y == right.origin.y &&
         left.size.width == right.size.width && left.size.height == right.size.height;
}

constexpr double kMicrosPerMilli = 1000.0;

bool duration_expired(std::uint64_t started_us, std::uint32_t duration_ms, std::uint64_t now_us) {
  return now_us >= started_us && now_us - started_us >= std::uint64_t{duration_ms} * 1000U;
}

std::int32_t rounded_coordinate(double value) {
  return static_cast<std::int32_t>(std::clamp(
      std::round(value), static_cast<double>(std::numeric_limits<std::int32_t>::min()),
      static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

std::uint32_t rounded_dimension(double value) {
  return static_cast<std::uint32_t>(std::clamp(
      std::round(value), 1.0, static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

renderer::FloatRect scaled(renderer::FloatRect bounds, double scale) {
  const double width = bounds.size.width * scale;
  const double height = bounds.size.height * scale;
  return {{bounds.origin.x + (bounds.size.width - width) * 0.5, bounds.origin.y + (bounds.size.height - height) * 0.5},
          {width, height}};
}

renderer::Rect rounded(const renderer::FloatRect& rect) {
  return {{rounded_coordinate(rect.origin.x), rounded_coordinate(rect.origin.y)},
          {rounded_dimension(rect.size.width), rounded_dimension(rect.size.height)}};
}

std::array<double, 4> coordinates(renderer::Rect rect) {
  return {static_cast<double>(rect.origin.x), static_cast<double>(rect.origin.y),
          static_cast<double>(rect.size.width), static_cast<double>(rect.size.height)};
}

std::array<double, 4> interpolate_values(const std::array<double, 4>& from,
                                         renderer::Rect to, double progress) {
  const auto value = [progress](double begin, double end) { return begin + (end - begin) * progress; };
  return {value(from[0], to.origin.x), value(from[1], to.origin.y),
          value(from[2], to.size.width), value(from[3], to.size.height)};
}

renderer::Rect interpolate(const std::array<double, 4>& from, renderer::Rect to, double progress) {
  const auto values = interpolate_values(from, to, progress);
  return {{rounded_coordinate(values[0]), rounded_coordinate(values[1])},
          {rounded_dimension(values[2]), rounded_dimension(values[3])}};
}

double tracking_progress(std::uint64_t started_us, std::uint64_t now_us) {
  constexpr double rate_per_second = 12.5;
  if (now_us <= started_us) return 0.0;
  const double elapsed = static_cast<double>(now_us - started_us) / 1000000.0;
  return 1.0 - std::exp(-rate_per_second * elapsed);
}

double cubic(double p1, double p2, double t) {
  const double inverse = 1.0 - t;
  return 3.0 * inverse * inverse * t * p1 + 3.0 * inverse * t * t * p2 + t * t * t;
}

double bezier_progress(double elapsed, const AnimationConfig& config) {
  const double x1 = config.bezier_x1_per_mille / 1000.0;
  const double y1 = config.bezier_y1_per_mille / 1000.0;
  const double x2 = config.bezier_x2_per_mille / 1000.0;
  const double y2 = config.bezier_y2_per_mille / 1000.0;
  double low = 0.0, high = 1.0;
  for (int iteration = 0; iteration < 16; ++iteration) {
    const double midpoint = (low + high) * 0.5;
    if (cubic(x1, x2, midpoint) < elapsed) low = midpoint; else high = midpoint;
  }
  return cubic(y1, y2, (low + high) * 0.5);
}

}  // namespace

AnimationSystem::AnimationSystem(AnimationConfig config) : config_(config) {}

void AnimationSystem::set_config(AnimationConfig config, std::uint64_t now_us) {
  if (config == config_) return;
  for (auto& entry : states_) {
    auto& state = entry.second;
    state.from = interpolate_values(state.from, state.target, progress(state, now_us));
    const float presence_amount = presence_progress(state, now_us);
    state.from_opacity += (state.target_opacity - state.from_opacity) * presence_progress(state, now_us, true);
    state.from_scale += (state.target_scale - state.from_scale) * presence_amount;
    state.from_offset += (state.target_offset - state.from_offset) * presence_amount;
    state.started_us = now_us;
    state.presence_started_us = now_us;
  }
  config_ = config;
  if (!config_.enabled || config_.duration_ms == 0) {
    for (auto it = states_.begin(); it != states_.end();) {
      if (!it->second.present) it = states_.erase(it);
      else {
        auto& state = it->second;
        state.from = coordinates(state.target);
        state.from_opacity = state.target_opacity = 1.0F;
        state.from_scale = state.target_scale = 1.0;
        state.from_offset = state.target_offset = 0.0;
        state.presence = Presence::steady;
        state.transition = Transition::idle;
        ++it;
      }
    }
  }
}

void AnimationSystem::prune(std::uint64_t now_us) {
  for (auto it = states_.begin(); it != states_.end();) {
    auto& state = it->second;
    if (state.presence != Presence::steady &&
        duration_expired(state.presence_started_us,
                         std::max(presence_duration(state), presence_duration(state, true)), now_us)) {
      if (!state.present) { it = states_.erase(it); continue; }
      state.from_opacity = state.target_opacity;
      state.from_scale = state.target_scale;
      state.from_offset = state.target_offset;
      state.presence = Presence::steady;
    }
    if (it->second.transition == Transition::tracking &&
        same_rect(interpolate(state.from, state.target, progress(state, now_us)), state.target)) {
      it->second.from = coordinates(it->second.target);
      it->second.transition = Transition::idle;
    } else if (it->second.transition == Transition::resizing &&
        duration_expired(it->second.started_us,
                         config_.move_duration_ms ? config_.move_duration_ms : config_.duration_ms, now_us)) {
      it->second.from = coordinates(it->second.target);
      it->second.transition = Transition::idle;
    }
    ++it;
  }
}

void AnimationSystem::update(std::span<const AnimationTarget> targets, std::uint64_t now_us) {
  prune(now_us);
  std::unordered_set<std::uint64_t> present;
  present.reserve(targets.size());
  for (const auto& target : targets) {
    present.insert(target.id);
    auto item = states_.find(target.id);
    if (item == states_.end()) {
      State state{target.bounds, target.bounds};
       if (target.animate_presence && config_.enabled && config_.duration_ms != 0 &&
           config_.open_window) {
        state.from_scale = config_.open_scale_per_mille / 1000.0;
        state.from_offset = config_.open_offset_px;
        state.from_opacity = 0.0F;
        state.presence_started_us = now_us;
        state.presence = Presence::opening;
      }
      states_.emplace(target.id, state);
      continue;
    }
    State& state = item->second;
    const auto current_bounds = interpolate_values(state.from, state.target, progress(state, now_us));
    const bool changed = !same_rect(state.target, target.bounds);
    const bool returning = !state.present;
    state.present = true;
    state.target = target.bounds;
    if (returning) {
      const float amount = presence_progress(state, now_us);
      state.from_opacity += (state.target_opacity - state.from_opacity) * presence_progress(state, now_us, true);
      state.from_scale += (state.target_scale - state.from_scale) * amount;
      state.from_offset += (state.target_offset - state.from_offset) * amount;
      state.target_opacity = 1.0F;
      state.target_scale = 1.0;
      state.target_offset = 0.0;
      state.presence_started_us = now_us;
      state.presence = Presence::opening;
      if (!target.animate_presence || !config_.enabled || config_.duration_ms == 0) {
        state.from_opacity = 1.0F;
        state.from_scale = 1.0;
        state.from_offset = 0.0;
        state.presence = Presence::steady;
      }
    }
    if (target.camera_motion) {
      state.from = coordinates(target.bounds);
      state.transition = Transition::idle;
      continue;
    }
    if (!target.animate && !target.track &&
        (state.transition == Transition::tracking || state.transition == Transition::resizing)) {
      state.from = coordinates(target.bounds);
      state.transition = Transition::idle;
    } else if (target.track && (changed || state.transition != Transition::tracking)) {
      state.from = current_bounds;
      state.started_us = now_us;
      state.transition = Transition::tracking;
    } else if (returning && !target.animate_presence) {
      state.from = coordinates(target.bounds);
      state.transition = Transition::idle;
    } else if (config_.enabled && config_.duration_ms != 0 &&
               ((changed && config_.resize && target.animate) ||
                (returning && target.animate_presence))) {
      state.from = current_bounds;
      state.started_us = now_us;
      state.transition = Transition::resizing;
    } else if (changed || returning) {
      state.from = coordinates(target.bounds);
      state.transition = Transition::idle;
    }
  }
  for (auto it = states_.begin(); it != states_.end();) {
    State& state = it->second;
    if (present.contains(it->first) || !state.present) { ++it; continue; }
    if (!config_.enabled || config_.duration_ms == 0 || !config_.close) {
      it = states_.erase(it);
      continue;
    }
    const auto current_bounds = interpolate(state.from, state.target, progress(state, now_us));
    const float amount = presence_progress(state, now_us);
    state.present = false;
    state.from = coordinates(current_bounds);
    state.target = current_bounds;
    state.transition = Transition::idle;
    state.from_opacity += (state.target_opacity - state.from_opacity) * presence_progress(state, now_us, true);
    state.from_scale += (state.target_scale - state.from_scale) * amount;
    state.from_offset += (state.target_offset - state.from_offset) * amount;
    state.target_opacity = 0.0F;
    state.target_scale = config_.close_scale_per_mille / 1000.0;
    state.target_offset = -static_cast<double>(config_.close_offset_px);
    state.presence_started_us = now_us;
    state.presence = Presence::closing;
    ++it;
  }
}

float AnimationSystem::progress(const State& state, std::uint64_t now_us) const {
  if (state.transition == Transition::idle || !config_.enabled || config_.duration_ms == 0) return 1.0F;
  if (state.transition == Transition::tracking)
    return static_cast<float>(tracking_progress(state.started_us, now_us));
  return progress(state.started_us, config_.move_duration_ms ? config_.move_duration_ms : config_.duration_ms, now_us);
}

float AnimationSystem::progress(std::uint64_t started_us, std::uint32_t duration_ms,
                                std::uint64_t now_us) const {
  if (!config_.enabled || duration_ms == 0) return 1.0F;
  if (now_us <= started_us) return 0.0F;
  const double elapsed = std::clamp(static_cast<double>(now_us - std::min(now_us, started_us)) /
                                        (static_cast<double>(duration_ms) * kMicrosPerMilli),
                                    0.0, 1.0);
  if (elapsed >= 1.0) return 1.0F;
  return static_cast<float>(bezier_progress(elapsed, config_));
}

AnimationSample AnimationSystem::sample_state(const State& state, std::uint64_t now_us) const {
  const float amount = progress(state, now_us);
  const float presence_amount = presence_progress(state, now_us);
  const float opacity_amount = presence_progress(state, now_us, true);
  const double scale = state.from_scale + (state.target_scale - state.from_scale) * presence_amount;
  const double offset = state.from_offset + (state.target_offset - state.from_offset) * presence_amount;
  const auto values = interpolate_values(state.from, state.target, amount);
  auto precise = scaled(renderer::FloatRect{{values[0], values[1]}, {std::max(1.0, values[2]), std::max(1.0, values[3])}}, scale);
  precise.origin.y += offset;
  return {rounded(precise), precise,
          std::clamp(state.from_opacity + (state.target_opacity - state.from_opacity) * opacity_amount, 0.0F, 1.0F),
          !state.present};
}

std::uint32_t AnimationSystem::presence_duration(const State& state, bool fade) const {
  if (fade && config_.fade_duration_ms != 0) return config_.fade_duration_ms;
  const auto duration = state.presence == Presence::closing ? config_.close_duration_ms : config_.open_duration_ms;
  return duration ? duration : config_.duration_ms;
}

float AnimationSystem::presence_progress(const State& state, std::uint64_t now_us, bool fade) const {
  if (state.presence == Presence::steady || !config_.enabled || config_.duration_ms == 0) return 1.0F;
  const double elapsed = static_cast<double>(now_us - std::min(now_us, state.presence_started_us)) /
                          (presence_duration(state, fade) * kMicrosPerMilli);
  if (elapsed >= 1.0) return 1.0F;
  if (elapsed <= 0.0) return 0.0F;
  return static_cast<float>(bezier_progress(elapsed, config_));
}

std::optional<AnimationSample> AnimationSystem::sample(std::uint64_t id, std::uint64_t now_us) const {
  const auto item = states_.find(id);
  return item == states_.end() ? std::nullopt : std::optional<AnimationSample>{sample_state(item->second, now_us)};
}

bool AnimationSystem::active(std::uint64_t now_us) const {
  if (tag_transition(now_us).has_value()) return true;
  return std::any_of(states_.begin(), states_.end(), [this, now_us](const auto& item) {
    if (item.second.presence != Presence::steady &&
        !duration_expired(item.second.presence_started_us,
                          std::max(presence_duration(item.second), presence_duration(item.second, true)), now_us)) return true;
    if (item.second.transition == Transition::tracking)
      return !same_rect(interpolate(item.second.from, item.second.target, progress(item.second, now_us)),
                        item.second.target);
    return item.second.transition != Transition::idle &&
            !duration_expired(item.second.started_us,
                              config_.move_duration_ms ? config_.move_duration_ms : config_.duration_ms, now_us);
  });
}

bool AnimationSystem::retains(std::uint64_t id) const { return states_.contains(id); }

std::vector<std::uint64_t> AnimationSystem::retained_ids() const {
  std::vector<std::uint64_t> ids;
  ids.reserve(states_.size());
  for (const auto& entry : states_) ids.push_back(entry.first);
  return ids;
}

void AnimationSystem::start_tag_transition(int direction, std::uint64_t now_us) {
  tag_transition_ = std::pair{direction < 0 ? -1 : 1, now_us};
}

std::optional<TagTransitionSample> AnimationSystem::tag_transition(std::uint64_t now_us) const {
  if (!tag_transition_ || !config_.enabled || config_.tag_duration_ms == 0) return std::nullopt;
  if (duration_expired(tag_transition_->second, config_.tag_duration_ms, now_us)) return std::nullopt;
  const float amount = std::clamp(progress(tag_transition_->second, config_.tag_duration_ms, now_us),
                                  0.0F, 1.0F);
  return TagTransitionSample{tag_transition_->first, amount, config_.tag_scale_per_mille,
                             config_.tag_parallax_per_mille, config_.tag_fade_per_mille,
                             config_.tag_transition_axis};
}

TagRootSample transform_tag_root(renderer::Rect bounds, std::uint32_t output_width,
                                 std::uint32_t output_height,
                                 const TagTransitionSample& transition, bool outgoing) {
  return transform_tag_root(renderer::FloatRect(bounds), output_width, output_height, transition, outgoing);
}

TagRootSample transform_tag_root(renderer::FloatRect bounds, std::uint32_t output_width,
                                 std::uint32_t output_height,
                                 const TagTransitionSample& transition, bool outgoing) {
  const double progress = std::clamp(static_cast<double>(transition.progress), 0.0, 1.0);
  const double depth = transition.scale_per_mille / 1000.0;
  const double scale = outgoing ? 1.0 + (depth - 1.0) * progress
                                : depth + (1.0 - depth) * progress;
  bounds = scaled(bounds, std::max(0.001, scale));
  const auto extent = transition.axis == TagTransitionAxis::vertical ? output_height : output_width;
  const double travel = outgoing
                            ? -transition.direction * progress * extent *
                                  (transition.parallax_per_mille / 1000.0)
                            : transition.direction * (1.0 - progress) * extent;
  (transition.axis == TagTransitionAxis::vertical ? bounds.origin.y : bounds.origin.x) += travel;
  const double fade = transition.fade_per_mille / 1000.0;
  const double opacity = outgoing ? 1.0 - fade * progress : 1.0 - fade * (1.0 - progress);
  return {rounded(bounds), bounds, static_cast<float>(std::clamp(opacity, 0.0, 1.0))};
}

void AnimationSystem::remove(std::uint64_t id) { states_.erase(id); }

}  // namespace zwwm
