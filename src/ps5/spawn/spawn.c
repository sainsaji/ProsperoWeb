// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-process spawner on top of the elfldr-derived loader: used by the
// spawn broker for every spawn, and by the UI itself in the "direct" spawn
// mode (spawn_glue.cpp selects the mode). PS5 target only.

#include "spawn_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "cpuset_assign_ps5.h"
#include "elfldr.h"

extern char** environ;

// The ELF image being spawned (our own multi-call ELF), mapped (or, if mmap
// fails, read) once and kept for the lifetime of the process. WebKit spawns
// auxiliary processes repeatedly, so re-reading the file each time would be
// wasteful; a read-only private mapping keeps the large image out of the
// heap. One slot: a request for another path replaces it.
static pthread_mutex_t g_image_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t* g_image_bytes = NULL;
static size_t g_image_size = 0;
static int g_image_mapped = 0;
static char g_image_path[1024];

// Adapter: this libc declares getenv() as char*(const char*); the resolver
// takes a const char*(const char*) lookup.
static const char* getenv_const(const char* name) { return getenv(name); }

static uint8_t* read_whole_file(int fd, size_t size) {
  uint8_t* bytes = malloc(size);
  if (!bytes) {
    return NULL;
  }
  size_t done = 0;
  while (done < size) {
    ssize_t n = read(fd, bytes + done, size - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      free(bytes);
      return NULL;
    }
    done += (size_t)n;
  }
  return bytes;
}

static void unload_image(void) {
  if (!g_image_bytes) {
    return;
  }
  if (g_image_mapped) {
    munmap((void*)g_image_bytes, g_image_size);
  } else {
    free((void*)g_image_bytes);
  }
  g_image_bytes = NULL;
  g_image_size = 0;
  g_image_path[0] = '\0';
}

static int load_image(const char* path) {
  if (strlen(path) >= sizeof(g_image_path)) {
    ps5_log_printf("spawn: ELF path too long: %s", path);
    return -1;
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    ps5_log_printf("spawn: cannot open %s: %s", path, strerror(errno));
    return -1;
  }

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    ps5_log_printf("spawn: cannot stat %s", path);
    close(fd);
    return -1;
  }

  size_t size = (size_t)st.st_size;
  const uint8_t* bytes = NULL;
  int mapped = 0;
  void* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map != MAP_FAILED) {
    bytes = map;
    mapped = 1;
  } else {
    ps5_log_printf("spawn: mmap %s failed (%s), reading it", path, strerror(errno));
    bytes = read_whole_file(fd, size);
  }
  close(fd);
  if (!bytes) {
    ps5_log_printf("spawn: cannot read %s", path);
    return -1;
  }

  if (elfldr_sanity_check(bytes, size) != 0) {
    ps5_log_printf("spawn: %s is not a valid ELF", path);
    if (mapped) {
      munmap((void*)bytes, size);
    } else {
      free((void*)bytes);
    }
    return -1;
  }

  ps5_log_printf("spawn: self ELF %s (%zu bytes, %s), fw 0x%08x, pid %d",
                 path, size, mapped ? "mapped" : "read",
                 (unsigned)kernel_get_fw_version(), (int)getpid());
  g_image_bytes = bytes;
  g_image_size = size;
  g_image_mapped = mapped;
  memcpy(g_image_path, path, strlen(path) + 1);
  return 0;
}

// Returns the image of path, loading it on first use. Callers serialise
// their spawns per image (one path per process in practice), so replacing
// the slot does not pull an image from under a running spawn.
static const uint8_t* image_for(const char* path, size_t* out_size) {
  const uint8_t* result = NULL;
  pthread_mutex_lock(&g_image_lock);
  if (!g_image_bytes || strcmp(g_image_path, path) != 0) {
    unload_image();
    load_image(path);
  }
  if (g_image_bytes) {
    result = g_image_bytes;
    *out_size = g_image_size;
  }
  pthread_mutex_unlock(&g_image_lock);
  return result;
}

// --- Child bookkeeping -------------------------------------------------------
//
// Every child is tracked until it is reaped, either by ps5_terminate_process()
// or, for children that exit on their own (a WebProcess crash, a
// NetworkProcess exit), by a reaper thread that polls them. Only tracked pids
// are waited for: a waitpid(-1) would also swallow the ptrace stops the
// loader waits for while it sets up a new child.

#define PS5_SPAWN_MAX_CHILDREN 64
#define PS5_SPAWN_REAP_INTERVAL_MS 1000
#define PS5_SPAWN_KILL_TIMEOUT_MS 2000

static pthread_mutex_t g_children_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_children[PS5_SPAWN_MAX_CHILDREN];
static int g_nchildren = 0;
static int g_reaper_started = 0;

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// 1 if pid is gone (reaped now, or not our child any more), 0 if still alive.
// *status receives the wait status when it was reaped now, -1 otherwise.
static int try_reap(pid_t pid, int* status) {
  *status = -1;
  for (;;) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
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

// Describes a wait status ("exit 1", "signal 9") into buf.
static const char* describe_status(int status, char* buf, size_t len) {
  if (status == -1) {
    snprintf(buf, len, "status unknown");
  } else if (WIFEXITED(status)) {
    snprintf(buf, len, "exit %d", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    snprintf(buf, len, "signal %d", WTERMSIG(status));
  } else {
    snprintf(buf, len, "status 0x%x", status);
  }
  return buf;
}

// Symbolic name of the errno values a spawn is expected to hit.
static const char* errno_name(int err) {
  switch (err) {
    case 0: return "0";
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case ESRCH: return "ESRCH";
    case EINTR: return "EINTR";
    case EIO: return "EIO";
    case E2BIG: return "E2BIG";
    case ENOEXEC: return "ENOEXEC";
    case EBADF: return "EBADF";
    case ECHILD: return "ECHILD";
    case EAGAIN: return "EAGAIN";
    case ENOMEM: return "ENOMEM";
    case EACCES: return "EACCES";
    case EFAULT: return "EFAULT";
    case EBUSY: return "EBUSY";
    case EINVAL: return "EINVAL";
    case EMFILE: return "EMFILE";
    case ENOSYS: return "ENOSYS";
    default: return "errno";
  }
}

// Caller holds g_children_lock. Returns 1 if pid was tracked.
static int untrack_locked(pid_t pid) {
  for (int i = 0; i < g_nchildren; i++) {
    if (g_children[i] == pid) {
      g_children[i] = g_children[--g_nchildren];
      return 1;
    }
  }
  return 0;
}

static void* reaper_main(void* arg) {
  (void)arg;
  for (;;) {
    sleep_ms(PS5_SPAWN_REAP_INTERVAL_MS);
    pthread_mutex_lock(&g_children_lock);
    for (int i = 0; i < g_nchildren;) {
      int status;
      if (try_reap(g_children[i], &status)) {
        char desc[32];
        ps5_log_printf("spawn: child pid=%d exited (%s)", (int)g_children[i],
                       describe_status(status, desc, sizeof(desc)));
        g_children[i] = g_children[--g_nchildren];
      } else {
        i++;
      }
    }
    pthread_mutex_unlock(&g_children_lock);
  }
  return NULL;
}

static void track_child(pid_t pid) {
  pthread_mutex_lock(&g_children_lock);
  if (g_nchildren < PS5_SPAWN_MAX_CHILDREN) {
    g_children[g_nchildren++] = pid;
  } else {
    ps5_log_printf("spawn: too many children, %d is not reaped", (int)pid);
  }
  if (!g_reaper_started) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, reaper_main, NULL) == 0) {
      pthread_detach(thread);
      g_reaper_started = 1;
    } else {
      ps5_log_line("spawn: cannot start the reaper thread");
    }
  }
  pthread_mutex_unlock(&g_children_lock);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// "12->3 13->4" for the log; "93:12->3" for a descriptor of another process.
static void describe_fds(const struct ps5_spawn_fd_entry* table, int n, char* buf,
                         size_t len) {
  size_t used = 0;
  buf[0] = '\0';
  for (int i = 0; i < n && used < len; i++) {
    int w = table[i].src_pid > 0
                ? snprintf(buf + used, len - used, "%s%d:%d->%d", i ? " " : "",
                           table[i].src_pid, table[i].parent_fd, table[i].child_fd)
                : snprintf(buf + used, len - used, "%s%d->%d", i ? " " : "",
                           table[i].parent_fd, table[i].child_fd);
    if (w < 0) {
      break;
    }
    used += (size_t)w;
  }
}

// One line per spawn with the outcome, the budget result and, on failure,
// the stage that failed (child- or parent-side).
static void log_spawn_result(const char* role, int pid, const char* fds,
                             const elfldr_spawn_report_t* report, long elapsed_ms) {
  char budget[96];
  if (report->budget_errno == 0) {
    snprintf(budget, sizeof(budget), "budget_set(0) ok");
  } else {
    snprintf(budget, sizeof(budget),
             "budget_set(0) failed: %s (continuing with inherited budget)",
             errno_name(report->budget_errno));
  }
  if (pid > 0) {
    ps5_log_printf("spawn: role=%s pid=%d fds=[%s] ok in %ldms, %s", role, pid, fds,
                   elapsed_ms, budget);
    return;
  }
  char status[32] = "";
  if (report->child_status) {
    describe_status(report->child_status, status, sizeof(status));
  }
  ps5_log_printf("spawn: role=%s FAILED at %s: %s %d (%s)%s%s, %s", role,
                 report->failed_stage ? report->failed_stage : "unknown stage",
                 errno_name(report->failed_errno), report->failed_errno,
                 report->failed_errno ? strerror(report->failed_errno) : "no errno",
                 status[0] ? ", child " : "", status, budget);
}

int ps5_spawn_elfldr(const char* elf_path, char* const* argv, char* const* envp,
                     const int* fds, int nfds, int src_pid,
                     struct ps5_spawn_failure* failure) {
  failure->stage = NULL;
  failure->err = 0;
  failure->budget_errno = 0;
  const char* role = argv && argv[0] ? argv[0] : "(none)";

  struct ps5_spawn_fd_entry table[PS5_SPAWN_MAX_FDS];
  int ntable = ps5_spawn_build_fd_table(fds, nfds, src_pid, table, PS5_SPAWN_MAX_FDS);
  if (ntable < 0) {
    ps5_log_printf("spawn: role=%s FAILED: invalid fd table (nfds=%d)", role, nfds);
    failure->stage = "fd table";
    failure->err = EINVAL;
    return -1;
  }
  char fd_desc[160];
  describe_fds(table, ntable, fd_desc, sizeof(fd_desc));

  size_t image_size = 0;
  const uint8_t* image = image_for(elf_path, &image_size);
  if (!image) {
    ps5_log_printf("spawn: role=%s FAILED: cannot load %s", role, elf_path);
    failure->stage = "load ELF";
    failure->err = ENOEXEC;
    return -1;
  }

  elfldr_spawn_report_t report;
  ps5_log_printf("spawn: role=%s starting, fds=[%s]", role, fd_desc);
  const long t0 = now_ms();
  int pid = elfldr_spawn_fds(argv, envp, table, ntable, image, image_size, &report);
  log_spawn_result(role, pid, fd_desc, &report, now_ms() - t0);
  failure->budget_errno = report.budget_errno;
  if (pid <= 0) {
    failure->stage = report.failed_stage ? report.failed_stage : "unknown stage";
    failure->err = report.failed_errno;
    return -1;
  }

  // The child started in the console's system cpuset (9,11,13-15); move it
  // onto the UI's app cores (src_pid is the UI). Best-effort, non-fatal.
  ps5_cpuset_assign_child(pid, src_pid);
  return pid;
}

int ps5_spawn_direct(const char* const* argv, const int* fds, int nfds) {
  if (!argv || !argv[0]) {
    ps5_log_line("spawn: called without a role");
    return -1;
  }
  const char* role = argv[0];

  // argv[0] is the child's role name, never a path: resolve the image from
  // PROSPEROWEB_SELF_PATH (exported by main() from the UI's own argv[0]) or
  // the default install path.
  char path[1024];
  if (ps5_spawn_resolve_self_path(NULL, getenv_const, path, sizeof(path)) != 0) {
    ps5_log_printf("spawn: role=%s FAILED: cannot resolve the self ELF path", role);
    return -1;
  }

  // rfork_thread/execve want a mutable, NUL-terminated argv; copy the caller's
  // (which is only valid for the duration of this call).
  int argc = 0;
  while (argv[argc]) {
    argc++;
  }
  char** child_argv = calloc((size_t)argc + 1, sizeof(char*));
  if (!child_argv) {
    ps5_log_printf("spawn: role=%s FAILED: out of memory", role);
    return -1;
  }
  int ok = 1;
  for (int i = 0; i < argc && ok; i++) {
    child_argv[i] = strdup(argv[i]);
    ok = child_argv[i] != NULL;
  }

  int pid = -1;
  if (ok) {
    struct ps5_spawn_failure failure;
    pid = ps5_spawn_elfldr(path, child_argv, environ, fds, nfds, 0, &failure);
  } else {
    ps5_log_printf("spawn: role=%s FAILED: out of memory", role);
  }
  if (pid > 0) {
    track_child(pid);
  }

  for (int i = 0; i < argc; i++) {
    free(child_argv[i]);
  }
  free(child_argv);

  return pid;
}

int ps5_terminate_direct(int pid) {
  if (pid <= 0) {
    return -1;
  }
  // An untracked pid was already reaped (by the reaper thread); it may have
  // been recycled since, so it must not be signalled.
  pthread_mutex_lock(&g_children_lock);
  int tracked = untrack_locked(pid);
  pthread_mutex_unlock(&g_children_lock);
  if (!tracked) {
    return 0;
  }

  // SIGTERM first: a WebProcess holding GPU memory from the system compositor
  // must tell it Quit itself (src/ps5/glslim); SIGKILL after a second.
  kill(pid, SIGTERM);  // best-effort: it may already be a zombie

  // Signals are asynchronous: wait (bounded) for the child to become
  // reapable instead of a single WNOHANG that usually comes too early.
  int killed = 0;
  for (int waited = 0; waited < 1000 + PS5_SPAWN_KILL_TIMEOUT_MS; waited += 10) {
    if (!killed && waited >= 1000) {
      kill(pid, SIGKILL);
      killed = 1;
    }
    int status;
    if (try_reap(pid, &status)) {
      return 0;
    }
    sleep_ms(10);
  }
  ps5_log_printf("spawn: child pid=%d did not exit after SIGKILL", pid);
  track_child(pid);  // let the reaper collect it later
  return -1;
}
