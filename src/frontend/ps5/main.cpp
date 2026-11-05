// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-call entry point. The browser ships as one static ELF; the role is
// selected by argv[0] (set by ps5_spawn_self in the launcher), then by a
// --role=<name> argument, then by the PROSPEROWEB_ROLE environment variable:
//
//   "WebProcess"     -> ps5_webkit_web_process_main    (WebKit static lib)
//   "NetworkProcess" -> ps5_webkit_network_process_main (WebKit static lib)
//   "spawn-broker"   -> ps5_spawn_broker_main          (src/ps5/spawn), the
//                       plain payload websrv starts to spawn the two above
//   "evo-host"       -> EvoHost (evo_host.h): the engine of EVO Player's
//                       Browser section, no window; also chosen by --evo-host
//   anything else    -> the UI process (Ps5App)
//
// --env=NAME=VALUE arguments are exported first: elfldr, which starts the
// broker for the native title, passes arguments but no environment.
//
// The same sources are also linked as the native title's eboot.bin
// (PROSPEROWEB_NATIVE_TITLE, docs/native-title.md). That build is only ever
// the UI: the broker and the children stay the payload ELF shipped next to
// it (<title>/prosperoweb.elf).

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "prosperoweb/core/legacy_profile.h"
#include "prosperoweb/spawn/spawn.h"
#include "ps5webkit/entry.h"
#include "evo_host.h"
#include "ps5_app.h"
#include "ps5_cpu_affinity.h"
#include "ps5_cpu_monitor.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"
#include "ipc_selftest.h"

// src/ps5/glslim (payload build only; the native title has no kernel access):
// weak, so a build without it links.
extern "C" int ps5_glslim_selftest(void) __attribute__((weak));

#if PROSPEROWEB_NATIVE_TITLE
#include <WebKit/WKRunLoop.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <pthread.h>

#include "native/native_title.h"
#include "ps5_hwdec.h"
#include "title_paths.h"
#endif

namespace {

#if !PROSPEROWEB_NATIVE_TITLE
// Returns the basename of a path (argv[0] may be absolute).
const char* basename_of(const char* path) {
  if (!path) {
    return "";
  }
  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

constexpr const char* kBrokerRole = "spawn-broker";
constexpr const char* kEvoHostRole = "evo-host";

// Resolves the process role from argv[0], then --evo-host or --role=, then
// PROSPEROWEB_ROLE.
std::string resolve_role(int argc, char** argv) {
  if (argc > 0 && argv[0]) {
    std::string role = basename_of(argv[0]);
    if (role == "WebProcess" || role == "NetworkProcess" || role == kBrokerRole ||
        role == kEvoHostRole) {
      return role;
    }
  }
  for (int i = 0; i < argc; i++) {
    if (argv[i] && std::strcmp(argv[i], "--evo-host") == 0) {
      return kEvoHostRole;
    }
    if (argv[i] && std::strncmp(argv[i], "--role=", 7) == 0 && argv[i][7]) {
      return argv[i] + 7;
    }
  }
  if (const char* env = std::getenv("PROSPEROWEB_ROLE")) {
    return env;
  }
  return "UI";
}

// The UI's own ELF is what the spawner re-executes for WebProcess and
// NetworkProcess. When the loader passed an absolute *.elf path as argv[0],
// export it, so the children run exactly the binary the UI was started from
// (not a stale copy at the default install path). PROSPEROWEB_SELF_PATH set
// by the user wins; otherwise the spawner uses /data/prosperoweb/prosperoweb.elf.
void export_self_path(int argc, char** argv) {
  if (argc < 1 || !argv[0] || argv[0][0] != '/') {
    return;
  }
  const std::string path = argv[0];
  const std::string suffix = ".elf";
  if (path.size() <= suffix.size() ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0 ||
      access(path.c_str(), R_OK) != 0) {
    return;
  }
  setenv("PROSPEROWEB_SELF_PATH", path.c_str(), /*overwrite=*/0);
}

#endif  // !PROSPEROWEB_NATIVE_TITLE

// The project was called ProsperoBrowser and ps5browser before: carry the
// newest old profile over once (UI role, before config.json is read).
// Copies, never moves, so an old build keeps working from its directory.
void migrate_legacy_profile_dir() {
  using namespace prosperoweb::frontend;
  const auto result = prosperoweb::core::migrate_legacy_profile(
      {std::begin(kLegacyDataDirs), std::end(kLegacyDataDirs)}, kPs5DataDir);
  if (!result.ran) {
    return;
  }
  log_printf("migrate: copied %d items from %s", result.copied, result.from_dir.c_str());
  for (const auto& failure : result.failed) {
    log_printf("migrate: failed: %s", failure.c_str());
  }
}

// Logs the PROSPEROBROWSER_* and PS5BROWSER_* variables adopted at startup,
// once per launch.
void log_legacy_env(const std::vector<std::string>& adopted) {
  using prosperoweb::frontend::log_printf;
  if (adopted.empty()) {
    return;
  }
  std::string names;
  for (const auto& name : adopted) {
    names += (names.empty() ? "" : " ") + name;
  }
  log_printf("env: deprecated variable names used (%s); rename them to PROSPEROWEB_*, "
             "the old names stop working in the next release",
             names.c_str());
}

// Exports every --env=NAME=VALUE argument (overriding).
void apply_arg_env(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (!argv[i] || std::strncmp(argv[i], "--env=", 6) != 0) {
      continue;
    }
    const std::string entry = argv[i] + 6;
    const size_t eq = entry.find('=');
    if (eq == 0 || eq == std::string::npos) {
      continue;
    }
    setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), /*overwrite=*/1);
  }
}

#if PROSPEROWEB_NATIVE_TITLE

constexpr const char* kApp0 = "/app0";

// Whether dir holds the running title: the same eboot.bin as /app0 (size
// and modification time; nullfs shows the same file on both sides).
bool same_title(const std::string& dir, const std::string& app0) {
  struct stat a {};
  struct stat b {};
  return stat((dir + "/eboot.bin").c_str(), &a) == 0 &&
         stat((app0 + "/eboot.bin").c_str(), &b) == 0 && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec;
}

// Where the title's files are for this process and for the payloads it
// starts (src/ps5/spawn/title_paths.h), exported for the spawner and the
// chrome; the payload ELF the broker runs for the children; the read-only
// assets and the writable profile.
void setup_native_title_env() {
  namespace spawn = prosperoweb::spawn;
  using prosperoweb::frontend::log_printf;
  const auto layout = spawn::resolve_title_layout(kApp0, PROSPEROWEB_TITLE_ID,
                                                  std::getenv("PROSPEROWEB_TITLE_DIR"), same_title);
  log_printf("title: %s; UI reads %s, helpers %s", layout.how.c_str(), layout.ui_root.c_str(),
             layout.child_root.c_str());
  setenv("PROSPEROWEB_TITLE_UI_ROOT", layout.ui_root.c_str(), 1);
  setenv("PROSPEROWEB_TITLE_CHILD_ROOT", layout.child_root.c_str(), 1);
  setenv("PROSPEROWEB_SELF_PATH", (layout.child_root + "/prosperoweb.elf").c_str(), 0);
  setenv("PROSPEROWEB_CHROME_DIR", (layout.child_root + "/data/chrome").c_str(), 0);
  const char* self = std::getenv("PROSPEROWEB_SELF_PATH");
  const std::string self_here = spawn::rebase_path(self, layout.child_root, layout.ui_root);
  struct stat st {};
  log_printf("title: payload ELF for the children %s (%s here)", self,
             stat(self_here.c_str(), &st) == 0 ? "present" : "MISSING");
  // The small broker-only ELF, which the loaders start instead of the
  // 176 MB multi-call one (broker_main.cpp).
  const std::string broker_here = layout.ui_root + "/prosperoweb-broker.elf";
  if (stat(broker_here.c_str(), &st) == 0) {
    setenv("PROSPEROWEB_BROKER_PATH", (layout.child_root + "/prosperoweb-broker.elf").c_str(), 0);
    log_printf("title: broker ELF %s (%lld bytes)", std::getenv("PROSPEROWEB_BROKER_PATH"),
               static_cast<long long>(st.st_size));
  } else {
    log_printf("title: no %s; the loaders start the multi-call ELF as the broker",
               broker_here.c_str());
  }

  native_boot_say("[prosperoweb] main: title layout resolved, file selftest\n");
  native_file_selftest(layout.ui_root.c_str(), layout.child_root.c_str());
  native_profile_probe(prosperoweb::frontend::kPs5DataDir);
  native_boot_say("[prosperoweb] main: file selftest done\n");
  native_socket_selftest();
  native_etahen_probe();
  prosperoweb::frontend::setup_runtime_env(prosperoweb::frontend::kPs5DataDir,
                                           layout.ui_root + "/data");
  // JavaScriptCore's JIT needs executable memory, which a title does not
  // get: the UI runs JSC without it. The children are payloads and keep
  // their own defaults: the spawner leaves these out of their environment.
  setenv("JSC_useJIT", "false", 1);
  setenv("JSC_useWasm", "false", 1);
  setenv("PROSPEROWEB_UI_ONLY_ENV", "JSC_useJIT JSC_useWasm", 1);
  // With PROSPEROWEB_TRACE=1: the children keep their stderr in the profile
  // and log the pages' console messages there.
  if (prosperoweb::frontend::trace_enabled()) {
    setenv("PROSPEROWEB_CHILD_STDERR", "1", 0);
    setenv("PROSPEROWEB_CONSOLE_LOG", "1", 0);
  }
  log_printf("env JSC_useJIT=false JSC_useWasm=false (UI only; the children keep theirs)");
}

// stdout/stderr of the title go nowhere: WebKit's own messages (failed
// launches, assertions) land in this file instead.
void redirect_stdio() {
  const std::string path = std::string(prosperoweb::frontend::kPs5DataDir) + "/stderr-ui.txt";
  native_boot_say(native_stdio_redirect(path.c_str()) == 0
                      ? "[prosperoweb] main: stdout/stderr -> stderr-ui.txt\n"
                      : "[prosperoweb] main: stdout/stderr redirect FAILED (kept on /dev/null)\n");
}

// Every 5 s from 10 s on, posts a callback to the WebKit main run loop and
// logs how long it took to run, or that the previous one has not run: the
// launcher completes each child's launch on that loop.
std::atomic<long long> g_heartbeat_posted{0};
std::atomic<long long> g_heartbeat_ran{0};
pthread_t g_main_thread;
std::atomic<int> g_heartbeat_where{0};  // 1 main thread, 2 the posting thread, 3 another

// Threads and thread-local storage as WTF relies on them (isMainThread is
// pthread_equal(pthread_self(), main); RunLoop::currentSingleton is
// thread-local): a second thread must see its own identity and storage.
thread_local int t_tls_probe = 0;

void thread_selftest() {
  using prosperoweb::frontend::log_printf;
  pthread_key_t key;
  const bool key_ok = pthread_key_create(&key, nullptr) == 0;
  if (key_ok) {
    pthread_setspecific(key, reinterpret_cast<void*>(0x11));
  }
  t_tls_probe = 1;
  pthread_t other{};
  bool same_self = true;
  int other_tls = -1;
  void* other_key = reinterpret_cast<void*>(-1);
  std::thread probe([&] {
    other = pthread_self();
    same_self = pthread_equal(other, g_main_thread) != 0;
    other_tls = t_tls_probe;
    if (key_ok) {
      other_key = pthread_getspecific(key);
    }
  });
  probe.join();
  log_printf("threads: second thread pthread_self %s main; thread_local %s (%d); "
             "pthread_getspecific %s (%p)",
             same_self ? "EQUALS" : "differs from", other_tls == 0 ? "separate" : "SHARED",
             other_tls, other_key == nullptr ? "separate" : "SHARED", other_key);
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The last IPC message this process sent (webkit-port patch 0018), if
// WebKit provides it.
extern "C" const char* ps5IpcLastSend(int* fd, long long* at_ms) __attribute__((weak));

// Every second from 10 s on, posts a callback to the WebKit main run loop.
// If one has not run after 3 s, the UI is frozen: the watchdog leaves a
// line in boot.txt (written without locks, in case the log's lock is held)
// and in log.txt, with the last IPC message sent, and another when the loop
// runs again. With PROSPEROWEB_TRACE=1 it also reports where each heartbeat
// ran and the per-descriptor IPC counts every 5 s.
void start_main_loop_watchdog() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    // After the broker opened the profile to every user (spawn_glue.cpp).
    native_profile_probe(prosperoweb::frontend::kPs5DataDir);
    bool frozen = false;
    for (long long tick = 0;; tick++) {
      const long long posted = g_heartbeat_posted.load();
      const long long ran = g_heartbeat_ran.load();
      const long long now = now_ms();
      if (posted && ran < posted && now - posted >= 3000 && !frozen) {
        frozen = true;
        int fd = -1;
        long long at = 0;
        const char* last = ps5IpcLastSend ? ps5IpcLastSend(&fd, &at) : nullptr;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "[prosperoweb] main loop: FROZEN for %lld ms; last IPC sent: %s on fd %d, "
                      "%lld ms ago\n",
                      now - posted, last ? last : "(none)", fd, at ? now - at : -1);
        native_boot_say(line);
        prosperoweb::frontend::log_line(std::string(line + 14, std::strlen(line + 14) - 1));
      } else if (posted && ran >= posted && frozen) {
        frozen = false;
        char line[128];
        std::snprintf(line, sizeof(line), "[prosperoweb] main loop: running again after %lld ms\n",
                      ran - posted);
        native_boot_say(line);
        prosperoweb::frontend::log_line(std::string(line + 14, std::strlen(line + 14) - 1));
      } else if (posted && ran >= posted && prosperoweb::frontend::trace_enabled() &&
                 tick % 5 == 0) {
        const int where = g_heartbeat_where.load();
        prosperoweb::frontend::log_printf(
            "main loop: heartbeat (posted from the watchdog thread) ran %lld ms after posting, "
            "on %s",
            ran - posted,
            where == 1   ? "the main thread"
            : where == 2 ? "the POSTING thread (inline: isMainThread is wrong there)"
                         : "ANOTHER thread");
      }
      if (prosperoweb::frontend::trace_enabled() && tick % 5 == 0) {
        char io[1024];
        native_trace_io_summary(io, sizeof(io));
        prosperoweb::frontend::log_printf("trace: io (sendmsg/recvmsg per fd): %s",
                                          io[0] ? io : "none");
      }
      if (!posted || ran >= posted) {
        g_heartbeat_posted.store(now_ms());
        static thread_local int poster_marker = 0;
        static int* poster = nullptr;
        poster = &poster_marker;
        WKRunLoopCallOnMainThread(
            [](void*) {
              const int where = pthread_equal(pthread_self(), g_main_thread)
                                    ? 1
                                    : (&poster_marker == poster ? 2 : 3);
              g_heartbeat_where.store(where);
              g_heartbeat_ran.store(now_ms());
            },
            nullptr);
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();
}

#endif  // PROSPEROWEB_NATIVE_TITLE

}  // namespace

int main(int argc, char** argv) {
  using namespace prosperoweb::frontend;
#if PROSPEROWEB_NATIVE_TITLE
  native_boot_say("[prosperoweb] main: entered\n");
#endif
  // Every role: what one process creates in the profile, the others (the
  // native title's UI runs as another user) can use.
  umask(0);
  apply_arg_env(argc, argv);
  // Before anything reads the environment (the role, then every module).
  // The children inherit the new names, so only the UI or broker logs this.
  const std::vector<std::string> legacy_env = prosperoweb::core::adopt_legacy_env();
#if PROSPEROWEB_NATIVE_TITLE
  // The title is the UI: the system starts it with no role argument, and
  // the broker and the children run the payload ELF.
  const std::string role = "UI";
  native_boot_say("[prosperoweb] main: legacy env adopted\n");
  mkdir(kPs5DataDir, 0755);
  native_boot_say("[prosperoweb] main: profile dir ok, redirecting stdio\n");
  redirect_stdio();
  native_boot_say("[prosperoweb] main: stdio redirected, opening log.txt\n");
  log_set_role(role);
  log_open(std::string(kPs5DataDir) + "/log.txt");
  native_boot_say("[prosperoweb] main: log.txt open\n");
  log_printf("main: native title %s pid=%d argv0=%s", PROSPEROWEB_TITLE_ID,
             static_cast<int>(getpid()), argc > 0 && argv[0] ? argv[0] : "(none)");
  g_main_thread = pthread_self();
  thread_selftest();
  // config.json "probe_fork" (1/2 by name, 3/4 call rfork_thread as a real
  // NID import): can the title create a child process itself? Before the
  // decoder, the broker and WebKit, so the child copies as little as possible;
  // bounded, start-up goes on either way.
  native_fork_probe(config_probe_level(kPs5DataDir, "probe_fork"));
  // First, before any other thread and before any helper is started: the
  // hardware decoder's module and compute queue (VLC for PS5's order). Its
  // switches may be in config.json, which is applied later.
  for (const char* name : {"PROSPEROWEB_HWDEC", "PROSPEROWEB_VIDEO_DECODE"}) {
    const std::string value = config_env_value(kPs5DataDir, name);
    if (!value.empty() && setenv(name, value.c_str(), /*overwrite=*/0) == 0) {
      log_printf("env %s=%s (config.json, before the decoder start-up)", name,
                 std::getenv(name));
    }
  }
  native_boot_say("[prosperoweb] main: config read, decoder start-up\n");
  hwdec_native_startup();
  native_boot_say("[prosperoweb] main: decoder start-up done, loading modules\n");
  char modules[256];
  native_load_system_modules(modules, sizeof(modules));
  log_printf("main: system modules: %s", modules);
  log_printf("main: heap %zu MiB of direct memory%s", native_heap_size() >> 20,
             native_heap_size() ? "" : " (none: the system heap)");
  log_legacy_env(legacy_env);
  native_boot_say("[prosperoweb] main: modules loaded, migrating profile\n");
  migrate_legacy_profile_dir();
  native_boot_say("[prosperoweb] main: title paths and runtime env\n");
  setup_native_title_env();
  native_boot_say("[prosperoweb] main: runtime env set, starting watchdog\n");
  start_main_loop_watchdog();
  native_boot_say("[prosperoweb] main: starting the UI\n");
  tune_process_cpus();
  start_cpu_monitor();
  Ps5App app;
  return app.run();
#else
  std::string role = resolve_role(argc, argv);
  const bool child = role == "WebProcess" || role == "NetworkProcess";
  const bool broker = role == kBrokerRole;
  const bool evo_host = role == kEvoHostRole;
  const std::string log_path = std::string(kPs5DataDir) + "/log.txt";

  // websrv starts the broker with stdout/stderr on a pipe it has already
  // closed: logging must not die of SIGPIPE before the broker redirects them.
  // The EVO host writes to EVO's channel, which closes whenever EVO does.
  if (broker || evo_host) {
    std::signal(SIGPIPE, SIG_IGN);
  }

  // The UI logs from the first line on (its environment defaults included);
  // the children only open the shared log after their (identical) environment
  // setup, to keep it readable.
  log_set_role(broker ? "broker" : role);
  if (!child) {
    log_open(log_path);
    log_printf("main: pid=%d ppid=%d argv0=%s", static_cast<int>(getpid()),
               static_cast<int>(getppid()), argc > 0 && argv[0] ? argv[0] : "(none)");
    log_legacy_env(legacy_env);
  }
  if (!child && !broker) {
    migrate_legacy_profile_dir();
  }

  // Before any role starts: WebProcess and NetworkProcess read fontconfig,
  // TLS and temp-dir settings from their own environment.
  setup_runtime_env(kPs5DataDir, kPs5DataDir);
  if (!child) {
    export_self_path(argc, argv);
  } else {
    log_open(log_path);
    log_printf("main: pid=%d ppid=%d started", static_cast<int>(getpid()),
               static_cast<int>(getppid()));
    // The IPC descriptor the UI handed over (ps5webkit/spawn.h: fd 3).
    prosperoweb::spawn::ipc_describe_inherited_fd(3);
    // PROSPEROWEB_CHILD_STDERR=1: the child's stderr (WebKit's own messages,
    // console messages with PROSPEROWEB_CONSOLE_LOG=1) goes to a file, as the
    // broker starts children with stderr on /dev/null.
    if (const char* keep = std::getenv("PROSPEROWEB_CHILD_STDERR"); keep && !std::strcmp(keep, "1")) {
      const std::string path = std::string(kPs5DataDir) + "/stderr-" + role + ".txt";
      if (std::freopen(path.c_str(), "a", stderr)) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        log_printf("main: stderr -> %s", path.c_str());
      }
    }
  }

  // Every role, before it starts a thread: the threads inherit the mask.
  tune_process_cpus();

  if (broker) {
    const int rc = ps5_spawn_broker_main();
    log_printf("main: broker returned %d", rc);
    return rc;
  }
  if (role == "WebProcess") {
    // The main thread also runs the media presentation tick: above the
    // decoder threads, which inherit the default priority.
    raise_main_thread_priority();
    // GPU compositing bring-up (EVO #237): with /data/prosperoweb/gpu
    // present, start Sony's GLES library here and check frames reach the GPU
    // and come back. Once started, every exit path tells the system
    // compositor Quit (ps5_glslim.c); its log is /data/prosperoweb/glslim.txt.
    if (ps5_glslim_selftest) {
      struct stat st;
      if (stat((std::string(kPs5DataDir) + "/gpu").c_str(), &st) == 0) {
        const int g = ps5_glslim_selftest();
        log_printf("main: GPU self-test %s (glslim.txt)", g == 0 ? "passed" : "FAILED");
        // WebKit makes its GL display only after a self-test that really
        // drew (a second process can get an empty grant whose init still
        // "succeeds"); otherwise it stays on the raster path.
        if (g == 0) {
          setenv("PROSPEROWEB_GPU_READY", "1", 1);
        }
      }
    }
    start_cpu_monitor();
    const int rc = ps5_webkit_web_process_main(argc, argv);
    log_printf("main: returned %d", rc);
    return rc;
  }
  if (role == "NetworkProcess") {
    const int rc = ps5_webkit_network_process_main(argc, argv);
    log_printf("main: returned %d", rc);
    return rc;
  }

  start_cpu_monitor();
  if (evo_host) {
    EvoHost host;
    const int rc = host.run();
    log_printf("main: evo-host returned %d", rc);
    return rc;
  }
  Ps5App app;
  return app.run();
#endif
}
