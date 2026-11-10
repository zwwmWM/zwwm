#include "zwayland/server/display.hpp"

#include <wayland-zwayland-server.h>

#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#if defined(__FreeBSD__)
#include <sys/ucred.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace zwayland::server {

namespace {
namespace protocol = zwayland::generated;
}  // namespace

// ---- Client ----------------------------------------------------------------

namespace {
// Freed server ids wait in FIFO order until this many are queued.
constexpr std::size_t kServerIdQuarantine = 64;
}  // namespace

Client::Client(Display& display, int fd)
    : display_(display), connection_(std::make_unique<wire::Connection>(fd)) {
#if defined(__FreeBSD__)
  struct xucred credentials {};
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &size) == 0 &&
      credentials.cr_version == XUCRED_VERSION) {
    pid_ = credentials.cr_pid;
    uid_ = credentials.cr_uid;
    gid_ = credentials.cr_ngroups > 0 ? credentials.cr_groups[0] : 0;
  }
#else
  struct ucred credentials {};
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0) {
    pid_ = credentials.pid;
    uid_ = credentials.uid;
    gid_ = credentials.gid;
  }
#endif
  connection_->set_max_output(display_.max_client_buffer_);
  // Bootstrap the wl_display object at id 1, as libwayland does.
  create_resource(&protocol::wl_display_interface, 1, 1);
}

Client::Client(Display& display, std::unique_ptr<wire::Connection> connection)
    : display_(display), connection_(std::move(connection)) {
  create_resource(&protocol::wl_display_interface, 1, 1);
}

Client::~Client() = default;

void Client::reap() {
  if (dispatch_depth_ == 0) graveyard_.clear();
}

bool Client::dispatch_available() {
  if (dead_) return false;
  const bool got_input = connection_->read_available();
  if (!got_input)
    return connection_->healthy() && !connection_->peer_closed();

  ++dispatch_depth_;
  bool ok = true;
  while (ok) {
    auto message = connection_->next_message();
    if (!message) break;

    Resource* resource = resources_.find(message->object_id);
    if (resource == nullptr) {
      connection_->consume(message->size, 0);
      post_error(1, 0, "invalid object");
      ok = false;
      break;
    }
    const Interface* iface = resource->interface;
    const std::uint32_t resource_id = resource->id;
    if (message->opcode >= iface->method_count) {
      connection_->consume(message->size, 0);
      post_error(resource_id, 1, "invalid method");
      ok = false;
      break;
    }
    const Message& method = iface->methods[message->opcode];
    if (method.since > resource->version) {
      connection_->consume(message->size, 0);
      post_error(resource_id, 1, "request requires a newer object version");
      ok = false;
      break;
    }
    // The signature, not the handler, decides how many FDs this message owns.
    const std::size_t fd_count = wire::signature_fd_count(method.signature);
    if (message->fds.size() < fd_count) {
      connection_->consume(message->size, 0);
      post_error(resource_id, 1, "request is missing a file descriptor");
      ok = false;
      break;
    }

    wire::MessageParser parser(message->body, message->fds.first(fd_count));
    std::string failure;
    bool failed = false;
    try {
      if (resource->handler) {
        resource->handler(*this, *resource, message->opcode, parser);
      } else if (auto* handler = display_.handler_for(iface)) {
        (*handler)(*this, *resource, message->opcode, parser);
      } else {
        throw std::runtime_error("object has no request handler");
      }
      if (!parser.at_end()) throw std::runtime_error("request has trailing data");
    } catch (const std::exception& exception) {
      failed = true;
      failure = exception.what();
    }
    for (std::size_t i = parser.consumed_fds(); i < fd_count; ++i) close(message->fds[i]);
    connection_->consume(message->size, fd_count);
    if (failed) {
      post_error(resource_id, 1, failure);
      ok = false;
    }
    if (dead_) ok = false;
  }
  --dispatch_depth_;
  reap();
  return ok && connection_->healthy() && !connection_->peer_closed();
}

void Client::flush() { (void)connection_->flush(); }

std::uint32_t Client::alloc_id() {
  while (free_server_ids_.size() > kServerIdQuarantine) {
    const std::uint32_t id = free_server_ids_.front();
    free_server_ids_.pop_front();
    if (!resources_.contains(id)) return id;
  }
  if (next_server_id_ >= wire::kServerIdStart && next_server_id_ != 0xffffffffU)
    return next_server_id_++;
  // Id space exhausted: fall back to any quarantined id.
  while (!free_server_ids_.empty()) {
    const std::uint32_t id = free_server_ids_.front();
    free_server_ids_.pop_front();
    if (!resources_.contains(id)) return id;
  }
  return 0;
}

Resource* Client::create_resource(const Interface* interface, std::uint32_t id,
                                   std::uint32_t version, std::any userdata) {
  if (id == 0) id = alloc_id();
  if (interface == nullptr || id == 0 || version == 0) return nullptr;
  auto resource = std::make_unique<Resource>();
  resource->id = id;
  resource->version = std::min(version, interface->version);
  resource->interface = interface;
  resource->userdata = std::move(userdata);
  resource->display = &display_;
  resource->client = this;
  Resource* result = resource.get();
  return resources_.insert(id, std::move(resource)) ? result : nullptr;
}

bool Client::valid_new_id(std::uint32_t id) const {
  return id > 0 && id < wire::kServerIdStart && !resources_.contains(id);
}

Resource* Client::find_resource(std::uint32_t id) { return resources_.find(id); }

void Client::destroy_resource(std::uint32_t id) {
  // Unlinking first makes re-entrant destroys of the same id a no-op.
  std::unique_ptr<Resource> owner = resources_.take(id);
  if (!owner) return;
  Resource& resource = *owner;
  resource.destroy_signal.emit_final(&resource);
  if (resource.destroy_handler) resource.destroy_handler(resource);
  if (id < wire::kServerIdStart) {
    if (!dead_) {
      wire::MessageBuilder builder(1);  // wl_display.delete_id
      builder.set_opcode(1);
      builder.append_uint(id);
      send_event(builder);
    }
  } else {
    free_server_ids_.push_back(id);
  }
  if (dispatch_depth_ > 0) graveyard_.push_back(std::move(owner));
}

void Client::queued_output(bool ok) {
  if (ok) {
    display_.client_output_changed(*this);
  } else if (!dead_) {
    // Backlog cap hit or allocation failed: the client is not keeping up.
    dead_ = true;
    display_.schedule_destroy_client(this);
  }
}

void Client::send_event(wire::MessageBuilder& message) {
  if (dead_) return;
  std::vector<int> fds = message.take_fds();
  queued_output(connection_->queue(message.seal(), fds));
}

void Client::send_event([[maybe_unused]] std::uint32_t object_id,
                        [[maybe_unused]] std::uint32_t opcode,
                        std::vector<std::byte> bytes, std::vector<int> fds) {
  if (dead_) {
    for (int fd : fds) close(fd);
    return;
  }
  queued_output(connection_->queue(std::span<const std::byte>(bytes), std::span<const int>(fds)));
}

void Client::post_error(std::uint32_t code, std::string_view message) {
  post_error(1, code, message);
}

void Client::post_error(std::uint32_t object_id, std::uint32_t code,
                        std::string_view message) {
  if (dead_) return;
  dead_ = true;
  if (find_resource(1) == nullptr) return;
  wire::MessageBuilder builder(1);
  builder.set_opcode(0);  // wl_display.error
  builder.append_object(object_id);
  builder.append_uint(code);
  builder.append_string(message);
  (void)connection_->queue(builder.seal(), {});
  (void)connection_->flush();
}

void Client::post_no_memory() { post_error(2, "out of memory"); }

void Client::destroy() {
  if (destroying_) return;
  destroying_ = true;
  destroy_signal.emit_final(this);
  for (std::uint32_t id : resources_.ids()) destroy_resource(id);
  flush();
  dead_ = true;
  display_.schedule_remove_client(this);
}

void Resource::destroy() {
  if (client != nullptr) client->destroy_resource(id);
}

void Resource::post_error(std::uint32_t code, std::string_view message) {
  if (client != nullptr) client->post_error(id, code, message);
}

// ---- Display --------------------------------------------------------------

Display::Display(const std::string& socket_name) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if ((socket_name.empty() || socket_name.front() != '/') &&
      (runtime == nullptr || runtime[0] != '/'))
    throw std::runtime_error("XDG_RUNTIME_DIR is not set");

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) throw std::runtime_error("cannot create socket");

  const auto bind_socket = [&](const std::string& candidate) {
    const std::string path = candidate.front() == '/'
                                 ? candidate
                                 : std::string(runtime) + "/" + candidate;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return false;
    const std::string lock_path = path + ".lock";
    if (lock_path.size() >= sizeof(sockaddr_un::sun_path) + 5U) return false;
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR |
                                                   O_NOFOLLOW,
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (lock_fd < 0) return false;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
      close(lock_fd);
      return false;
    }
    struct stat socket_stat {};
    if (lstat(path.c_str(), &socket_stat) == 0) {
      if ((socket_stat.st_mode & (S_IWUSR | S_IWGRP)) == 0 ||
          unlink(path.c_str()) < 0) {
        unlink(lock_path.c_str());
        close(lock_fd);
        return false;
      }
    } else if (errno != ENOENT) {
      unlink(lock_path.c_str());
      close(lock_fd);
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1U);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), address_size) < 0) {
      unlink(lock_path.c_str());
      close(lock_fd);
      return false;
    }
    socket_path_ = path;
    lock_path_ = lock_path;
    lock_fd_ = lock_fd;
    return true;
  };
  if (socket_name.empty()) {
    for (std::uint32_t index = 0; index != 32; ++index) {
      const std::string candidate = "wayland-" + std::to_string(index);
      if (bind_socket(candidate)) {
        socket_name_ = candidate;
        break;
      }
    }
  } else if (bind_socket(socket_name)) {
    socket_name_ = socket_name;
  }
  const auto cleanup_socket = [this]() {
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (!socket_path_.empty()) unlink(socket_path_.c_str());
    if (!lock_path_.empty()) unlink(lock_path_.c_str());
    if (lock_fd_ >= 0) {
      close(lock_fd_);
      lock_fd_ = -1;
    }
  };
  if (socket_name_.empty()) {
    cleanup_socket();
    throw std::runtime_error("cannot bind Wayland socket");
  }
  if (listen(listen_fd_, 128) < 0) {
    cleanup_socket();
    throw std::runtime_error("cannot listen");
  }

  try {
    loop_.add_fd(listen_fd_, kEventReadable, [this](int, int) {
      accept_client();
      return true;
    });
    // One flush per client per loop iteration, however many events were sent.
    loop_.add_prepare([this] { flush_dirty_clients(); });
  } catch (...) {
    cleanup_socket();
    throw;
  }

  // Register built-in core handlers.
  set_handler(&protocol::wl_display_interface, [this](Client& client, Resource&, uint32_t opcode,
                                        wire::MessageParser& parser) {
    if (opcode == 0) {  // sync
      const uint32_t id = parser.read_new_id();
      Resource* cb = client.create_resource(&protocol::wl_callback_interface, id, 1);
      if (cb == nullptr) {
        client.post_no_memory();
        return;
      }
      wire::MessageBuilder done(id);
      done.set_opcode(0);
      done.append_uint(next_serial_);
      client.send_event(done);
      cb->destroy();
    } else if (opcode == 1) {  // get_registry
      const uint32_t id = parser.read_new_id();
      Resource* registry = client.create_resource(&protocol::wl_registry_interface, id, 1);
      if (registry == nullptr) {
        client.post_no_memory();
        return;
      }
      client.registry_ids.insert(id);
      registry->set_destroy_handler([&client, id](Resource&) {
        client.registry_ids.erase(id);
      });
      notify_registry(client, id);
    }
  });

  set_handler(&protocol::wl_registry_interface, [this](Client& client, Resource& registry, uint32_t,
                                         wire::MessageParser& parser) {
    const uint32_t name = parser.read_uint();
    const std::string iface = std::string(parser.read_string());
    const uint32_t version = parser.read_uint();
    const uint32_t id = parser.read_new_id();
    for (const auto& global : globals_) {
      if (global.name != name) continue;
      if (!global_visible(client, global)) break;
      if (global.interface->name != iface) {
        registry.post_error(0, "interface mismatch for global");
        return;
      }
      if (version == 0 || global.version < version) {
        registry.post_error(0, "invalid version for global");
        return;
      }
      if (global.bind) global.bind(client, version, id);
      return;
    }
    registry.post_error(0, "unknown global");
  });
}

Display::~Display() {
  destroy_signal.emit_final(this);
  for (auto& client : clients_) client->destroy();
  clients_.clear();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    if (!socket_path_.empty()) unlink(socket_path_.c_str());
  }
  if (!lock_path_.empty()) unlink(lock_path_.c_str());
  if (lock_fd_ >= 0) close(lock_fd_);
}

Client* Display::add_loopback_client(std::unique_ptr<wire::Connection> connection) {
  auto client = std::make_unique<Client>(*this, std::move(connection));
  Client* raw = client.get();
  // Process loopback data synchronously and flush replies over the same link.
  raw->connection().set_data_ready([raw]() {
    if (raw->dispatch_available()) raw->flush();
    else raw->destroy();
  });
  clients_.push_back(std::move(client));
  return raw;
}

Client* Display::add_client(int fd) {
  auto client = std::make_unique<Client>(*this, fd);
  Client* raw = client.get();
  loop_.add_fd(fd, kEventReadable, [this, raw](int, int mask) {
    if ((mask & (kEventError | kEventHangup)) != 0 ||
        ((mask & kEventReadable) != 0 && !raw->dispatch_available())) {
      raw->destroy();
      return false;
    }
    // Replies are flushed by the prepare hook; only drain a backlog here.
    if ((mask & kEventWritable) != 0) flush_client(*raw);
    return true;
  });
  clients_.push_back(std::move(client));
  return raw;
}

std::uint32_t Display::add_global(const Interface* interface,
                                   std::uint32_t version, BindHandler bind) {
  if (interface == nullptr || version == 0)
    throw std::invalid_argument("global requires an interface and version");
  Global global;
  global.interface = interface;
  if (version > interface->version)
    throw std::invalid_argument("global version exceeds interface version");
  if (next_global_name_ == std::numeric_limits<std::uint32_t>::max())
    throw std::overflow_error("Wayland global name space is exhausted");
  global.version = version;
  global.name = next_global_name_++;
  global.bind = std::move(bind);
  globals_.push_back(global);
  const Global announced = globals_.back();
  for (auto& client : clients_) notify_global(*client, announced);
  return global.name;
}

void Display::set_handler(const Interface* interface, RequestHandler handler) {
  handlers_[interface] = std::move(handler);
}

RequestHandler* Display::handler_for(const Interface* interface) {
  auto it = handlers_.find(interface);
  return it == handlers_.end() ? nullptr : &it->second;
}

void Display::notify_registry(Client& client, std::uint32_t registry_id) {
  for (const auto& global : globals_) {
    if (global.removed || !global_visible(client, global)) continue;
    wire::MessageBuilder builder(registry_id);
    builder.set_opcode(0);  // wl_registry.global
    builder.append_uint(global.name);
    builder.append_string(global.interface->name);
    builder.append_uint(global.version);
    client.send_event(builder);
  }
}

void Display::notify_global(Client& client, const Global& global) {
  if (!global_visible(client, global)) return;
  for (const std::uint32_t registry_id : client.registry_ids) {
    wire::MessageBuilder builder(registry_id);
    builder.set_opcode(0);  // wl_registry.global
    builder.append_uint(global.name);
    builder.append_string(global.interface->name);
    builder.append_uint(global.version);
    client.send_event(builder);
  }
}

void Display::accept_client() {
  int client_fd = accept4(listen_fd_, nullptr, nullptr,
                          SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (client_fd < 0) return;
  add_client(client_fd);
}

void Display::remove_client(Client* client) {
  std::erase(dirty_clients_, client);
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (it->get() == client) {
      int fd = client->fd();
      loop_.remove(fd);
      clients_.erase(it);
      return;
    }
  }
}

void Display::schedule_remove_client(Client* client) {
  loop_.add_idle([this, client]() { remove_client(client); });
}

void Display::schedule_destroy_client(Client* client) {
  loop_.add_idle([this, client]() {
    const bool live = std::any_of(clients_.begin(), clients_.end(),
                                  [client](const auto& entry) { return entry.get() == client; });
    if (live) client->destroy();
  });
}

void Display::client_output_changed(Client& client) {
  // In-process peers expect synchronous delivery (roundtrips rely on it).
  if (client.connection().loopback()) {
    client.flush();
    return;
  }
  if (!client.dirty_) {
    client.dirty_ = true;
    dirty_clients_.push_back(&client);
  }
}

void Display::flush_client(Client& client) {
  if (client.dead_) return;
  if (!client.connection().flush() || !client.connection().healthy()) {
    client.dead_ = true;
    schedule_destroy_client(&client);
    return;
  }
  // Writable is armed only while the socket is backed up.
  const bool backlog = client.connection().has_output();
  if (backlog != client.write_armed_ && client.fd() >= 0) {
    loop_.update_fd(client.fd(), kEventReadable | (backlog ? kEventWritable : 0));
    client.write_armed_ = backlog;
  }
}

void Display::flush_dirty_clients() {
  if (dirty_clients_.empty()) return;
  std::vector<Client*> dirty;
  dirty.swap(dirty_clients_);
  for (Client* client : dirty) {
    client->dirty_ = false;
    flush_client(*client);
  }
  // Keep the vector's capacity for the next iteration.
  if (dirty_clients_.empty()) {
    dirty.clear();
    dirty_clients_.swap(dirty);
  }
}

void Display::set_max_client_buffer(std::size_t bytes) {
  max_client_buffer_ = bytes;
  for (auto& client : clients_)
    if (!client->connection().loopback()) client->connection().set_max_output(bytes);
}

void Display::run() { loop_.run(); }

void Display::flush_clients() {
  for (auto& client : clients_) {
    if (client->connection().loopback()) client->flush();
    else flush_client(*client);
  }
}

void Display::set_global_filter(
    std::function<bool(const Client&, const Global&)> filter) {
  global_filter_ = std::move(filter);
}

void Display::restrict_global(std::uint32_t name, std::function<bool(const Client&)> allowed) {
  global_restrictions_[name] = std::move(allowed);
}

bool Display::global_visible(const Client& client, const Global& global) const {
  const auto restriction = global_restrictions_.find(global.name);
  if (restriction != global_restrictions_.end() && !restriction->second(client)) return false;
  return !global_filter_ || global_filter_(client, global);
}

void Display::remove_global(std::uint32_t name) {
  auto global = std::find_if(globals_.begin(), globals_.end(),
                             [name](const Global& item) { return item.name == name; });
  if (global == globals_.end() || global->removed) return;
  global->removed = true;
  global_restrictions_.erase(name);
  const Global removed = *global;
  for (auto& client : clients_) {
    if (!global_visible(*client, removed)) continue;
    for (const std::uint32_t registry_id : client->registry_ids) {
      wire::MessageBuilder builder(registry_id);
      builder.set_opcode(1);  // wl_registry.global_remove
      builder.append_uint(name);
      client->send_event(builder);
    }
  }
}

void Display::destroy_global(std::uint32_t name) {
  remove_global(name);
  std::erase_if(globals_, [name](const Global& global) {
    return global.name == name;
  });
}

void Display::stop() { loop_.stop(); }

std::uint32_t Display::next_serial() { return ++next_serial_; }

}  // namespace zwayland::server
