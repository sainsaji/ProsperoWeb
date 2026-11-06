// SPDX-License-Identifier: GPL-3.0-or-later
// Mesa in the WebProcess over EVO Player's GPU broker (ps5_mesa.h).
#include "ps5_mesa.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The prebuilt stack (ps5_mesa_gpu.o).
void* mesa_eglGetProcAddress(const char* name);
int ps5_gpu_client_bind(int fd, bool reserved);

int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align);

// src/frontend/ps5/ps5_cpu_affinity.cpp (the pthread_create wrap).
void ps5_thread_set_min_stack(size_t bytes) __attribute__((weak));

#define MESA_FLAG "/data/prosperoweb/mesa"
#define MESA_LOG "/data/prosperoweb/mesa.txt"

static int g_log = -1;
static int g_reserved;   // the broker's client ranges are held
static int g_active;

void ps5_mesa_log(const char* fmt, ...) {
  if (g_log < 0) {
    mkdir("/data/prosperoweb", 0777);
    g_log = open(MESA_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (g_log < 0) {
      return;
    }
  }
  char line[1024];
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int n = snprintf(line, sizeof line, "[%ld.%03ld pid %d] ", (long)ts.tv_sec, ts.tv_nsec / 1000000L,
                   (int)getpid());
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) {
    return;
  }
  n += m < (int)(sizeof line - n - 1) ? m : (int)(sizeof line - n - 2);
  line[n++] = '\n';
  (void)write(g_log, line, (size_t)n);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int wanted(void) {
  struct stat st;
  return getenv("PROSPEROWEB_GPU_FD") && stat(MESA_FLAG, &st) == 0;
}

// The broker's clients map its shared memory at fixed addresses
// (PS5_Vulkan protocol.h): hold them before anything else maps there. A
// constructor, so it runs before WebKit's own static initialisers.
static void reserve_ranges(void) {
  static const uintptr_t base[2] = {0x240000000ull, 0x3000000000ull};
  static const size_t bytes[2] = {512ull << 20, 16ull << 30};
  int ok = 1;
  for (int i = 0; i < 2; i++) {
    void* at = (void*)base[i];
    const int rc = sceKernelReserveVirtualRange(&at, bytes[i], 0x10 /* fixed */ | 0x80 /* no overwrite */,
                                                16384);
    if (rc != 0 || at != (void*)base[i]) {
      ok = 0;
    }
    ps5_mesa_log("reserve %#lx + %zu MB -> %#x at %p", (unsigned long)base[i], bytes[i] >> 20,
                 (unsigned)rc, at);
  }
  g_reserved = ok;
}

__attribute__((constructor(101))) static void ps5_mesa_reserve_early(void) {
  if (wanted()) {
    reserve_ranges();
  }
}

// --- the self-test, through Mesa's own entry points -------------------------

typedef void* EGLDisplayP;
typedef void* EGLContextP;
typedef int EGLint_;
typedef unsigned GLenum_;
typedef unsigned GLuint_;
typedef int GLint_;

#define EGL_NONE_ 0x3038
#define EGL_VENDOR_ 0x3053
#define EGL_VERSION_ 0x3054
#define EGL_EXTENSIONS_ 0x3055
#define EGL_OPENGL_ES_API_ 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION_ 0x3098
#define EGL_SURFACE_TYPE_ 0x3033
#define EGL_PBUFFER_BIT_ 0x0001
#define EGL_RENDERABLE_TYPE_ 0x3040
#define EGL_OPENGL_ES2_BIT_ 0x0004
#define EGL_RED_SIZE_ 0x3024
#define EGL_GREEN_SIZE_ 0x3023
#define EGL_BLUE_SIZE_ 0x3022
#define EGL_ALPHA_SIZE_ 0x3021
#define GL_VERSION_ 0x1F02
#define GL_RENDERER_ 0x1F01
#define GL_SHADING_LANGUAGE_VERSION_ 0x8B8C
#define GL_FRAGMENT_SHADER_ 0x8B30
#define GL_COMPILE_STATUS_ 0x8B81
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_RGBA8_ 0x8058
#define GL_RGBA_ 0x1908
#define GL_UNSIGNED_BYTE_ 0x1401
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_COLOR_BUFFER_BIT_ 0x4000

#define FUNCS(X)                                                                       \
  X(EGLDisplayP, eglGetDisplay, (void*))                                               \
  X(unsigned, eglInitialize, (EGLDisplayP, EGLint_*, EGLint_*))                        \
  X(EGLint_, eglGetError, (void))                                                      \
  X(const char*, eglQueryString, (EGLDisplayP, EGLint_))                               \
  X(unsigned, eglBindAPI, (unsigned))                                                  \
  X(unsigned, eglChooseConfig, (EGLDisplayP, const EGLint_*, void**, EGLint_, EGLint_*)) \
  X(EGLContextP, eglCreateContext, (EGLDisplayP, void*, EGLContextP, const EGLint_*))   \
  X(unsigned, eglMakeCurrent, (EGLDisplayP, void*, void*, EGLContextP))                \
  X(unsigned, eglDestroyContext, (EGLDisplayP, EGLContextP))                           \
  X(const unsigned char*, glGetString, (GLenum_))                                      \
  X(GLenum_, glGetError, (void))                                                       \
  X(GLuint_, glCreateShader, (GLenum_))                                                \
  X(void, glShaderSource, (GLuint_, int, const char* const*, const GLint_*))           \
  X(void, glCompileShader, (GLuint_))                                                  \
  X(void, glGetShaderiv, (GLuint_, GLenum_, GLint_*))                                  \
  X(void, glGetShaderInfoLog, (GLuint_, int, int*, char*))                             \
  X(void, glDeleteShader, (GLuint_))                                                   \
  X(void, glGenTextures, (int, GLuint_*))                                              \
  X(void, glDeleteTextures, (int, const GLuint_*))                                     \
  X(void, glBindTexture, (GLenum_, GLuint_))                                           \
  X(void, glTexStorage2D, (GLenum_, int, GLenum_, int, int))                           \
  X(void, glGenFramebuffers, (int, GLuint_*))                                          \
  X(void, glDeleteFramebuffers, (int, const GLuint_*))                                 \
  X(void, glBindFramebuffer, (GLenum_, GLuint_))                                       \
  X(void, glFramebufferTexture2D, (GLenum_, GLenum_, GLenum_, GLuint_, GLint_))        \
  X(GLenum_, glCheckFramebufferStatus, (GLenum_))                                      \
  X(void, glViewport, (GLint_, GLint_, int, int))                                      \
  X(void, glClearColor, (float, float, float, float))                                  \
  X(void, glClear, (unsigned))                                                         \
  X(void, glReadPixels, (GLint_, GLint_, int, int, GLenum_, GLenum_, void*))           \
  X(void, glFinish, (void))

#define DECL(ret, name, args) static ret(*p_##name) args;
FUNCS(DECL)
#undef DECL

static int load_funcs(void) {
  int ok = 1;
#define LOAD(ret, name, args)                                   \
  p_##name = (ret(*) args)mesa_eglGetProcAddress(#name);        \
  if (!p_##name) {                                              \
    ps5_mesa_log("self-test: Mesa has no %s", #name);           \
    ok = 0;                                                     \
  }
  FUNCS(LOAD)
#undef LOAD
  return ok;
}

static int selftest(void) {
  if (!load_funcs()) {
    return -1;
  }
  double t0 = now_ms();
  EGLDisplayP dpy = p_eglGetDisplay((void*)0);  // EGL_DEFAULT_DISPLAY: Mesa's "videoout" platform
  EGLint_ major = 0, minor = 0;
  if (!dpy || !p_eglInitialize(dpy, &major, &minor)) {
    ps5_mesa_log("self-test: EGL init failed: %#x", p_eglGetError());
    return -2;
  }
  ps5_mesa_log("self-test: EGL %d.%d (%s) in %.1f ms", major, minor, p_eglQueryString(dpy, EGL_VENDOR_),
               now_ms() - t0);
  ps5_mesa_log("self-test: EGL extensions: %s", p_eglQueryString(dpy, EGL_EXTENSIONS_));
  // What WebKit's GLContext asks for (surfaceless contexts take a pbuffer config).
  const EGLint_ cfg_attrs[] = {EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RENDERABLE_TYPE_, EGL_OPENGL_ES2_BIT_,
                               EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8,
                               EGL_NONE_};
  EGLint_ nconf = 0;
  void* config = 0;
  p_eglChooseConfig(dpy, cfg_attrs, &config, 1, &nconf);
  ps5_mesa_log("self-test: pbuffer RGBA8 ES2 configs: %d", nconf);

  p_eglBindAPI(EGL_OPENGL_ES_API_);
  const EGLint_ ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION_, 3, EGL_NONE_};
  EGLContextP ctx = p_eglCreateContext(dpy, nconf > 0 ? config : 0, 0, ctx_attrs);
  if (!ctx || !p_eglMakeCurrent(dpy, 0, 0, ctx)) {
    ps5_mesa_log("self-test: surfaceless ES 3 context failed: %#x", p_eglGetError());
    return -3;
  }
  ps5_mesa_log("self-test: GL_VERSION %s | GL_RENDERER %s | GLSL %s", (const char*)p_glGetString(GL_VERSION_),
               (const char*)p_glGetString(GL_RENDERER_),
               (const char*)p_glGetString(GL_SHADING_LANGUAGE_VERSION_));

  int rc = 0;
  const char* fs =
      "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0, 0.5, 0.25, 1.0); }\n";
  t0 = now_ms();
  GLuint_ s = p_glCreateShader(GL_FRAGMENT_SHADER_);
  p_glShaderSource(s, 1, &fs, 0);
  p_glCompileShader(s);
  GLint_ compiled = 0;
  p_glGetShaderiv(s, GL_COMPILE_STATUS_, &compiled);
  char info[256] = {0};
  p_glGetShaderInfoLog(s, sizeof info, 0, info);
  ps5_mesa_log("self-test: GLSL ES 3.00 compile -> %d in %.1f ms %s", compiled, now_ms() - t0, info);
  p_glDeleteShader(s);
  if (!compiled) {
    rc = -4;
  }

  GLuint_ tex = 0, fbo = 0;
  p_glGenTextures(1, &tex);
  p_glBindTexture(GL_TEXTURE_2D_, tex);
  p_glTexStorage2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, 64, 64);
  p_glGenFramebuffers(1, &fbo);
  p_glBindFramebuffer(GL_FRAMEBUFFER_, fbo);
  p_glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, tex, 0);
  const GLenum_ status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER_);
  p_glViewport(0, 0, 64, 64);
  p_glClearColor(0.f, 1.f, 0.f, 1.f);
  p_glClear(GL_COLOR_BUFFER_BIT_);
  ps5_mesa_log("self-test: FBO status %#x, cleared; reading back", status);
  uint32_t px[64] = {0};
  t0 = now_ms();
  p_glReadPixels(0, 0, 8, 8, GL_RGBA_, GL_UNSIGNED_BYTE_, px);
  const GLenum_ err = p_glGetError();
  ps5_mesa_log("self-test: FBO status %#x, clear read back %#x (want 0xff00ff00) in %.2f ms, GL error %#x", status,
               px[0], now_ms() - t0, err);
  if (status != GL_FRAMEBUFFER_COMPLETE_ || px[0] != 0xff00ff00u || err != 0) {
    rc = rc ? rc : -5;
  }
  p_glBindFramebuffer(GL_FRAMEBUFFER_, 0);
  p_glDeleteFramebuffers(1, &fbo);
  p_glDeleteTextures(1, &tex);
  p_glFinish();
  p_eglMakeCurrent(dpy, 0, 0, 0);
  p_eglDestroyContext(dpy, ctx);
  return rc;
}

int ps5_mesa_start(void) {
  if (g_active) {
    return 0;
  }
  if (!wanted()) {
    return -1;
  }
  const int fd = atoi(getenv("PROSPEROWEB_GPU_FD"));
  if (!g_reserved) {
    reserve_ranges();  // late: the constructor did not see the flag
  }
  ps5_mesa_log("start: endpoint fd %d, ranges reserved %d", fd, g_reserved);
  if (fd < 3) {
    return -1;
  }
  // Before Mesa makes its first thread.
  if (ps5_thread_set_min_stack) {
    ps5_thread_set_min_stack((size_t)2 << 20);
  }
  double t0 = now_ms();
  const int bind = ps5_gpu_client_bind(fd, g_reserved != 0);
  ps5_mesa_log("start: ps5_gpu_client_bind -> %d in %.1f ms", bind, now_ms() - t0);
  if (bind != 0) {
    return -1;
  }
  setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
  setenv("GALLIUM_THREAD", "0", 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
  t0 = now_ms();
  const int rc = selftest();
  ps5_mesa_log("start: self-test %s (%d) in %.1f ms", rc == 0 ? "passed" : "FAILED", rc, now_ms() - t0);
  if (rc != 0) {
    return -1;
  }
  g_active = 1;
  return 0;
}

int ps5_mesa_active(void) { return g_active; }

void* ps5_mesa_proc(const char* name) { return g_active && name ? mesa_eglGetProcAddress(name) : NULL; }
