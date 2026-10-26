#include "compositor_server_internal.hpp"

#include <wlr-output-power-management-unstable-v1-zwayland-server.h>

#include <algorithm>
#include <new>

namespace protocol = zwayland::generated;
namespace zwwm::detail {
namespace {

OutputState* find_output(Observer* observer, const zwayland::server::Resource* resource) {
  if (observer->outputs == nullptr || resource == nullptr) return nullptr;
  for (auto& output : *observer->outputs)
    if (!output->retired && std::find(output->resources.begin(), output->resources.end(), resource) != output->resources.end())
      return output.get();
  return nullptr;
}

std::uint32_t mode(bool powered) {
  return powered ? protocol::ZWLR_OUTPUT_POWER_V1_MODE_ON : protocol::ZWLR_OUTPUT_POWER_V1_MODE_OFF;
}

struct OutputPowerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void set_mode(zwayland::server::Client&, zwayland::server::Resource& resource, std::uint32_t requested) {
    auto* power = resource.data<OutputPowerState>();
    if (requested != protocol::ZWLR_OUTPUT_POWER_V1_MODE_ON && requested != protocol::ZWLR_OUTPUT_POWER_V1_MODE_OFF) {
      resource.post_error(protocol::ZWLR_OUTPUT_POWER_V1_ERROR_INVALID_MODE, "invalid output power mode");
      return;
    }
    auto* output = power == nullptr ? nullptr : output_state(power->observer, power->output);
    if (output == nullptr) { protocol::zwlr_output_power_v1_send_failed(resource); return; }
    const bool on = requested == protocol::ZWLR_OUTPUT_POWER_V1_MODE_ON;
    if (output->powered == on) { protocol::zwlr_output_power_v1_send_mode(resource, mode(on)); return; }
    auto* observer = power->observer;
    if (observer->output_power_callback != nullptr &&
        !observer->output_power_callback(observer->output_power_data, power->output, on)) {
      protocol::zwlr_output_power_v1_send_failed(resource);
      return;
    }
    output->powered = on;
    publish_output_power(observer, power->output);
  }
};

struct OutputPowerManagerHandler {
  void destroy(zwayland::server::Client&, zwayland::server::Resource& resource) { resource.destroy(); }
  void get_output_power(zwayland::server::Client& client, zwayland::server::Resource& manager, std::uint32_t id,
                        zwayland::server::Resource* output_resource) {
    auto* observer = manager.data<Observer>();
    auto* resource = client.create_resource(&protocol::zwlr_output_power_v1_interface, id, 1);
    if (resource == nullptr) { client.post_no_memory(); return; }
    resource->set_handler(protocol::zwlr_output_power_v1_handler(OutputPowerHandler{}));
    auto* output = find_output(observer, output_resource);
    if (output == nullptr) { protocol::zwlr_output_power_v1_send_failed(*resource); return; }
    auto* power = new (std::nothrow) OutputPowerState{observer, resource, output->info.id};
    if (power == nullptr) { resource->destroy(); client.post_no_memory(); return; }
    observer->output_powers.push_back(power);
    resource->set_data(power);
    resource->set_destroy_handler([](zwayland::server::Resource& destroyed) {
      auto* power = destroyed.data<OutputPowerState>();
      std::erase(power->observer->output_powers, power);
      delete power;
    });
    protocol::zwlr_output_power_v1_send_mode(*resource, mode(output->powered));
  }
};

}  // namespace

void publish_output_power(Observer* observer, OutputId output) {
  const auto* state = output_state(observer, output);
  for (auto* power : observer->output_powers) {
    if (power->output != output) continue;
    if (state == nullptr) protocol::zwlr_output_power_v1_send_failed(*power->resource);
    else protocol::zwlr_output_power_v1_send_mode(*power->resource, mode(state->powered));
  }
}

void bind_output_power_manager(zwayland::server::Client* client, void* data, std::uint32_t, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::zwlr_output_power_manager_v1_interface, id, 1);
  if (resource == nullptr) { client->post_no_memory(); return; }
  resource->set_data(static_cast<Observer*>(data));
  resource->set_handler(protocol::zwlr_output_power_manager_v1_handler(OutputPowerManagerHandler{}));
}

}  // namespace zwwm::detail
