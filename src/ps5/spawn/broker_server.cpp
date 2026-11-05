// SPDX-License-Identifier: GPL-3.0-or-later
#include "broker_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "broker_proto.h"
#include "prosperoweb/spawn/spawn.h"

namespace prosperoweb::spawn {

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr int kKillTimeoutMs = 2000;
constexpr int kPollMs = 200;
// A child gets SIGTERM and this long to leave before SIGKILL: a WebProcess
// that holds GPU memory from the system compositor must tell it Quit itself
// (src/ps5/glslim), or the memory stays taken until the console reboots.
constexpr int kTermGraceMs = 1000;

std::atomic<BrokerSpawnHook> g_hook{nullptr};
std::atomic<BrokerSelftestHook> g_selftest_hook{nullptr};

long long ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// 1 if pid is gone (reaped now, or not our child any more), 0 if alive.
// *status is the wait status when reaped now, -1 otherwise. Only tracked
// pids are waited for: waitpid(-1) would also take the ptrace stops the
// loader waits for while it sets up a new child.
int try_reap(int pid, int* status) {
  *status = -1;
  for (;;) {
    int st = 0;
    const pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      *status = st;
      return 1;
    }
    if (r == 0) {
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    return 1;  // ECHILD: already reaped
  }
}

std::string describe_status(int status) {
  char buf[32];
  if (status == -1) {
    std::snprintf(buf, sizeof(buf), "status unknown");
  } else if (WIFEXITED(status)) {
    std::snprintf(buf, sizeof(buf), "exit %d", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    std::snprintf(buf, sizeof(buf), "signal %d", WTERMSIG(status));
  } else {
    std::snprintf(buf, sizeof(buf), "status 0x%x", status);
  }
  return buf;
}

json error_reply(const std::string& stage, int err, const std::string& error) {
  return json{{"ok", false}, {"stage", stage}, {"errno", err}, {"error", error}};
}

class Server {
 public:
  explicit Server(const BrokerServerOptions& options) : opt_(options) {}

  int run(const std::atomic<bool>* stop);

 private:
  bool listen_all();
  bool listen_unix(const std::string& path);
  bool listen_tcp(int port);
  void reaper_main();
  void send_to_client(const json& msg);
  void drop_client(const char* why);
  void handle(const json& msg, std::vector<int>* fds);
  json do_spawn(const json& msg, std::vector<int>* fds);
  json do_terminate(const json& msg);
  size_t child_count();
  void kill_all_children();

  BrokerServerOptions opt_;
  struct Listener {
    int fd;
    bool tcp;
    std::string unix_path;  // removed on exit
  };
  std::vector<Listener> listeners_;

  std::mutex send_mu_;  // guards client_fd_ for writers (the reaper)
  int client_fd_ = -1;  // only the main thread changes it

  std::mutex children_mu_;
  std::vector<int> children_;

  std::mutex reaper_mu_;
  std::condition_variable reaper_cv_;
  bool reaper_stop_ = false;
};

bool Server::listen_unix(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    ps5_log_printf("broker: invalid socket path '%s'", path.c_str());
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  // A stale socket from a broker that died; a live one would have been
  // found by the client before it asked for a new broker.
  if (unlink(path.c_str()) == 0) {
    ps5_log_printf("broker: removed stale %s", path.c_str());
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    ps5_log_printf("broker: socket(AF_UNIX) for %s: errno %d (%s)", path.c_str(), errno,
                   std::strerror(errno));
    return false;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ps5_log_printf("broker: bind unix %s: errno %d (%s)", path.c_str(), errno,
                   std::strerror(errno));
    close(fd);
    return false;
  }
  // The UI runs as a big app with another identity; let it connect.
  if (chmod(path.c_str(), 0666) != 0) {
    ps5_log_printf("broker: chmod 0666 %s: %s", path.c_str(), std::strerror(errno));
  }
  if (listen(fd, 4) != 0) {
    ps5_log_printf("broker: listen unix %s: errno %d (%s)", path.c_str(), errno,
                   std::strerror(errno));
    close(fd);
    unlink(path.c_str());
    return false;
  }
  listeners_.push_back({fd, false, path});
  ps5_log_printf("broker: listening on unix %s", path.c_str());
  return true;
}

bool Server::listen_tcp(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    ps5_log_printf("broker: socket(AF_INET): errno %d (%s)", errno, std::strerror(errno));
    return false;
  }
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable off-console
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ps5_log_printf("broker: bind tcp 127.0.0.1:%d: errno %d (%s)", port, errno,
                   std::strerror(errno));
    close(fd);
    return false;
  }
  if (listen(fd, 4) != 0) {
    ps5_log_printf("broker: listen tcp 127.0.0.1:%d: errno %d (%s)", port, errno,
                   std::strerror(errno));
    close(fd);
    return false;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    port = ntohs(addr.sin_port);
  }
  if (opt_.bound_tcp_port) {
    opt_.bound_tcp_port->store(port);
  }
  listeners_.push_back({fd, true, ""});
  ps5_log_printf("broker: listening on tcp 127.0.0.1:%d", port);
  return true;
}

// The first AF_UNIX path that binds, plus loopback TCP. TCP is listened on
// even when AF_UNIX works: the UI (a big app) may not see the broker's
// filesystem namespace the same way, and then it falls back to TCP.
bool Server::listen_all() {
  for (const auto& path : opt_.socket_paths) {
    if (listen_unix(path)) {
      break;
    }
  }
  if (opt_.tcp_port >= 0) {
    listen_tcp(opt_.tcp_port);
  }
  if (opt_.bound_tcp_port && (listeners_.empty() || !listeners_.back().tcp)) {
    opt_.bound_tcp_port->store(-1);
  }
  if (listeners_.empty()) {
    ps5_log_line("broker: no AF_UNIX path and no TCP port could be listened on");
    return false;
  }
  return true;
}

size_t Server::child_count() {
  std::lock_guard<std::mutex> lock(children_mu_);
  return children_.size();
}

void Server::send_to_client(const json& msg) {
  std::lock_guard<std::mutex> lock(send_mu_);
  if (client_fd_ >= 0 && !broker_send_frame(client_fd_, msg)) {
    ps5_log_printf("broker: send to client failed: %s", std::strerror(errno));
  }
}

void Server::drop_client(const char* why) {
  std::lock_guard<std::mutex> lock(send_mu_);
  if (client_fd_ >= 0) {
    ps5_log_printf("broker: client disconnected (%s), %zu children alive", why,
                   [this] {
                     std::lock_guard<std::mutex> l(children_mu_);
                     return children_.size();
                   }());
    close(client_fd_);
    client_fd_ = -1;
  }
}

void Server::reaper_main() {
  std::unique_lock<std::mutex> lock(reaper_mu_);
  while (!reaper_stop_) {
    reaper_cv_.wait_for(lock, std::chrono::milliseconds(opt_.reap_interval_ms));
    if (reaper_stop_) {
      break;
    }
    std::vector<std::pair<int, int>> exited;
    {
      std::lock_guard<std::mutex> l(children_mu_);
      for (size_t i = 0; i < children_.size();) {
        int status;
        if (try_reap(children_[i], &status)) {
          exited.emplace_back(children_[i], status);
          children_[i] = children_.back();
          children_.pop_back();
        } else {
          i++;
        }
      }
    }
    for (const auto& [pid, status] : exited) {
      ps5_log_printf("broker: child pid=%d exited (%s)", pid, describe_status(status).c_str());
      send_to_client(json{{"ev", "exited"}, {"pid", pid}, {"status", status}});
    }
  }
}

json Server::do_spawn(const json& msg, std::vector<int>* fds) {
  BrokerSpawnRequest req;
  req.elf = broker_get_string(msg, "elf");
  const long long nfds = broker_get_int(msg, "nfds", 0);
  if (req.elf.empty() || !broker_get_strings(msg, "argv", &req.argv) || req.argv.empty() ||
      req.argv[0].empty() || !broker_get_strings(msg, "env", &req.env)) {
    return error_reply("request", EINVAL, "spawn needs elf, argv[0] and env");
  }
  if (nfds != static_cast<long long>(fds->size())) {
    return error_reply("request", EINVAL,
                       "nfds=" + std::to_string(nfds) + " but " +
                           std::to_string(fds->size()) + " descriptors arrived");
  }
  std::vector<int> src_fds;
  std::string why;
  switch (broker_get_src_fds(msg, &src_fds, &why)) {
    case BrokerSrcFds::kInvalid:
      return error_reply("request", EINVAL, why);
    case BrokerSrcFds::kAbsent:
      req.fds = *fds;  // ours now: they came as SCM_RIGHTS
      break;
    case BrokerSrcFds::kPresent: {
      if (!fds->empty()) {
        return error_reply("request", EINVAL, "both SCM_RIGHTS descriptors and src_fds");
      }
      const long long src_pid = broker_get_int(msg, "src_pid", 0);
      if (src_pid <= 0 || src_pid > 0x7fffffff) {
        return error_reply("request", EINVAL, "src_fds needs a positive src_pid");
      }
      req.src_pid = static_cast<int>(src_pid);
      req.fds = std::move(src_fds);
      std::string list;
      for (int fd : req.fds) {
        list += (list.empty() ? "" : ",") + std::to_string(fd);
      }
      ps5_log_printf("broker: role=%s takes fds [%s] from pid %d (rdup)", req.argv[0].c_str(),
                     list.c_str(), req.src_pid);
      break;
    }
  }

  const BrokerSpawnHook hook = g_hook.load();
  if (!hook) {
    return error_reply("hook", ENOSYS, "no spawn backend");
  }
  const auto t0 = Clock::now();
  const BrokerSpawnResult res = hook(req);
  if (res.pid <= 0) {
    ps5_log_printf("broker: role=%s FAILED at %s: errno %d (%s)", req.argv[0].c_str(),
                   res.stage.empty() ? "unknown stage" : res.stage.c_str(), res.err,
                   res.error.c_str());
    return error_reply(res.stage.empty() ? "unknown stage" : res.stage, res.err, res.error);
  }
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    children_.push_back(res.pid);
  }
  ps5_log_printf("broker: role=%s pid=%d spawned in %lldms", req.argv[0].c_str(), res.pid,
                 ms_since(t0));
  return json{{"ok", true}, {"pid", res.pid}};
}

json Server::do_terminate(const json& msg) {
  const long long pid = broker_get_int(msg, "pid", -1);
  if (pid <= 0) {
    return error_reply("request", EINVAL, "terminate needs a pid");
  }
  // An untracked pid was already reaped (and may have been recycled since),
  // so it must not be signalled.
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    auto it = std::find(children_.begin(), children_.end(), static_cast<int>(pid));
    if (it == children_.end()) {
      return json{{"ok", true}};
    }
    children_.erase(it);
  }
  kill(static_cast<pid_t>(pid), SIGTERM);  // best-effort: it may be a zombie already
  bool killed = false;
  for (int waited = 0; waited < kTermGraceMs + kKillTimeoutMs; waited += 10) {
    if (!killed && waited >= kTermGraceMs) {
      kill(static_cast<pid_t>(pid), SIGKILL);
      killed = true;
    }
    int status;
    if (try_reap(static_cast<int>(pid), &status)) {
      ps5_log_printf("broker: child pid=%lld terminated (%s)", pid,
                     describe_status(status).c_str());
      return json{{"ok", true}};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ps5_log_printf("broker: child pid=%lld did not exit after SIGKILL", pid);
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    children_.push_back(static_cast<int>(pid));  // the reaper collects it later
  }
  return error_reply("terminate", ETIMEDOUT, "child did not exit after SIGKILL");
}

void Server::handle(const json& msg, std::vector<int>* fds) {
  const std::string op = broker_get_string(msg, "op");
  json reply;
  if (op == "spawn") {
    reply = do_spawn(msg, fds);
  } else if (op == "terminate") {
    reply = do_terminate(msg);
  } else if (op == "ping") {
    reply = json{{"ok", true}, {"pid", static_cast<int>(getpid())}};
  } else if (op == "ipc-selftest") {
    const BrokerSelftestHook hook = g_selftest_hook.load();
    reply = hook ? hook(msg) : error_reply("ipc-selftest", ENOSYS, "no self-test in this broker");
  } else {
    reply = error_reply("request", EINVAL, "unknown op '" + op + "'");
  }
  // The child holds its own duplicates by now.
  for (int fd : *fds) {
    close(fd);
  }
  fds->clear();
  send_to_client(reply);
}

void Server::kill_all_children() {
  std::vector<int> pids;
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    pids.swap(children_);
  }
  for (int pid : pids) {
    kill(pid, SIGTERM);  // kTermGraceMs to leave, then SIGKILL
  }
  for (int pid : pids) {
    int status;
    bool killed = false;
    for (int waited = 0; waited < kTermGraceMs + kKillTimeoutMs && !try_reap(pid, &status); waited += 10) {
      if (!killed && waited >= kTermGraceMs) {
        kill(pid, SIGKILL);
        killed = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ps5_log_printf("broker: killed child pid=%d", pid);
  }
}

int Server::run(const std::atomic<bool>* stop) {
  if (!listen_all()) {
    return 1;
  }
  // The sockets are bound and listening now: a UI connect() already succeeds
  // (the kernel completes it from the listen backlog). Any slow preparation
  // runs here, so it never delays the bind and race the UI's first connect.
  if (opt_.on_listening) {
    opt_.on_listening();
  }
  ps5_log_printf("broker: ready, pid %d", static_cast<int>(getpid()));

  std::thread reaper([this] { reaper_main(); });
  auto alone_since = Clock::now();  // no client since then
  int rc = 0;

  for (;;) {
    if (stop && stop->load()) {
      ps5_log_line("broker: stop requested");
      break;
    }
    // Before the UI's first connection only: websrv started the broker and
    // the UI is about to connect. Once it has, the broker lives exactly as
    // long as that connection (see below).
    if (client_fd_ < 0) {
      const long long alone = ms_since(alone_since);
      if (alone >= opt_.idle_ms) {
        ps5_log_printf("broker: no client for %lldms, exiting", alone);
        break;
      }
    }

    std::vector<pollfd> pfd;
    for (const auto& l : listeners_) {
      pfd.push_back({l.fd, POLLIN, 0});
    }
    const size_t client_slot = pfd.size();
    if (client_fd_ >= 0) {
      pfd.push_back({client_fd_, POLLIN, 0});
    }
    const int n = poll(pfd.data(), pfd.size(), kPollMs);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ps5_log_printf("broker: poll: %s", std::strerror(errno));
      rc = 1;
      break;
    }

    bool accepted = false;
    for (size_t i = 0; i < listeners_.size() && !accepted; i++) {
      if (!(pfd[i].revents & POLLIN)) {
        continue;
      }
      const int fd = accept(listeners_[i].fd, nullptr, nullptr);
      if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
          ps5_log_printf("broker: accept: %s", std::strerror(errno));
        }
        continue;
      }
      if (listeners_[i].tcp) {
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      }
      if (client_fd_ >= 0) {
        drop_client("replaced by a new connection");
      }
      std::lock_guard<std::mutex> lock(send_mu_);
      client_fd_ = fd;
      ps5_log_printf("broker: client connected over %s, %zu children alive",
                     listeners_[i].tcp ? "tcp" : "unix", child_count());
      accepted = true;
    }
    if (accepted) {
      continue;
    }

    if (client_fd_ >= 0 && client_slot < pfd.size() &&
        (pfd[client_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
      json msg;
      std::vector<int> fds;
      const BrokerRecv r = broker_recv_frame(client_fd_, &msg, &fds);
      if (r == BrokerRecv::kOk) {
        handle(msg, &fds);
      } else {
        // The UI keeps its connection for its whole lifetime, so a closed
        // connection means the UI is gone (or gave up on us): its children
        // are orphans. Exiting now also means websrv, which reads the whole
        // ELF to start a broker, is asked at most once per UI run.
        drop_client(r == BrokerRecv::kClosed ? "closed" : std::strerror(errno));
        ps5_log_printf("broker: client disconnected, terminating %zu children and exiting",
                       child_count());
        break;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(reaper_mu_);
    reaper_stop_ = true;
  }
  reaper_cv_.notify_all();
  reaper.join();
  kill_all_children();
  drop_client("broker exiting");
  for (const auto& l : listeners_) {
    close(l.fd);
    if (!l.unix_path.empty()) {
      unlink(l.unix_path.c_str());
    }
  }
  ps5_log_printf("broker: exiting (%d)", rc);
  return rc;
}

}  // namespace

void broker_set_spawn_hook(BrokerSpawnHook hook) { g_hook.store(hook); }

void broker_set_selftest_hook(BrokerSelftestHook hook) { g_selftest_hook.store(hook); }

int broker_serve(const BrokerServerOptions& options, const std::atomic<bool>* stop) {
  Server server(options);
  return server.run(stop);
}

}  // namespace prosperoweb::spawn
