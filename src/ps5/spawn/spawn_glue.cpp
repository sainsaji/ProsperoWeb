// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WebKit spawn contract (webkit-port/include/ps5webkit/spawn.h) and the
// spawn broker role. PS5 target only.
//
// The UI runs as the foreground big app (the hbldr app websrv starts, or
// the native title PRWB00001), and rfork+ptrace+execve from a big app
// panics the console kernel. So by default (PROSPEROWEB_SPAWN_MODE=broker)
// the UI asks the spawn broker, a plain payload, to spawn for it. The UI
// starts the broker through elfldr (127.0.0.1:9021) or websrv's /hbldr
// (127.0.0.1:8080), PROSPEROWEB_BROKER_LOADER=auto|elfldr|websrv;
// PROSPEROWEB_SPAWN_MODE=direct keeps the in-process spawner (spawn.c) for
// plain-payload (elfldr) launches. See README.md and docs/native-title.md.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "broker_client.h"
#include "broker_proto.h"
#include "broker_server.h"
#include "cpuset_assign_ps5.h"
#include "ipc_selftest.h"
#include "spawn_backend.h"
#include "title_paths.h"

extern char** environ;

namespace {

using namespace prosperoweb::spawn;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

// After a loader starts the broker, the broker can take a second or more to
// bind 17771 (it then opens its profile tree). A single connect races that, so
// poll the connect for this whole window before giving up on a launched broker.
constexpr int kBrokerConnectWindowMs = 10000;
constexpr int kBrokerConnectIntervalMs = 200;
// After this many consecutive spawns without a broker, wait kBackoffMs before
// the next attempt: WebKit relaunches a failed process right away, forever,
// and every attempt asks websrv to start another broker.
constexpr int kBackoffAfterFailures = 2;
constexpr int kBackoffMs = 30000;
constexpr int kWebsrvTimeoutMs = 30000;  // websrv reads the ELF before it answers
constexpr int kSpawnTimeoutMs = 60000;
constexpr int kTerminateTimeoutMs = 5000;
// The first request after connecting waits out the broker's on_listening work
// (open_profile), which runs before it accepts: generous so a large profile
// tree does not look like an unreachable broker, but bounded so a wedged
// broker never holds the spawn thread (and g_mu) for too long.
constexpr int kPingTimeoutMs = 12000;
constexpr const char* kDefaultWebsrv = "127.0.0.1:8080";
constexpr const char* kDefaultElfldr = "127.0.0.1:9021";
constexpr int kElfldrTimeoutMs = 30000;  // elfldr reads the ELF before it answers
// etaHEN's bundled elfldr: when 9021 is refused, ask etaHEN's utility daemon to
// launch it (BREW_UTIL_LAUNCH_ELFLDR), then re-poll 9021. See docs/no-elfldr.md.
constexpr int kElfldrProbeMs = 1500;        // is 9021 already up? (keeps the happy path)
constexpr int kEtahenAskTimeoutMs = 2000;   // bounded: the util socket may be absent
constexpr int kElfldrRepollTotalMs = 5000;  // wait for elfldr to come up after the ask
constexpr int kElfldrRepollIntervalMs = 250;

long long ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

std::string env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && v[0] ? v : fallback;
}

const char* getenv_const(const char* name) { return std::getenv(name); }

std::vector<BrokerEndpoint> endpoints() {
  return broker_endpoints(std::getenv("PROSPEROWEB_SPAWN_SOCK"),
                          std::getenv("PROSPEROWEB_SPAWN_PORT"));
}

std::string describe(const std::vector<BrokerEndpoint>& eps) {
  std::string out;
  for (const auto& ep : eps) {
    out += (out.empty() ? "" : ", ") + ep.describe();
  }
  return out;
}

bool direct_mode() {
  static const bool direct = [] {
    const std::string mode = env_or("PROSPEROWEB_SPAWN_MODE", "broker");
    const bool d = mode == "direct";
    if (!d && mode != "broker") {
      ps5_log_printf("spawn: unknown PROSPEROWEB_SPAWN_MODE=%s, using broker", mode.c_str());
    }
    ps5_log_printf("spawn: mode %s", d ? "direct (in-process rfork)" : "broker");
    return d;
  }();
  return direct;
}

std::string describe_status(int status) {
  char buf[32];
  if (status < 0) {
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

void on_child_exit(int pid, int status) {
  ps5_log_printf("spawn: child pid=%d exited (%s)", pid, describe_status(status).c_str());
}

std::mutex g_mu;  // serialises spawns and terminates
std::unique_ptr<BrokerClient> g_client;
std::set<int> g_direct_pids;  // children of a PROSPEROWEB_SPAWN_FALLBACK=direct spawn
int g_broker_failures = 0;    // consecutive spawns that found no broker
Clock::time_point g_backoff_until{};
// Guards the connect race / respawn loop (g_mu): launch a broker only when none
// is already coming up, keep polling the one in flight, and cap relaunches.
BrokerLaunchPolicy g_launch_policy;

long long steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

std::string dir_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

// The broker's settings: it resolves the same endpoints as we do.
std::vector<std::string> broker_env(const std::string& self_path) {
  std::vector<std::string> env = {"PROSPEROWEB_ROLE=spawn-broker"};
  const char* sock = std::getenv("PROSPEROWEB_SPAWN_SOCK");
  if (sock && sock[0]) {
    env.push_back(std::string("PROSPEROWEB_SPAWN_SOCK=") + sock);
  }
  env.push_back("PROSPEROWEB_SPAWN_PORT=" +
                std::to_string(broker_tcp_port(std::getenv("PROSPEROWEB_SPAWN_PORT"))));
  env.push_back("PROSPEROWEB_SELF_PATH=" + self_path);
  return env;
}

// The ELF the loaders start as the broker: PROSPEROWEB_BROKER_PATH (the
// native title's small prosperoweb-broker.elf), else the multi-call ELF
// itself in its spawn-broker role. Either way the broker spawns the
// children from self_path.
std::string broker_elf(const std::string& self_path) {
  return env_or("PROSPEROWEB_BROKER_PATH", self_path.c_str());
}

// Asks websrv to start the broker ELF as a daemon payload in the broker role.
bool start_broker_via_websrv(const std::string& self_path, std::string* reason) {
  const std::string websrv = env_or("PROSPEROWEB_WEBSRV", kDefaultWebsrv);
  // argv[0] (also the process name) is the role; --role= and
  // PROSPEROWEB_ROLE say the same in case a loader rewrites argv[0].
  const std::string args = "spawn-broker --role=spawn-broker";
  std::string env;
  for (const auto& entry : broker_env(self_path)) {
    env += (env.empty() ? "" : " ") + websrv_escape_token(entry);
  }
  const std::string elf = broker_elf(self_path);
  const std::string target = websrv_broker_target(elf, dir_of(elf), args, env);
  const auto t0 = Clock::now();
  std::string error;
  const int code = http_get(websrv, target, kWebsrvTimeoutMs, &error);
  if (code < 0) {
    ps5_log_printf("websrv: GET http://%s%s -> failed: %s", websrv.c_str(), target.c_str(),
                   error.c_str());
    if (reason) {
      *reason = "websrv " + websrv + ": " + error;
    }
    return false;
  }
  ps5_log_printf("websrv: GET http://%s%s -> HTTP %d in %lldms", websrv.c_str(),
                 target.c_str(), code, ms_since(t0));
  if (code != 200 && reason) {
    *reason = "websrv " + websrv + ": HTTP " + std::to_string(code);
  }
  return code == 200;
}

// When 9021 is refused, asks etaHEN's utility daemon to launch its bundled
// elfldr (BREW_UTIL_LAUNCH_ELFLDR) and re-polls 9021. Best-effort and bounded:
// the command socket is AF_UNIX under /system_tmp, which a uid-1 sandboxed
// title usually cannot see (ENOENT) — on such a console this logs and falls
// through, it never fakes success. See docs/no-elfldr.md. Returns true only if
// 9021 actually came up. The happy path (9021 already answering) never gets
// here: the caller probes the port first.
bool ask_etahen_launch_elfldr(const std::string& elfldr) {
  const std::string sock = env_or("PROSPEROWEB_ETAHEN_UTIL_SOCK", kEtahenUtilSocketPath);
  ps5_log_printf("spawn: 9021 refused; asking etaHEN to start elfldr via util socket %s",
                 sock.c_str());
  std::string detail;
  std::string error;
  const int rc =
      etahen_send(sock, etahen_launch_elfldr_request(), kEtahenAskTimeoutMs, &detail, &error);
  if (rc < 0) {
    ps5_log_printf("spawn: etaHEN util socket %s unreachable (%s); cannot ask it to start elfldr",
                   sock.c_str(), error.c_str());
    return false;
  }
  if (rc != 0) {
    ps5_log_printf("spawn: etaHEN declined to start elfldr%s%s", detail.empty() ? "" : ": ",
                   detail.c_str());
    // It still may have launched; re-poll regardless.
  } else {
    ps5_log_line("spawn: etaHEN accepted the launch-elfldr request");
  }
  const std::string elfldr_copy = elfldr;
  const long long up = poll_until_true(
      [&elfldr_copy] {
        std::string e;
        return tcp_port_open(elfldr_copy, kElfldrProbeMs, &e);
      },
      kElfldrRepollTotalMs, kElfldrRepollIntervalMs);
  if (up >= 0) {
    ps5_log_printf("spawn: elfldr up on 9021 after %lldms", up);
    return true;
  }
  ps5_log_printf("spawn: elfldr did not come up on 9021 within %dms after asking etaHEN",
                 kElfldrRepollTotalMs);
  return false;
}

// Sends the elfldr load request on a detached thread and never blocks the
// caller on it. Why: the URI already asks for pipe=0 (stdio detached), and
// ps5-payload-dev's standalone elfldr honours that and returns in ~1s. But
// etaHEN's *bundled* elfldr ignores pipe=0 and hands the broker the request
// socket as its stdio, so the HTTP call stays open until the broker process
// exits (~20s on its no-client timeout). Blocking on that reply means the UI
// would only start polling 17771 after the broker had already died (run63).
// So we fire-and-forget: the broker appearing on 17771 is the success signal,
// not this reply. The thread captures everything by value, touches no shared
// state, and ends when elfldr closes or the recv times out.
void fire_elfldr_async(const std::string& elfldr, const std::string& line) {
  std::thread([elfldr, line] {
    const auto t0 = Clock::now();
    std::string reply;
    std::string error;
    const int rc = elfldr_send(elfldr, line, kElfldrTimeoutMs, &reply, &error);
    if (rc < 0) {
      ps5_log_printf("elfldr(async): %s <- %s -> failed: %s", elfldr.c_str(), line.c_str(),
                     error.c_str());
    } else {
      ps5_log_printf("elfldr(async): %s <- %s -> %s in %lldms (reply ignored; 17771 is the signal)",
                     elfldr.c_str(), line.c_str(),
                     rc == 0 ? (reply.empty() ? "accepted" : reply.c_str()) : reply.c_str(),
                     ms_since(t0));
    }
  }).detach();
}

// Asks elfldr to load "<self_path>" from the console's file system as a
// payload in the broker role. elfldr passes no environment, so the settings
// travel as --env= arguments (main() exports them). The request is issued
// asynchronously (see fire_elfldr_async): success is decided by the broker
// coming up on 17771 (ensure_broker's poll-connect), not by this call. We
// return true as soon as 9021 is reachable and the request is fired.
bool start_broker_via_elfldr(const std::string& self_path, std::string* reason) {
  const std::string elfldr = env_or("PROSPEROWEB_ELFLDR", kDefaultElfldr);
  // Is 9021 up? If refused, give etaHEN a chance to bring up its bundled
  // elfldr first (bounded; a no-op on the happy path, which answers at once).
  std::string probe_err;
  bool up = tcp_port_open(elfldr, kElfldrProbeMs, &probe_err);
  if (!up && env_or("PROSPEROWEB_ETAHEN_ASK", "1") != "0") {
    up = ask_etahen_launch_elfldr(elfldr);
  }
  if (!up) {
    ps5_log_printf("elfldr: %s not reachable: %s", elfldr.c_str(), probe_err.c_str());
    if (reason) {
      *reason = "elfldr " + elfldr + ": " + probe_err;
    }
    return false;
  }
  const std::string line =
      elfldr_payload_line(broker_elf(self_path), elfldr_broker_args(broker_env(self_path)));
  ps5_log_printf("elfldr: %s issuing load (async, not waiting for the reply) <- %s", elfldr.c_str(),
                 line.c_str());
  fire_elfldr_async(elfldr, line);
  return true;
}

// Starts the broker with the loaders PROSPEROWEB_BROKER_LOADER allows.
// "auto" (the default) tries both: elfldr first in the native title (no
// websrv needed), websrv first when websrv's tile started the UI (it is
// running then). main() sets PROSPEROWEB_TITLE_UI_ROOT only in the title.
bool start_broker(const std::string& self_path, std::string* reason) {
  const std::string loader = env_or("PROSPEROWEB_BROKER_LOADER", "auto");
  std::vector<std::string> order;
  if (loader == "elfldr" || loader == "websrv") {
    order = {loader};
  } else {
    if (loader != "auto") {
      ps5_log_printf("spawn: unknown PROSPEROWEB_BROKER_LOADER=%s, using auto", loader.c_str());
    }
    const bool native = !env_or("PROSPEROWEB_TITLE_UI_ROOT", "").empty();
    order = native ? std::vector<std::string>{"elfldr", "websrv"}
                   : std::vector<std::string>{"websrv", "elfldr"};
  }
  for (size_t i = 0; i < order.size(); i++) {
    ps5_log_printf("spawn: starting the broker %s (children from %s) via %s (loader %s, "
                   "attempt %zu/%zu)",
                   broker_elf(self_path).c_str(), self_path.c_str(), order[i].c_str(),
                   loader.c_str(), i + 1, order.size());
    std::string attempt_reason;
    const bool ok = order[i] == "elfldr"
                        ? start_broker_via_elfldr(self_path, &attempt_reason)
                        : start_broker_via_websrv(self_path, &attempt_reason);
    if (ok) {
      return true;
    }
    if (reason && !attempt_reason.empty()) {
      *reason += (reason->empty() ? "" : "; ") + attempt_reason;
    }
  }
  ps5_log_line("spawn: no loader started the broker (elfldr on 127.0.0.1:9021 or websrv on "
               "127.0.0.1:8080 must be running)");
  return false;
}

std::atomic<ps5_spawn_progress_fn> g_progress{nullptr};
std::atomic<void*> g_progress_data{nullptr};

// Media control-socket delivery (ps5_spawn_set_media_control). Both set turns
// on the per-WebProcess control sockets for audio and video.
std::atomic<ps5_media_control_fn> g_media_audio_cb{nullptr};
std::atomic<ps5_media_control_fn> g_media_video_cb{nullptr};
std::atomic<void*> g_media_cb_data{nullptr};

// GPU broker endpoint (ps5_spawn_set_gpu_endpoint): one waiting for the next
// WebProcess.
std::mutex g_gpu_mu;
std::condition_variable g_gpu_cv;
int g_gpu_fd = -1;
int g_gpu_cookie = -1;
ps5_gpu_client_fn g_gpu_cb = nullptr;
void* g_gpu_cb_data = nullptr;
int g_gpu_wait_ms = 0;

// The waiting endpoint, or -1 (after waiting up to g_gpu_wait_ms when the
// Mesa flag is set). The caller owns the fd.
int take_gpu_endpoint(int* cookie) {
  std::unique_lock<std::mutex> lock(g_gpu_mu);
  if (g_gpu_fd < 0 && g_gpu_wait_ms > 0) {
    struct stat st;
    if (stat("/data/prosperoweb/mesa", &st) == 0) {
      g_gpu_cv.wait_for(lock, std::chrono::milliseconds(g_gpu_wait_ms),
                        [] { return g_gpu_fd >= 0; });
    }
  }
  const int fd = g_gpu_fd;
  *cookie = g_gpu_cookie;
  g_gpu_fd = -1;
  g_gpu_cookie = -1;
  return fd;
}

void give_back_gpu_endpoint(int fd, int cookie) {
  std::lock_guard<std::mutex> lock(g_gpu_mu);
  if (g_gpu_fd >= 0) {
    close(fd);  // a newer one arrived meanwhile
    return;
  }
  g_gpu_fd = fd;
  g_gpu_cookie = cookie;
}

// One AF_UNIX SOCK_SEQPACKET control socket handed to a WebProcess: the UI
// end goes to a media server, the child end is injected as an inherited fd.
struct ControlPair {
  int ui_fd = -1;
  int child_fd = -1;
};

void report_progress(const char* event) {
  if (const ps5_spawn_progress_fn fn = g_progress.load(std::memory_order_acquire)) {
    fn(event, g_progress_data.load(std::memory_order_acquire));
  }
}

// Tells the UI no ELF loader could be reached, with a one-line reason, so it
// can replace the stuck progress bar with a clear message (ps5_app.cpp). The
// reason travels in the progress event after "no-loader:".
void report_loader_missing(const std::string& reason) {
  report_progress(("no-loader:" + reason).c_str());
}

// Tells the UI a loader did start the broker but the UI could not connect to
// it on 17771. Distinct from "no-loader": the console does have a working
// loader, so the UI keeps retrying rather than blaming a missing one.
void report_broker_unreachable(const std::string& reason) {
  report_progress(("broker-unreachable:" + reason).c_str());
}

// Caller holds g_mu. Connects to the broker, starting it if needed.
//
// The hard part is the connect race: a loader starts the broker, but the broker
// takes a second or more to bind 17771. A single connect attempt loses that
// race and returns "Connection refused"; relaunching the broker on every such
// failure spawns an endless series of brokers that each self-exit with "no
// client". So: launch a broker only when none is already in flight (the launch
// policy), then poll-connect for a bounded window, and cap relaunches.
bool ensure_broker(const std::string& self_path, bool may_start) {
  if (g_client && g_client->connected()) {
    return true;
  }
  const std::vector<BrokerEndpoint> eps = endpoints();
  if (!g_client) {
    g_client = std::make_unique<BrokerClient>(eps, on_child_exit);
  }
  const auto t0 = Clock::now();
  ps5_log_printf("spawn: broker connecting, trying %s", describe(eps).c_str());
  std::string failures;
  if (!g_client->connect_once(&failures)) {
    if (!may_start) {
      ps5_log_printf("spawn: broker not reachable: %s", failures.c_str());
      return false;
    }
    ps5_log_printf("spawn: broker not running (%s)", failures.c_str());
    const BrokerLaunchPolicy::Action action = g_launch_policy.decide(steady_ms());
    if (action == BrokerLaunchPolicy::kGiveUp) {
      ps5_log_printf("spawn: launched %d brokers, none reachable on 17771; giving up for now",
                     g_launch_policy.launches_done);
      report_broker_unreachable("started the loader " +
                                std::to_string(g_launch_policy.launches_done) +
                                "x but could not connect on 127.0.0.1:17771");
      return false;
    }
    if (action == BrokerLaunchPolicy::kLaunch) {
      std::string reason;
      if (!start_broker(self_path, &reason)) {
        report_loader_missing(reason.empty() ? failures : reason);
        return false;
      }
      g_launch_policy.on_launched(steady_ms());
      ps5_log_printf("spawn: broker launch #%d accepted; polling 127.0.0.1:17771 up to %dms",
                     g_launch_policy.launches_done, kBrokerConnectWindowMs);
    } else {
      ps5_log_printf("spawn: a broker is already coming up (launch #%d); polling 127.0.0.1:17771 "
                     "up to %dms without relaunching",
                     g_launch_policy.launches_done, kBrokerConnectWindowMs);
    }
    if (!g_client->connect_wait(kBrokerConnectWindowMs, kBrokerConnectIntervalMs, &failures)) {
      ps5_log_printf("spawn: broker launched but 127.0.0.1:17771 connect failed after %dms (%s)",
                     kBrokerConnectWindowMs, failures.c_str());
      // Not a missing loader: keep the splash and let the next attempt (or the
      // background retry) poll again; it stops only once relaunches run out.
      return false;
    }
  }
  g_launch_policy.on_connected();
  const BrokerEndpoint ep = g_client->endpoint();
  ps5_log_printf("spawn: broker connected via %s%s", ep.describe().c_str(),
                 ep.tcp ? " (descriptors passed by rdup from this pid)" : "");
  json reply;
  if (!g_client->request(json{{"op", "ping"}}, nullptr, 0, &reply, kPingTimeoutMs) ||
      !broker_get_bool(reply, "ok", false)) {
    ps5_log_printf("spawn: broker did not answer ping (%s)", std::strerror(errno));
    return false;
  }
  ps5_log_printf("spawn: broker ready pid=%lld in %lldms", broker_get_int(reply, "pid", -1),
                 ms_since(t0));
  // Once per UI: the IPC primitives across the title's sandbox
  // (ipc_selftest.h). Always in the native title, opt-in elsewhere.
  static bool selftest_done = false;
  if (!selftest_done && (!env_or("PROSPEROWEB_TITLE_UI_ROOT", "").empty() ||
                         env_or("PROSPEROWEB_IPC_SELFTEST", "") == "1")) {
    selftest_done = true;
    ipc_selftest_ui(*g_client);
  }
  report_progress("broker");
  return true;
}

// Waits out a back-off set by earlier failures, without holding g_mu (a
// terminate must not wait for it).
void wait_for_backoff(const char* role) {
  long long left;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    left = std::chrono::duration_cast<std::chrono::milliseconds>(g_backoff_until -
                                                                 Clock::now())
               .count();
  }
  if (left > 0) {
    ps5_log_printf("spawn: role=%s waiting %lldms (broker back-off)", role, left);
    std::this_thread::sleep_for(std::chrono::milliseconds(left));
  }
}

// Caller holds g_mu.
void note_broker_result(bool ok) {
  if (ok) {
    if (g_broker_failures >= kBackoffAfterFailures) {
      ps5_log_line("spawn: broker available again, back-off cleared");
    }
    g_broker_failures = 0;
    return;
  }
  g_broker_failures++;
  if (g_broker_failures >= kBackoffAfterFailures) {
    g_backoff_until = Clock::now() + std::chrono::milliseconds(kBackoffMs);
    ps5_log_printf("spawn: no broker %d times in a row, backing off %dms before the next "
                   "attempt",
                   g_broker_failures, kBackoffMs);
  }
}

int spawn_via_broker(const char* const* argv, const int* fds, int nfds) {
  const char* role = argv[0];
  char path[1024];
  if (ps5_spawn_resolve_self_path(nullptr, getenv_const, path, sizeof(path)) != 0) {
    ps5_log_printf("spawn: role=%s FAILED: cannot resolve the self ELF path", role);
    return -1;
  }
  if (nfds < 0 || nfds > kBrokerMaxFds) {
    ps5_log_printf("spawn: role=%s FAILED: invalid fd table (nfds=%d)", role, nfds);
    return -1;
  }

  json msg{{"op", "spawn"}, {"elf", path}};
  json jargv = json::array();
  for (int i = 0; argv[i]; i++) {
    jargv.push_back(argv[i]);
  }
  // The native title reads its files as /app0, which the children (plain
  // payloads) do not have: hand them the folder as they see it
  // (title_paths.h; main() exports both roots).
  // PROSPEROWEB_UI_ONLY_ENV names variables that are the UI's own (the
  // native title runs JSC without JIT); the children keep their defaults.
  const std::string ui_only = " " + env_or("PROSPEROWEB_UI_ONLY_ENV", "") + " ";
  std::vector<std::string> env;
  for (char** e = environ; e && *e; e++) {
    const std::string entry = *e;
    const std::string name = entry.substr(0, entry.find('='));
    if (name == "PROSPEROWEB_UI_ONLY_ENV" || ui_only.find(" " + name + " ") != std::string::npos) {
      continue;
    }
    env.push_back(entry);
  }
  const std::string ui_root = env_or("PROSPEROWEB_TITLE_UI_ROOT", "");
  const std::string child_root = env_or("PROSPEROWEB_TITLE_CHILD_ROOT", "");
  if (!ui_root.empty() && !child_root.empty() && ui_root != child_root) {
    env = rebase_env(env, ui_root, child_root);
  }
  // Media control sockets for a WebProcess under the media-category sandbox,
  // where the UI cannot listen() (docs/native-title.md). One AF_UNIX
  // SOCK_SEQPACKET socketpair per service: the child end is injected as an
  // inherited fd (child fd 3+i, named to the child through the environment),
  // the UI end goes to the audio/video server. When socketpair fails the
  // WebProcess keeps the loopback-TCP path (clean degradation).
  const ps5_media_control_fn audio_cb = g_media_audio_cb.load();
  const ps5_media_control_fn video_cb = g_media_video_cb.load();
  std::vector<ControlPair> control;
  std::vector<int> all_fds(fds, fds + nfds);
  if (audio_cb && video_cb && std::strcmp(role, "WebProcess") == 0 && nfds + 2 <= kBrokerMaxFds) {
    int a[2] = {-1, -1};
    int v[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, a) == 0 &&
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, v) == 0) {
      const int audio_child = 3 + nfds;
      const int video_child = 3 + nfds + 1;
      env.push_back("PROSPEROWEB_AUDIO_CONTROL_FD=" + std::to_string(audio_child));
      env.push_back("PROSPEROWEB_VIDEO_CONTROL_FD=" + std::to_string(video_child));
      control.push_back({a[0], a[1]});
      control.push_back({v[0], v[1]});
      all_fds.push_back(a[1]);
      all_fds.push_back(v[1]);
      ps5_log_printf("spawn: role=%s media control sockets: audio child fd %d, video child fd %d",
                     role, audio_child, video_child);
    } else {
      if (a[0] >= 0) {
        close(a[0]);
        close(a[1]);
      }
      ps5_log_printf("spawn: role=%s media control socketpair failed (%s); using tcp", role,
                     std::strerror(errno));
    }
  }

  // EVO's GPU broker endpoint (#237): the WebProcess's Mesa binds it.
  int gpu_cookie = -1;
  int gpu_fd = -1;
  if (std::strcmp(role, "WebProcess") == 0 &&
      static_cast<int>(all_fds.size()) < kBrokerMaxFds) {
    gpu_fd = take_gpu_endpoint(&gpu_cookie);
    if (gpu_fd >= 0) {
      const int child = 3 + static_cast<int>(all_fds.size());
      env.push_back("PROSPEROWEB_GPU_FD=" + std::to_string(child));
      all_fds.push_back(gpu_fd);
      ps5_log_printf("spawn: role=%s GPU endpoint (cookie %d) at child fd %d", role, gpu_cookie,
                     child);
    }
  }
  auto gpu_spawned = [&](int pid) {
    if (gpu_fd < 0) {
      return;
    }
    close(gpu_fd);
    gpu_fd = -1;
    ps5_gpu_client_fn cb;
    void* data;
    {
      std::lock_guard<std::mutex> lock(g_gpu_mu);
      cb = g_gpu_cb;
      data = g_gpu_cb_data;
    }
    if (cb) {
      cb(gpu_cookie, pid, data);
    }
  };
  auto gpu_failed = [&] {
    if (gpu_fd >= 0) {
      give_back_gpu_endpoint(gpu_fd, gpu_cookie);
      gpu_fd = -1;
    }
  };

  json jenv = json::array();
  for (const auto& entry : env) {
    jenv.push_back(entry);
  }
  msg["argv"] = std::move(jargv);
  msg["env"] = std::move(jenv);

  void* const cb_data = g_media_cb_data.load();
  auto deliver_control = [&] {
    if (control.size() != 2) {
      return;
    }
    close(control[0].child_fd);
    close(control[1].child_fd);
    audio_cb(control[0].ui_fd, cb_data);
    video_cb(control[1].ui_fd, cb_data);
    control.clear();
  };
  auto discard_control = [&] {
    for (const auto& c : control) {
      if (c.ui_fd >= 0) {
        close(c.ui_fd);
      }
      if (c.child_fd >= 0) {
        close(c.child_fd);
      }
    }
    control.clear();
  };

  // With the direct fallback a missing broker costs no websrv round-trips
  // after the first, so there is nothing to back off from.
  const bool fallback_direct = env_or("PROSPEROWEB_SPAWN_FALLBACK", "") == "direct";
  if (!fallback_direct) {
    wait_for_backoff(role);
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const bool have_broker = ensure_broker(path, /*may_start=*/true);
  note_broker_result(have_broker || fallback_direct);
  if (have_broker) {
    // Over AF_UNIX the descriptors travel as SCM_RIGHTS. Over TCP they
    // cannot: the frame names them and the broker rdups them out of this
    // process. Either way the caller keeps its copies open until request()
    // returns (the WebKit launcher closes the client end only after
    // ps5_spawn_self() returns), so they still exist when the broker injects
    // them into the child.
    const bool tcp = g_client->endpoint().tcp;
    int send_n = static_cast<int>(all_fds.size());
    if (tcp) {
      msg["nfds"] = 0;
      msg["src_pid"] = static_cast<int>(getpid());
      msg["src_fds"] = json(all_fds);
      send_n = 0;
    } else {
      msg["nfds"] = static_cast<int>(all_fds.size());
    }
    const auto t0 = Clock::now();
    json reply;
    if (g_client->request(msg, all_fds.data(), send_n, &reply, kSpawnTimeoutMs)) {
      if (broker_get_bool(reply, "ok", false)) {
        const int pid = static_cast<int>(broker_get_int(reply, "pid", -1));
        deliver_control();
        gpu_spawned(pid);
        ps5_log_printf("spawn: role=%s pid=%d via broker (%s) in %lldms", role, pid,
                       tcp ? "tcp, rdup" : "unix, SCM_RIGHTS", ms_since(t0));
        return pid;
      }
      discard_control();
      gpu_failed();
      const int err = static_cast<int>(broker_get_int(reply, "errno", 0));
      ps5_log_printf("spawn: role=%s FAILED via broker at %s: errno %d (%s) %s", role,
                     broker_get_string(reply, "stage", "?").c_str(), err,
                     err ? std::strerror(err) : "no errno",
                     broker_get_string(reply, "error").c_str());
      return -1;
    }
    ps5_log_printf("spawn: role=%s: broker connection lost (%s)", role, std::strerror(errno));
  }

  if (fallback_direct) {
    ps5_log_printf("spawn: role=%s: no broker, PROSPEROWEB_SPAWN_FALLBACK=direct, "
                   "spawning in-process",
                   role);
    const int pid = ps5_spawn_direct(argv, all_fds.data(), static_cast<int>(all_fds.size()));
    if (pid > 0) {
      g_direct_pids.insert(pid);
      deliver_control();
      gpu_spawned(pid);
    } else {
      discard_control();
      gpu_failed();
    }
    return pid;
  }
  discard_control();
  gpu_failed();
  ps5_log_printf("spawn: role=%s FAILED: no spawn broker (in-process spawning from the "
                 "big app panics the console; PROSPEROWEB_SPAWN_FALLBACK=direct forces it)",
                 role);
  return -1;
}

int terminate_via_broker(int pid) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_direct_pids.erase(pid)) {
    return ps5_terminate_direct(pid);
  }
  if (!ensure_broker("", /*may_start=*/false)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: no broker", pid);
    return -1;
  }
  json reply;
  if (!g_client->request(json{{"op", "terminate"}, {"pid", pid}}, nullptr, 0, &reply,
                         kTerminateTimeoutMs)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: broker connection lost (%s)", pid,
                   std::strerror(errno));
    return -1;
  }
  if (!broker_get_bool(reply, "ok", false)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: %s", pid,
                   broker_get_string(reply, "error", "?").c_str());
    return -1;
  }
  return 0;
}

// --- Broker role ------------------------------------------------------------

// Opens the profile to every process: the native title's UI runs as an
// unprivileged user (uid 1, console run 28) and could not write files the
// root payloads had created there (0755/0644). Directories get rwx and files
// rw for everyone; symbolic links are not followed. Returns the number of
// entries changed; *failed counts chmod failures.
int open_profile_tree(const std::string& dir, int depth, int* failed, int* seen) {
  int changed = 0;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    return 0;
  }
  while (dirent* e = readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..") || *seen > 50000) {
      continue;
    }
    (*seen)++;
    const std::string path = dir + "/" + e->d_name;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) {
      continue;
    }
    const mode_t want = S_ISDIR(st.st_mode) ? (st.st_mode | 0777) : (st.st_mode | 0666);
    if ((st.st_mode & 07777) != (want & 07777)) {
      if (chmod(path.c_str(), want & 07777) == 0) {
        changed++;
      } else {
        (*failed)++;
      }
    }
    if (S_ISDIR(st.st_mode) && depth < 16) {
      changed += open_profile_tree(path, depth + 1, failed, seen);
    }
  }
  closedir(d);
  return changed;
}

void open_profile(const char* root) {
  const auto t0 = Clock::now();
  mkdir(root, 0777);
  int failed = 0;
  int seen = 0;
  int changed = chmod(root, 0777) == 0 ? 1 : 0;
  changed += open_profile_tree(root, 0, &failed, &seen);
  ps5_log_printf("broker: profile %s opened to all users: %d entries changed, %d failed, %d "
                 "seen, %lldms",
                 root, changed, failed, seen, ms_since(t0));
}

std::atomic<bool> g_stop{false};

void on_sigterm(int) { g_stop.store(true); }

std::vector<char*> c_strings(const std::vector<std::string>& in) {
  std::vector<char*> out;
  out.reserve(in.size() + 1);
  for (const auto& s : in) {
    out.push_back(const_cast<char*>(s.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

BrokerSpawnResult elfldr_hook(const BrokerSpawnRequest& req) {
  std::vector<char*> argv = c_strings(req.argv);
  std::vector<char*> envp = c_strings(req.env);
  ps5_spawn_failure failure{};
  BrokerSpawnResult res;
  res.pid = ps5_spawn_elfldr(req.elf.c_str(), argv.data(), envp.data(), req.fds.data(),
                             static_cast<int>(req.fds.size()), req.src_pid, &failure);
  if (res.pid <= 0) {
    res.pid = -1;
    res.stage = failure.stage ? failure.stage : "unknown stage";
    res.err = failure.err;
    res.error = failure.err ? std::strerror(failure.err) : "";
  }
  return res;
}

}  // namespace

extern "C" int ps5_spawn_self(const char* const* argv, const int* fds, int nfds) {
  if (!argv || !argv[0]) {
    ps5_log_line("spawn: called without a role");
    return -1;
  }
  const int pid =
      direct_mode() ? ps5_spawn_direct(argv, fds, nfds) : spawn_via_broker(argv, fds, nfds);
  if (pid > 0) {
    report_progress(argv[0]);
  }
  return pid;
}

extern "C" void ps5_spawn_set_progress(ps5_spawn_progress_fn fn, void* data) {
  g_progress_data.store(data, std::memory_order_release);
  g_progress.store(fn, std::memory_order_release);
}

extern "C" void ps5_spawn_set_media_control(ps5_media_control_fn audio, ps5_media_control_fn video,
                                            void* data) {
  g_media_cb_data.store(data, std::memory_order_release);
  g_media_audio_cb.store(audio, std::memory_order_release);
  g_media_video_cb.store(video, std::memory_order_release);
}

extern "C" void ps5_spawn_set_gpu_endpoint(int fd, int cookie) {
  {
    std::lock_guard<std::mutex> lock(g_gpu_mu);
    if (g_gpu_fd >= 0) {
      close(g_gpu_fd);
    }
    g_gpu_fd = fd;
    g_gpu_cookie = cookie;
  }
  g_gpu_cv.notify_all();
}

extern "C" void ps5_spawn_set_gpu_client_cb(ps5_gpu_client_fn fn, void* data, int wait_ms) {
  std::lock_guard<std::mutex> lock(g_gpu_mu);
  g_gpu_cb = fn;
  g_gpu_cb_data = data;
  g_gpu_wait_ms = wait_ms;
}

extern "C" int ps5_terminate_process(int pid) {
  if (pid <= 0) {
    return -1;
  }
  return direct_mode() ? ps5_terminate_direct(pid) : terminate_via_broker(pid);
}

extern "C" int ps5_spawn_try_bring_up_broker(void) {
  if (direct_mode()) {
    return 1;  // no broker needed; nothing to bring up
  }
  char path[1024];
  if (ps5_spawn_resolve_self_path(nullptr, getenv_const, path, sizeof(path)) != 0) {
    ps5_log_line("spawn: retry: cannot resolve the self ELF path");
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  return ensure_broker(path, /*may_start=*/true) ? 1 : 0;
}

extern "C" int ps5_spawn_broker_main(void) {
  // Files the broker and its children create stay usable by the UI.
  umask(0);
  // websrv hands the daemon the write end of a pipe as stdout/stderr and
  // closes the read end when the request does not ask for pipe=1: point
  // stdio at /dev/null (log.txt and klog carry the log), then restore
  // SIGPIPE (main() ignored it) so the children start as before.
  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
      close(null_fd);
    }
  }
  std::signal(SIGPIPE, SIG_DFL);

  // The broker itself started in the system cpuset; widen it to the root set
  // (no UI process to join yet). Best-effort, non-fatal.
  ps5_cpuset_assign_child(getpid(), 0);

  struct sigaction sa {};
  sa.sa_handler = on_sigterm;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);

  BrokerServerOptions options;
  options.socket_paths = broker_socket_paths(std::getenv("PROSPEROWEB_SPAWN_SOCK"));
  for (const auto& path : options.socket_paths) {
    mkdir(dir_of(path).c_str(), 0777);  // best-effort; bind reports the real error
  }
  options.tcp_port = broker_tcp_port(std::getenv("PROSPEROWEB_SPAWN_PORT"));
  // Open the profile only after the sockets listen, so the UI's connect()
  // lands in the backlog immediately instead of racing a not-yet-bound socket.
  options.on_listening = [] { open_profile("/data/prosperoweb"); };
  broker_set_spawn_hook(elfldr_hook);
  broker_set_selftest_hook(ipc_selftest_broker);
  return broker_serve(options, &g_stop);
}
