// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process spawning contract implemented for the PS5 homebrew port. The WebKit
// static library declares ps5_spawn_self()/ps5_terminate_process() (see
// webkit-port/include/ps5webkit/spawn.h); this library defines them on top of
// an elfldr-derived loader (src/ps5/spawn/elfldr.c, GPL-3.0, see README.md).
//
// The pure (platform-independent) helpers in this header are compiled on the
// host as well, so they can be unit-tested without ptrace or the PS5 kernel.

#ifndef PROSPEROWEB_SPAWN_SPAWN_H
#define PROSPEROWEB_SPAWN_SPAWN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- The spawn contract (defined only on the PS5 target). -----------------

// Starts a new process running the current executable (the same multi-call
// ELF) with argv[0] as the role name, and places fds[i] at child fd 3+i.
// Returns the child pid, or -1 on failure. See webkit-port spawn.h.
int ps5_spawn_self(const char* const* argv, const int* fds, int nfds);

// Forcefully terminates and reaps a process created by ps5_spawn_self().
// Returns 0 on success, -1 on failure. Safe for an already-dead pid.
int ps5_terminate_process(int pid);

// Tries once to connect to the spawn broker, starting one through a loader if
// none is up (the same path a spawn takes), and returns non-zero if the broker
// is now connected. Safe to call repeatedly from a background thread: it holds
// the broker alive so WebKit's next process launch succeeds. The UI's startup
// error screen calls this so a loader or broker that appears late is picked up
// without relaunching the app. Returns 0 (and never blocks beyond the
// bounded connect window) when still unreachable.
int ps5_spawn_try_bring_up_broker(void);

// Runs the spawn broker role: the plain-payload process that performs the
// spawns for the UI (see src/ps5/spawn/README.md, "Spawn broker"). main()
// dispatches the "spawn-broker" role here. Returns the exit code.
int ps5_spawn_broker_main(void);

// Startup progress for the UI's loading screen: `event` is "broker" when
// the broker answered, the role name ("WebProcess", "NetworkProcess") when a
// child was spawned, or "no-loader:<reason>" when no ELF loader could be
// reached to start the broker (the UI then shows a message instead of a stuck
// progress bar). Called on the spawning thread (a WebKit work queue); the
// observer must be thread-safe. NULL removes it.
typedef void (*ps5_spawn_progress_fn)(const char* event, void* data);
void ps5_spawn_set_progress(ps5_spawn_progress_fn fn, void* data);

// Media control-socket rendezvous for the Media application-category sandbox,
// where the UI cannot listen() (docs/native-title.md). When both callbacks
// are set, ps5_spawn_self() gives every WebProcess an inherited AF_UNIX
// SOCK_SEQPACKET socket per media service: the child's end is named by
// PROSPEROWEB_AUDIO_CONTROL_FD / PROSPEROWEB_VIDEO_CONTROL_FD in its
// environment, and the UI end is handed to the matching callback (audio, then
// video) once the child is spawned. The callback takes ownership of the fd
// (AudioServer/VideoServer::add_control_fd). Called on the spawning thread;
// the callbacks must be thread-safe. Both NULL (the default) disables this and
// keeps the loopback-TCP path. See src/ps5/spawn/spawn_glue.cpp.
typedef void (*ps5_media_control_fn)(int fd, void* data);
void ps5_spawn_set_media_control(ps5_media_control_fn audio, ps5_media_control_fn video,
                                 void* data);

// GPU broker endpoint for the next WebProcess (EVO Player #237: EVO owns the
// GPU and submits for the WebProcess's Mesa). The engine host stores the
// endpoint EVO sent (fd ownership passes here; a second set replaces and
// closes the first). The next WebProcess spawn inherits it, named by
// PROSPEROWEB_GPU_FD in its environment, and the callback reports which pid
// took which cookie. With /data/prosperoweb/mesa present, a WebProcess spawn
// waits up to wait_ms for an endpoint that has not arrived yet.
typedef void (*ps5_gpu_client_fn)(int cookie, int pid, void* data);
void ps5_spawn_set_gpu_endpoint(int fd, int cookie);
void ps5_spawn_set_gpu_client_cb(ps5_gpu_client_fn fn, void* data, int wait_ms);

// --- Logging, provided by the executable (src/frontend/ps5/ps5_log.cpp). ---
//
// The spawner reports every spawn and every child-side failure through these,
// so they reach /data/prosperoweb/log.txt and not only stderr. Thread-safe:
// WebKit launches processes from a work queue thread.

void ps5_log_line(const char* line);
void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- Pure helpers (compiled on host and target; unit-tested on host). ------

// Maximum number of extra descriptors ps5_spawn_self() will forward. The IPC
// model only needs one (the SOCK_SEQPACKET end), but allow a small margin.
#define PS5_SPAWN_MAX_FDS 8

// Resolves the path of our own ELF image, used as the program to re-exec.
// Strategy, in order:
//   1. the PROSPEROWEB_SELF_PATH environment variable, if set and non-empty;
//   2. the fixed install path /data/prosperoweb/prosperoweb.elf;
//   3. argv0, but only if it is an absolute path (begins with '/').
// getenv_fn is the environment lookup (pass the real getenv; tests inject a
// stub). The result is copied into out_path (always NUL-terminated when the
// return value is 0). Returns 0 on success, -1 if nothing resolved or the
// path did not fit in out_len.
int ps5_spawn_resolve_self_path(const char* argv0,
                                const char* (*getenv_fn)(const char*),
                                char* out_path, size_t out_len);

// One entry of the child's extra fd table: a descriptor of a source process
// and the descriptor number it must appear as in the child. The loader
// copies it into the child with rdup(src_pid, parent_fd).
struct ps5_spawn_fd_entry {
  int parent_fd;  // descriptor number in the source process
  int child_fd;   // descriptor number in the spawned child (== 3 + index)
  int src_pid;    // process that owns parent_fd; 0 = the spawning process
};

// Builds the child fd table from the fds[]/nfds the launcher passed: each
// fds[i] (a descriptor of process src_pid, 0 = the caller) maps to child fd
// 3+i. Validates nfds against PS5_SPAWN_MAX_FDS and rejects negative
// descriptors and a negative src_pid. Writes up to out_max entries into out
// and returns their count, or -1 on error.
int ps5_spawn_build_fd_table(const int* fds, int nfds, int src_pid,
                             struct ps5_spawn_fd_entry* out, int out_max);

#ifdef __cplusplus
}
#endif

#endif  // PROSPEROWEB_SPAWN_SPAWN_H
