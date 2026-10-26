#include "compositor_server_internal.hpp"

#include <zwwm-edge-swipe-v1-zwayland-server.h>

#include <algorithm>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {

const OutputState* output_at(const Observer* observer, std::int32_t x, std::int32_t y) {
  if (observer == nullptr || observer->outputs == nullptr) return nullptr;
  for (const auto& output : *observer->outputs) {
    const auto& info = output->info;
    if (!output->retired && x >= info.logical_x && y >= info.logical_y &&
        x < info.logical_x + static_cast<std::int32_t>(info.logical_width) &&
        y < info.logical_y + static_cast<std::int32_t>(info.logical_height)) return output.get();
  }
  return nullptr;
}

// Offsets along and across the swipe, positive towards the output's inside.
std::pair<double, double> offsets(std::uint32_t edge, double dx, double dy) {
  switch (edge) {
    case protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_TOP: return {dy, dx};
    case protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_BOTTOM: return {-dy, dx};
    case protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_LEFT: return {dx, dy};
    default: return {-dx, dy};
  }
}

void finish(SeatState* seat, std::uint32_t time, bool cancelled) {
  if (!seat->edge_swipe) return;
  const auto swipe = *seat->edge_swipe;
  seat->edge_swipe.reset();
  if (swipe.target != nullptr)
    protocol::zwwm_edge_swipe_v1_send_ended(*swipe.target->resource, time, swipe.distance, swipe.lateral, swipe.velocity,
                                            cancelled ? 1U : 0U);
}

struct EdgeSwipeHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void set_edges(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t edges) {
    resource.data<EdgeSwipeState>()->edges = edges;
  }
};

struct EdgeSwipeManagerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_edge_swipe(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id,
                      std::uint32_t edges) {
    auto* seat = manager.data<SeatState>();
    auto* resource = client.create_resource(&protocol::zwwm_edge_swipe_v1_interface, id, 1);
    auto* swipe = resource == nullptr ? nullptr : new (std::nothrow) EdgeSwipeState{seat, resource, edges};
    if (swipe == nullptr) {
      if (resource != nullptr) resource->destroy();
      client.post_no_memory();
      return;
    }
    seat->edge_swipes.push_back(swipe);
    resource->set_data(swipe);
    resource->set_handler(protocol::zwwm_edge_swipe_v1_handler(EdgeSwipeHandler{}));
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) {
      auto* swipe = destroyed.data<EdgeSwipeState>();
      auto* seat = swipe->seat;
      if (seat->edge_swipe && seat->edge_swipe->target == swipe) seat->edge_swipe->target = nullptr;
      std::erase(seat->edge_swipes, swipe);
      delete swipe;
    });
  }
};

}  // namespace

bool edge_swipe_touch_down(SeatState* seat, std::uint32_t time, std::int32_t id, std::int32_t x, std::int32_t y) {
  const auto* observer = seat->display.observer;
  if (seat->edge_swipe || seat->edge_swipes.empty() || observer == nullptr || observer->config == nullptr) return false;
  const auto size = static_cast<std::int32_t>(observer->config->mobile.edge_swipe_size);
  const auto* output = output_at(observer, x, y);
  if (size <= 0 || output == nullptr) return false;
  const auto& info = output->info;
  const std::int32_t local_x = x - info.logical_x, local_y = y - info.logical_y;
  const std::array<std::pair<std::uint32_t, std::int32_t>, 4> distances{{
      {protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_TOP, local_y},
      {protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_BOTTOM, static_cast<std::int32_t>(info.logical_height) - 1 - local_y},
      {protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_LEFT, local_x},
      {protocol::ZWWM_EDGE_SWIPE_MANAGER_V1_EDGE_RIGHT, static_cast<std::int32_t>(info.logical_width) - 1 - local_x}}};
  const auto nearest = std::min_element(distances.begin(), distances.end(),
                                        [](const auto& left, const auto& right) { return left.second < right.second; });
  if (nearest->second >= size) return false;
  const auto target = std::find_if(seat->edge_swipes.rbegin(), seat->edge_swipes.rend(),
                                   [edge = nearest->first](const EdgeSwipeState* swipe) { return (swipe->edges & edge) != 0; });
  if (target == seat->edge_swipes.rend()) return false;
  auto& swipe = seat->edge_swipe.emplace();
  swipe.target = *target;
  swipe.id = id;
  swipe.edge = nearest->first;
  swipe.x = x;
  swipe.y = y;
  swipe.time = swipe.last_time = time;
  zwayland::server::Resource* output_resource = nullptr;
  for (auto* resource : output->resources)
    if (resource->client == (*target)->resource->client) { output_resource = resource; break; }
  protocol::zwwm_edge_swipe_v1_send_began(*(*target)->resource, time, output_resource, nearest->first,
                                          static_cast<double>(local_x), static_cast<double>(local_y));
  return true;
}

bool edge_swipe_touch_motion(SeatState* seat, std::uint32_t time, std::int32_t id, std::int32_t x, std::int32_t y) {
  if (!seat->edge_swipe || seat->edge_swipe->id != id) return false;
  auto& swipe = *seat->edge_swipe;
  const auto [distance, lateral] = offsets(swipe.edge, x - swipe.x, y - swipe.y);
  if (time > swipe.last_time) {
    const double instant = (distance - swipe.distance) * 1000.0 / static_cast<double>(time - swipe.last_time);
    swipe.velocity = swipe.velocity == 0.0 ? instant : swipe.velocity * 0.6 + instant * 0.4;
  }
  swipe.distance = distance;
  swipe.lateral = lateral;
  swipe.last_time = time;
  if (swipe.target != nullptr) protocol::zwwm_edge_swipe_v1_send_progress(*swipe.target->resource, time, distance, lateral);
  return true;
}

bool edge_swipe_touch_up(SeatState* seat, std::uint32_t time, std::int32_t id) {
  if (!seat->edge_swipe || seat->edge_swipe->id != id) return false;
  finish(seat, time, false);
  return true;
}

void edge_swipe_cancel(SeatState* seat, std::uint32_t time) { finish(seat, time, true); }

void bind_edge_swipe_manager(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwwm_edge_swipe_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<SeatState*>(data));
  resource->set_handler(protocol::zwwm_edge_swipe_manager_v1_handler(EdgeSwipeManagerHandler{}));
}

}  // namespace zwwm::detail
