// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_cpu_monitor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <unistd.h>

#include "cpu_mask.h"
#include "ps5_cpu_affinity.h"
#include "ps5_log.h"

// Defined by the media engine (FFmpegVideoDecoder.cpp) when it is linked.
extern "C" void ps5_media_video_decoder_stats(unsigned* decoders, unsigned* threads)
    __attribute__((weak));

namespace prosperoweb::frontend {

namespace {

constexpr int kIntervalSeconds = 30;

bool env_off(const char* name) {
  const char* value = std::getenv(name);
  return value && (!std::strcmp(value, "0") || !std::strcmp(value, "off"));
}

std::string mask_text(int level, int which, id_t id, cpu::CpuMask* out = nullptr) {
  cpu::CpuMask mask;
  const int error = get_cpu_mask(level, which, id, mask);
  if (out) {
    *out = mask;
  }
  return error ? "error " + cpu::errno_name(error) : cpu::to_list(mask);
}

double seconds_of(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

struct ThreadInfo {
  long tid = 0;
  std::string name;
  double cpu_seconds = 0;
  int last_cpu = -1;
  std::string mask;
};

// The process's threads (sysctl kern.proc.pid with threads). Empty when the
// kernel's kinfo_proc does not look like the SDK's, or the console has no
// such sysctl (ENOENT on the PS5).
std::vector<ThreadInfo> list_threads_sysctl(std::string& why) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD,
                static_cast<int>(getpid())};
  size_t bytes = 0;
  if (sysctl(mib, 4, nullptr, &bytes, nullptr, 0) != 0 || bytes == 0) {
    why = std::string("sysctl kern.proc.pid: ") + std::strerror(errno);
    return {};
  }
  bytes += bytes / 4 + 4096;  // threads may appear meanwhile
  std::vector<char> buffer(bytes);
  if (sysctl(mib, 4, buffer.data(), &bytes, nullptr, 0) != 0) {
    why = std::string("sysctl kern.proc.pid: ") + std::strerror(errno);
    return {};
  }
  std::vector<ThreadInfo> threads;
  size_t offset = 0;
  while (offset + sizeof(kinfo_proc) <= bytes) {
    kinfo_proc info;
    std::memcpy(&info, buffer.data() + offset, sizeof(info));
    if (info.ki_structsize != static_cast<int>(sizeof(kinfo_proc))) {
      why = "kinfo_proc size " + std::to_string(info.ki_structsize) + " != " +
            std::to_string(sizeof(kinfo_proc));
      return {};
    }
    ThreadInfo t;
    t.tid = info.ki_tid;
    info.ki_tdname[sizeof(info.ki_tdname) - 1] = '\0';
    t.name = info.ki_tdname;
    t.cpu_seconds = seconds_of(info.ki_rusage.ru_utime) + seconds_of(info.ki_rusage.ru_stime);
    t.last_cpu = info.ki_lastcpu;
    t.mask = mask_text(CPU_LEVEL_WHICH, CPU_WHICH_TID, static_cast<id_t>(t.tid));
    threads.push_back(std::move(t));
    offset += sizeof(kinfo_proc);
  }
  return threads;
}

class Monitor {
 public:
  void report(bool first) {
    const auto now = std::chrono::steady_clock::now();
    cpu::CpuMask root_mask, process_mask;
    const std::string root_text = mask_text(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, &root_mask);
    const std::string process_text = mask_text(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, &process_mask);
    const int root_count = root_mask.count();
    const int process_count = process_mask.count();

    // Threads: the kernel's list when it has one, else the threads this
    // executable created (pthread_create is wrapped, ps5_cpu_affinity.h).
    std::vector<ThreadInfo> threads;
    std::string source = "kern.proc";
    if (sysctl_works_) {
      std::string why;
      threads = list_threads_sysctl(why);
      if (threads.empty()) {
        sysctl_works_ = false;
        log_printf("cpu: no kernel thread list (%s), using the threads created through "
                   "pthread_create",
                   why.c_str());
      }
    }
    if (!sysctl_works_) {
      source = "pthread_create";
      for (const ThreadSample& s : sample_registered_threads()) {
        ThreadInfo t;
        t.tid = s.tid;
        t.name = s.name;
        t.cpu_seconds = s.cpu_seconds < 0 ? 0 : s.cpu_seconds;
        t.mask = s.mask;
        threads.push_back(std::move(t));
      }
    }
    std::map<std::string, int> masks;  // cpu list -> threads
    for (const ThreadInfo& t : threads) {
      masks[t.mask]++;
    }
    std::string mask_summary;
    for (const auto& [list, count] : masks) {
      mask_summary += (mask_summary.empty() ? "" : ", ") + list + " x" + std::to_string(count);
    }
    if (threads.empty()) {
      mask_summary = "-";
    }
    const ThreadCounters counters = registered_thread_counters();

    unsigned decoders = 0;
    unsigned decoder_threads = 0;
    if (ps5_media_video_decoder_stats) {
      ps5_media_video_decoder_stats(&decoders, &decoder_threads);
    }

    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double user = seconds_of(usage.ru_utime);
    const double sys = seconds_of(usage.ru_stime);
    const double wall =
        first ? 0.0 : std::chrono::duration<double>(now - last_time_).count();
    const double busy = wall > 0 ? (user - last_user_ + sys - last_sys_) / wall : 0.0;

    // Busiest threads since the previous report.
    std::vector<std::pair<double, const ThreadInfo*>> deltas;
    for (const ThreadInfo& t : threads) {
      auto it = last_thread_cpu_.find(t.tid);
      const double before = it == last_thread_cpu_.end() ? 0.0 : it->second;
      deltas.emplace_back(t.cpu_seconds - before, &t);
    }
    std::sort(deltas.begin(), deltas.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (std::size_t i = 0; i < deltas.size() && i < 6; i++) {
      if (deltas[i].first < 0.05) {
        break;
      }
      char item[112];
      if (deltas[i].second->last_cpu >= 0) {
        std::snprintf(item, sizeof(item), "%s%s %.1fs@cpu%d", top.empty() ? "" : ", ",
                      deltas[i].second->name.c_str(), deltas[i].first, deltas[i].second->last_cpu);
      } else {
        std::snprintf(item, sizeof(item), "%s%s %.1fs", top.empty() ? "" : ", ",
                      deltas[i].second->name.c_str(), deltas[i].first);
      }
      top += item;
    }

    log_printf("cpu: affinity %s (%d of %d, root %s) | threads %zu (%s; started %u, re-widened "
               "%u, failed %u): %s | video decoders %u, %u codec threads | user %.1fs sys %.1fs "
               "over %.0fs = %.2f cpus busy | top %s",
               process_text.c_str(), process_count, root_count, root_text.c_str(), threads.size(),
               source.c_str(), counters.started, counters.widened, counters.widen_failed,
               mask_summary.c_str(), decoders, decoder_threads, user - last_user_,
               sys - last_sys_, wall, busy, top.empty() ? "-" : top.c_str());

    last_time_ = now;
    last_user_ = user;
    last_sys_ = sys;
    last_thread_cpu_.clear();
    for (const ThreadInfo& t : threads) {
      last_thread_cpu_[t.tid] = t.cpu_seconds;
    }
  }

 private:
  std::chrono::steady_clock::time_point last_time_;
  double last_user_ = 0;
  double last_sys_ = 0;
  std::map<long, double> last_thread_cpu_;
  bool sysctl_works_ = true;
};

}  // namespace

void start_cpu_monitor() {
  if (env_off("PROSPEROWEB_CPU_MONITOR")) {
    return;
  }
  auto* monitor = new Monitor();  // lives as long as the process
  monitor->report(/*first=*/true);
  std::thread([monitor] {
    // /data/prosperoweb/cpu_now: a report whenever the file's mtime changes
    // (the benchmark touches it at the start and end of a run, so every
    // process reports exactly the window in between).
    auto mtime = [] {
      struct stat st;
      return stat("/data/prosperoweb/cpu_now", &st) == 0
                 ? static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec
                 : 0LL;
    };
    long long seen = mtime();
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(kIntervalSeconds);
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const long long m = mtime();
      const auto now = std::chrono::steady_clock::now();
      if (m != seen || now >= next) {
        seen = m;
        next = now + std::chrono::seconds(kIntervalSeconds);
        monitor->report(/*first=*/false);
      }
    }
  }).detach();
}

}  // namespace prosperoweb::frontend
