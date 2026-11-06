// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_cpu_affinity.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <new>

#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/rtprio.h>
#include <unistd.h>

#include "ps5_log.h"

// In libkernel, not declared by the SDK's FreeBSD 11 headers.
extern "C" int pthread_getname_np(pthread_t thread, char* name, size_t length);

namespace prosperoweb::frontend {

namespace {

bool env_off(const char* name) {
  const char* value = std::getenv(name);
  return value && (!std::strcmp(value, "0") || !std::strcmp(value, "off"));
}

int raw_get(int level, int which, id_t id, std::size_t size, cpu::CpuMask& mask) {
  mask = {};
  if (cpuset_getaffinity(static_cast<cpulevel_t>(level), static_cast<cpuwhich_t>(which), id, size,
                         reinterpret_cast<cpuset_t*>(mask.bytes.data())) != 0) {
    return errno;
  }
  return 0;
}

std::once_flag g_probe_once;
std::size_t g_size = 0;
std::string g_tried;

// Mask the threads must start with; set by tune_process_cpus().
std::atomic<bool> g_have_target{false};
cpu::CpuMask g_target;

std::atomic<unsigned> g_started{0};
std::atomic<unsigned> g_widened{0};
std::atomic<unsigned> g_widen_failed{0};

struct Registered {
  pthread_t handle;
  clockid_t clock;
  bool has_clock;
};

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}
std::map<long, Registered>& registry() {
  static auto* threads = new std::map<long, Registered>();  // never destroyed: threads outlive exit()
  return *threads;
}

void register_self() {
  Registered entry{pthread_self(), {}, false};
  entry.has_clock = pthread_getcpuclockid(entry.handle, &entry.clock) == 0;
  const long tid = pthread_getthreadid_np();
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry()[tid] = entry;
}

void unregister_self() {
  const long tid = pthread_getthreadid_np();
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().erase(tid);
}

struct Start {
  void* (*fn)(void*);
  void* arg;
};

void* thread_trampoline(void* raw) {
  const Start start = *static_cast<Start*>(raw);
  delete static_cast<Start*>(raw);
  g_started.fetch_add(1, std::memory_order_relaxed);
  if (g_have_target.load(std::memory_order_acquire)) {
    cpu::CpuMask mine;
    if (get_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, mine) == 0 && !(mine == g_target)) {
      if (set_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, g_target) == 0) {
        g_widened.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_widen_failed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  register_self();
  void* result = start.fn(start.arg);
  unregister_self();
  return result;
}

}  // namespace

std::size_t kernel_cpuset_size(std::string* tried) {
  std::call_once(g_probe_once, [] {
    const cpu::ProbeResult r = cpu::probe_size([](std::size_t size, void* buffer) {
      if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, size,
                             static_cast<cpuset_t*>(buffer)) != 0) {
        return errno;
      }
      return 0;
    });
    g_size = r.size;
    g_tried = r.tried;
  });
  if (tried) {
    *tried = g_tried;
  }
  return g_size;
}

int get_cpu_mask(int level, int which, id_t id, cpu::CpuMask& mask) {
  const std::size_t size = kernel_cpuset_size();
  if (!size) {
    mask = {};
    return ERANGE;
  }
  return raw_get(level, which, id, size, mask);
}

int set_cpu_mask(int level, int which, id_t id, const cpu::CpuMask& mask) {
  const std::size_t size = kernel_cpuset_size();
  if (!size) {
    return ERANGE;
  }
  if (cpuset_setaffinity(static_cast<cpulevel_t>(level), static_cast<cpuwhich_t>(which), id, size,
                         reinterpret_cast<const cpuset_t*>(mask.bytes.data())) != 0) {
    return errno;
  }
  return 0;
}

void tune_process_cpus() {
  std::string tried;
  const std::size_t size = kernel_cpuset_size(&tried);
  const long online = sysconf(_SC_NPROCESSORS_ONLN);
  if (!size) {
    log_printf("cpu: %ld cpus online, no cpuset size works (tried %s), affinity left alone", online,
               tried.c_str());
    return;
  }
  auto text = [](int error, const cpu::CpuMask& m) {
    return error ? "error " + cpu::errno_name(error)
                 : cpu::to_list(m) + " (" + cpu::to_hex(m) + ")";
  };
  cpu::CpuMask before, root, cpuset;
  const int before_error = get_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, before);
  const int root_error = get_cpu_mask(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, root);
  const int cpuset_error = get_cpu_mask(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, cpuset);

  cpu::CpuMask requested;
  bool have_requested = false;
  if (const char* env = std::getenv("PROSPEROWEB_CPUMASK"); env && *env) {
    have_requested = cpu::parse_hex(env, requested);
    if (!have_requested) {
      log_printf("cpu: PROSPEROWEB_CPUMASK=%s is not a hex mask, ignored", env);
    }
  }

  std::string outcome;
  if (before_error) {
    outcome = "process mask unreadable";
  } else if (env_off("PROSPEROWEB_CPU_WIDEN")) {
    outcome = "PROSPEROWEB_CPU_WIDEN=0";
  } else {
    const cpu::WidenPlan plan = cpu::plan_widen(before, root_error ? nullptr : &root,
                                                have_requested ? &requested : nullptr);
    outcome = plan.reason;
    if (plan.change) {
      // The whole process (every thread); failing that, this thread, which
      // the role's threads are created from.
      int error = set_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, plan.target);
      if (error == 0) {
        outcome += ", set for the process";
      } else {
        const int thread_error = set_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, plan.target);
        outcome += ", process " + cpu::errno_name(error) + ", main thread " +
                   (thread_error ? cpu::errno_name(thread_error) : std::string("set"));
        error = thread_error;
      }
      if (error == 0) {
        g_target = plan.target;
        g_have_target.store(true, std::memory_order_release);
      }
    }
  }
  cpu::CpuMask after;
  const int after_error = get_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, after);
  log_printf("cpu: mask before %s, root %s, after %s (size %zu bytes) | %ld cpus online, cpuset %s, "
             "%s | probe %s",
             text(before_error, before).c_str(), text(root_error, root).c_str(),
             text(after_error, after).c_str(), size, online, text(cpuset_error, cpuset).c_str(),
             outcome.c_str(), tried.c_str());
}

void raise_main_thread_priority() {
  if (env_off("PROSPEROWEB_THREAD_PRIO")) {
    return;
  }
  struct rtprio before{};
  const bool known = rtprio_thread(RTP_LOOKUP, 0, &before) == 0;
  // The lowest realtime level is still above every timeshare thread.
  struct rtprio wanted{RTP_PRIO_REALTIME, RTP_PRIO_MAX};
  if (rtprio_thread(RTP_SET, 0, &wanted) == 0) {
    struct rtprio after{};
    rtprio_thread(RTP_LOOKUP, 0, &after);
    log_printf("prio: main thread rtprio %s%d/%d -> %d/%d", known ? "" : "? ", before.type,
               before.prio, after.type, after.prio);
    return;
  }
  const int rt_error = errno;
  int policy = 0;
  sched_param param{};
  pthread_getschedparam(pthread_self(), &policy, &param);
  const int old_policy = policy;
  const int old_priority = param.sched_priority;
  param.sched_priority = sched_get_priority_min(SCHED_RR);
  const int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
  pthread_getschedparam(pthread_self(), &policy, &param);
  log_printf("prio: main thread rtprio %d/%d, realtime refused (%s); pthread_setschedparam "
             "SCHED_RR: %s, policy %d/%d -> %d/%d",
             before.type, before.prio, cpu::errno_name(rt_error).c_str(),
             rc ? cpu::errno_name(rc).c_str() : "ok", old_policy, old_priority, policy,
             param.sched_priority);
}

std::vector<ThreadSample> sample_registered_threads() {
  std::vector<ThreadSample> out;
  std::lock_guard<std::mutex> lock(registry_mutex());
  out.reserve(registry().size());
  for (const auto& [tid, entry] : registry()) {
    ThreadSample s;
    s.tid = tid;
    char name[64] = {};
    if (pthread_getname_np(entry.handle, name, sizeof(name)) == 0) {
      name[sizeof(name) - 1] = '\0';
      s.name = name;
    }
    if (s.name.empty()) {
      s.name = "tid" + std::to_string(tid);
    }
    timespec ts{};
    if (entry.has_clock && clock_gettime(entry.clock, &ts) == 0) {
      s.cpu_seconds = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    }
    cpu::CpuMask mask;
    const int error = get_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, static_cast<id_t>(tid), mask);
    s.mask = error ? "error " + cpu::errno_name(error) : cpu::to_list(mask);
    out.push_back(std::move(s));
  }
  return out;
}

ThreadCounters registered_thread_counters() {
  return {g_started.load(), g_widened.load(), g_widen_failed.load()};
}

}  // namespace prosperoweb::frontend

// Linked with -Wl,--wrap=pthread_create,--wrap=pthread_exit (CMakeLists.txt):
// every pthread_create of the executable (WebKit, libavcodec, SDL) lands here.
extern "C" {
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*),
                          void* arg);
[[noreturn]] void __real_pthread_exit(void* value);

// The least stack a new thread gets; 0 leaves the creator's choice
// (ps5_thread_set_min_stack, called by src/ps5/mesa once Mesa starts).
std::atomic<std::size_t> ps5_thread_min_stack{0};
void ps5_thread_set_min_stack(std::size_t bytes) {
  ps5_thread_min_stack.store(bytes, std::memory_order_release);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*),
                          void* arg) {
  using prosperoweb::frontend::Start;
#if PROSPEROWEB_NATIVE_TITLE
  // A title's default thread stack is 64 KiB (the Kodi and Stremio PS5
  // ports, native/posix_fixes.c): threads made with default attributes get
  // 2 MiB, explicit sizes are raised to 1 MiB at least.
  constexpr std::size_t kDefaultStack = std::size_t{2} << 20;
  constexpr std::size_t kMinStack = std::size_t{1} << 20;
  pthread_attr_t local;
  bool own_attr = false;
  std::size_t size = 0;
  if (!attr) {
    if (pthread_attr_init(&local) == 0) {
      pthread_attr_setstacksize(&local, kDefaultStack);
      attr = &local;
      own_attr = true;
    }
  } else if (pthread_attr_getstacksize(attr, &size) == 0 && size < kMinStack) {
    pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), kMinStack);
  }
  struct AttrGuard {
    pthread_attr_t* attr;
    ~AttrGuard() {
      if (attr) {
        pthread_attr_destroy(attr);
      }
    }
  } guard{own_attr ? &local : nullptr};
#else
  // Mesa in the WebProcess (src/ps5/mesa, EVO Player #237): GLSL compiles on
  // whichever thread draws, and nir_opt_varyings alone takes a ~93 KB frame,
  // so once Mesa starts every thread gets at least 2 MiB (libkernel's own
  // stacks, flexible memory; a payload has almost no direct memory).
  const std::size_t min_stack = ps5_thread_min_stack.load(std::memory_order_acquire);
  pthread_attr_t local;
  bool own_attr = false;
  if (min_stack) {
    std::size_t size = 0;
    if (!attr) {
      if (pthread_attr_init(&local) == 0) {
        pthread_attr_setstacksize(&local, min_stack);
        attr = &local;
        own_attr = true;
      }
    } else if (pthread_attr_getstacksize(attr, &size) == 0 && size < min_stack) {
      pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), min_stack);
    }
  }
  struct AttrGuard {
    pthread_attr_t* attr;
    ~AttrGuard() {
      if (attr) {
        pthread_attr_destroy(attr);
      }
    }
  } guard{own_attr ? &local : nullptr};
#endif
  auto* start = new (std::nothrow) Start{fn, arg};
  if (!start) {
    return __real_pthread_create(thread, attr, fn, arg);
  }
  const int rc =
      __real_pthread_create(thread, attr, &prosperoweb::frontend::thread_trampoline, start);
  if (rc != 0) {
    delete start;
  }
  return rc;
}

[[noreturn]] void __wrap_pthread_exit(void* value) {
  prosperoweb::frontend::unregister_self();
  __real_pthread_exit(value);
}
}
