// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5_glslim: Sony's GLES 2 library (libSceGLSlimVSH, the one the system
// browser's WebProcess composites with) brought up inside a payload process.
//
// GLSlim's init asks the system compositor service (AgcCompositor) for GPU
// memory. The service only serves known app categories, so the process's
// category (struct proc +0x4b0 on 13.60) is set to common_dialog first; the
// process keeps its own authid (a borrowed one makes every direct-memory map
// fail). The service then grants up to 48 MB.
//
// The service keeps a client's slot until the client sends Quit itself, and a
// slot left behind needs a console reboot. Once ps5_glslim_open() has started
// GLSlim, every exit path (exit(), SIGTERM, crash signals) sends Quit:
// ps5_glslim_bye(). A SIGKILL cannot be caught, so whoever stops this process
// must send SIGTERM first and give it a moment.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Loads the library, sets the process category and GLSlim's configuration,
// installs the exit guards. Returns 0 when GL calls may be made, -1 if not
// (logged). Call once, from the main thread, before any other thread starts
// using GL. Logs to /data/prosperoweb/glslim.txt.
int ps5_glslim_open(void);

// Address of a GLSlim export (EGL / GLES name), NULL before ps5_glslim_open()
// succeeded or for a name the library does not have.
void* ps5_glslim_proc(const char* name);

// GLSlim's frame end. eglSwapBuffers is GLSlim's frame boundary; its submit
// leaves the frame's last commands unflushed, and its wait for a reused frame
// slot never wakes for a payload. So: take the current slot, swap, flush the
// compositor ring, then wait (bounded) until the GPU wrote the slot's label.
// Returns the wait in microseconds, or -1 when the label did not arrive.
long ps5_glslim_frame_end(void* egl_display, void* egl_surface);

// Sends Quit to the compositor service if this process may hold a slot.
// Async-signal-safe; runs once.
void ps5_glslim_bye(const char* why);

// Milestone check: a pbuffer context, an FBO, 30 frames of clears ended with
// ps5_glslim_frame_end() and read back. Logs timings; returns 0 when every
// readback showed its own frame's colour.
int ps5_glslim_selftest(void);

// printf into the GLSlim log (not signal-safe).
void ps5_glslim_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
