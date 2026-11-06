// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5_mesa: Mesa (EGL 1.5 + OpenGL ES 3.2 over Zink on the broker-client RADV)
// in the WebProcess, with EVO Player as the GPU owner (EVO Player #237).
//
// A payload cannot drive the GPU itself. EVO Player runs PS5_Vulkan's GPU
// broker: this process builds RADV command buffers in memory EVO shares with
// it, and EVO submits them. The engine host passes EVO's endpoint to the
// WebProcess as an inherited fd named by PROSPEROWEB_GPU_FD.
//
// On only when /data/prosperoweb/mesa exists and PROSPEROWEB_GPU_FD is set.
// The Mesa stack is one prebuilt object (EVO Player tools/build_mesa_bundle.sh,
// Prospero Browser's GPU stack: Mesa/RADV MIT, broker client LGPL-2.1+,
// libps5platform GPL-3.0+) whose only exports are mesa_eglGetProcAddress and
// ps5_gpu_client_*. webkit-port/stubs resolve every EGL/GLES call through
// ps5_mesa_proc() first, then GLSlim.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Binds the endpoint, sets Mesa's environment (Zink, no shader cache, no
// gallium thread), raises new threads' stacks to 2 MiB, and runs a self-test
// (EGL display, ES 3 context, a GLSL ES 3.00 shader, a clear read back).
// 0 when WebKit may use it. Call once on the main thread before WebKit starts.
// Logs to /data/prosperoweb/mesa.txt.
int ps5_mesa_start(void);

// 1 once ps5_mesa_start() succeeded.
int ps5_mesa_active(void);

// An EGL or GLES entry point from Mesa, NULL before ps5_mesa_start() succeeded.
void* ps5_mesa_proc(const char* name);

// printf into the Mesa log (not signal-safe).
void ps5_mesa_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
