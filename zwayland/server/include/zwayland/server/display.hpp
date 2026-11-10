#pragma once

#include <any>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zwayland/server/event_loop.hpp"
#include "zwayland/server/interface.hpp"
#include "zwayland/server/signal.hpp"
#include "zwayland/wire/connection.hpp"
#include "zwayland/wire/message.hpp"
#include "zwayland/wire/object_map.hpp"

namespace zwayland::server {

class Display;
class Client;
struct Resource;

using RequestHandler =
    std::function<void(Client& client, Resource& resource, std::uint32_t opcode,
                       wire::MessageParser& parser)>;

// userdata is type-erased and owned here.
struct Resource {
  std::uint32_t id = 0;
  std::uint32_t version = 1;
  const Interface* interface = nullptr;
  std::any userdata;
  RequestHandler handler;
  std::function<void(Resource&)> destroy_handler;
  Display* display = nullptr;
  class Client* client = nullptr;

  // Fired with `this` before removal, by destroy() or client teardown.
  Signal destroy_signal;

  // Returns a pointer to the type-erased userdata for convenient access.
  [[nodiscard]] void* get_user_data() { return &userdata; }
  [[nodiscard]] const void* get_user_data() const { return &userdata; }
  [[nodiscard]] class Client* get_client() { return client; }
  [[nodiscard]] const char* get_class() const {
    return interface ? interface->name : "";
  }

  // Triggers `destroy_signal` then removes this resource from its client.
  void destroy();
  // Delegates to `client->post_error`.
  void post_error(std::uint32_t code, std::string_view message);

  template <typename T>
  [[nodiscard]] T* data() const {
    const auto value = std::any_cast<T*>(&userdata);
    return value == nullptr ? nullptr : *value;
  }

  template <typename T>
  void set_data(T* value) { userdata = value; }
  void set_data(std::nullptr_t) { userdata.reset(); }

  void set_handler(RequestHandler value) { handler = std::move(value); }
  void set_destroy_handler(std::function<void(Resource&)> value) {
    destroy_handler = std::move(value);
  }
};

class Client {
 public:
  Client(Display& display, int fd);
  Client(Display& display, std::unique_ptr<wire::Connection> connection);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] int fd() const { return connection_->fd(); }
  [[nodiscard]] Display& display() const { return display_; }
  [[nodiscard]] wire::Connection& connection() { return *connection_; }

  // False on hangup/error so the display can drop the client.
  [[nodiscard]] bool dispatch_available();

  // Flushes queued outgoing messages.
  void flush();

  // Allocates a server-side object id (>= 0xff000000).
  [[nodiscard]] std::uint32_t alloc_id();

  // `id` must be allocated or client-supplied via bind/new_id.
  Resource* create_resource(const Interface* interface, std::uint32_t id,
                             std::uint32_t version = 1, std::any userdata = {});
  [[nodiscard]] bool valid_new_id(std::uint32_t id) const;
  [[nodiscard]] Resource* find_resource(std::uint32_t id);
  void destroy_resource(std::uint32_t id);

  // Queues an event addressed to `object_id`.
  void send_event(std::uint32_t object_id, std::uint32_t opcode,
                  std::vector<std::byte> bytes, std::vector<int> fds = {});
  // Encodes straight from the builder into the output buffer and takes its FDs.
  void send_event(wire::MessageBuilder& message);

  // Posts a protocol error on the client's wl_display and tears it down.
  void post_error(std::uint32_t code, std::string_view message);
  void post_error(std::uint32_t object_id, std::uint32_t code,
                  std::string_view message);

  // Posts wl_display out-of-memory and marks the client dead.
  void post_no_memory();

  // Fired exactly once when the client is removed from the display.
  Signal destroy_signal;
  void add_destroy_listener(Listener& l) { destroy_signal.add(l); }

  // Emits resource then client destroy signals and removes the client.
  void destroy();

  [[nodiscard]] bool dead() const { return dead_; }
  [[nodiscard]] pid_t pid() const { return pid_; }
  [[nodiscard]] uid_t uid() const { return uid_; }
  [[nodiscard]] gid_t gid() const { return gid_; }

  // Globals actually advertised, so bind/global_remove obey visibility.
  std::unordered_set<std::uint32_t> registry_ids;

 private:
  Display& display_;
  std::unique_ptr<wire::Connection> connection_;
  friend class Display;
  void queued_output(bool ok);
  void reap();

  wire::ObjectMap<Resource> resources_;
  // Resources destroyed mid-dispatch stay allocated until the dispatch unwinds.
  std::vector<std::unique_ptr<Resource>> graveyard_;
  std::size_t dispatch_depth_ = 0;
  // Delayed reuse so late client requests cannot hit a recycled object.
  std::deque<std::uint32_t> free_server_ids_;
  std::uint32_t next_server_id_ = wire::kServerIdStart;
  bool dead_ = false;
  bool destroying_ = false;
  bool dirty_ = false;
  bool write_armed_ = false;
  pid_t pid_ = 0;
  uid_t uid_ = 0;
  gid_t gid_ = 0;
};

// bind creates the resource and sends the initial events.
using BindHandler = std::function<void(Client&, std::uint32_t version, std::uint32_t id)>;

struct Global {
  const Interface* interface = nullptr;
  std::uint32_t version = 1;
  std::uint32_t name = 0;
  BindHandler bind;
  bool removed = false;
};

class Display {
 public:
  explicit Display(const std::string& socket_name = {});
  ~Display();

  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  friend class Client;

  // Returns the global's name; registries are notified.
  std::uint32_t add_global(const Interface* interface, std::uint32_t version,
                           BindHandler bind);

  // Withdraws a global and notifies existing registries (global_remove).
  void remove_global(std::uint32_t name);
  void destroy_global(std::uint32_t name);

  // Connection must already be linked to a peer with its reader set.
  Client* add_loopback_client(std::unique_ptr<wire::Connection> connection);
  Client* add_client(int fd);

  // Registers the handler for requests addressed to resources of `interface`.
  void set_handler(const Interface* interface, RequestHandler handler);

  // Returns the handler registered for `interface`, or nullptr.
  [[nodiscard]] RequestHandler* handler_for(const Interface* interface);

  // Monotonic, starting at 1.
  std::uint32_t next_serial();

  // Event-loop passthroughs backed by the display's EventLoop.
  int add_idle(std::function<void()> cb) { return loop_.add_idle(std::move(cb)); }
  int add_timer(std::uint32_t ms, std::function<void()> cb) {
    return loop_.add_timer(ms, std::move(cb));
  }
  int add_timer(std::function<void()> cb) {
    return loop_.add_timer(std::move(cb));
  }
  void update_timer(int handle, std::uint32_t ms) {
    loop_.update_timer(handle, ms);
  }
  void remove_source(int handle) { loop_.remove(handle); }

  // Runs the event loop until stop() is called.
  void run();
  void stop();
  // Dirty clients already flush before the loop sleeps.
  void flush_clients();
  // Clients whose unsent output exceeds this are disconnected (0 = unlimited).
  void set_max_client_buffer(std::size_t bytes);

  [[nodiscard]] const std::string& socket_name() const { return socket_name_; }
  [[nodiscard]] const std::vector<Global>& globals() const { return globals_; }
  [[nodiscard]] EventLoop& event_loop() { return loop_; }
  void set_global_filter(std::function<bool(const Client&, const Global&)> filter);
  // Limits one global to accepted clients, independently of the global filter.
  void restrict_global(std::uint32_t name, std::function<bool(const Client&)> allowed);
  Signal destroy_signal;

  // Internal: used by Client for registry publication.
  void notify_registry(Client& client, std::uint32_t registry_id);

 private:
  void accept_client();
  void remove_client(Client* client);
  void schedule_remove_client(Client* client);
  void client_output_changed(Client& client);
  void flush_dirty_clients();
  void flush_client(Client& client);
  void schedule_destroy_client(Client* client);
  void notify_global(Client& client, const Global& global);
  [[nodiscard]] bool global_visible(const Client& client, const Global& global) const;

  std::string socket_name_;
  std::string socket_path_;
  std::string lock_path_;
  int listen_fd_ = -1;
  int lock_fd_ = -1;

  EventLoop loop_;
  std::uint32_t next_serial_ = 0;

  std::vector<Global> globals_;
  std::uint32_t next_global_name_ = 1;

  std::unordered_map<const Interface*, RequestHandler> handlers_;
  std::function<bool(const Client&, const Global&)> global_filter_;
  std::unordered_map<std::uint32_t, std::function<bool(const Client&)>> global_restrictions_;
  std::vector<std::unique_ptr<Client>> clients_;
  std::vector<Client*> dirty_clients_;
  std::size_t max_client_buffer_ = 4 * 1024 * 1024;
};

}  // namespace zwayland::server
