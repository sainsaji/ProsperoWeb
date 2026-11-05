// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5_glslim: see ps5_glslim.h. Offsets are for firmware 13.60 (measured on
// the console, EVO Player issue #237, docs/ui/browser.md "GPU compositing").
#include "ps5_glslim.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt,
                             int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);
int sceKernelGetIpcPath(const char* name, char* path);
int sceSysmoduleLoadModuleInternal(uint32_t id);  // -lSceSysmodule

#define GLSLIM_LOG "/data/prosperoweb/glslim.txt"
#define GLSLIM_SIZES "/data/prosperoweb/glslim_sizes"

// 13.60: the app info block the compositor reads (sceKernelGetAppInfo) sits at
// struct proc +0x460; its category word (+0x50) is proc +0x4b0. Payloads have
// 0xffffffff ("unknown"), Sony's NKWebProcess 0x01000000 (common_dialog).
#define PROC_APP_CATEGORY 0x4b0
#define CATEGORY_COMMON_DIALOG 0x01000000u

// libSceGLSlimVSH: its current-context getter. In a context: frame slots at
// +0x8e8 (0x200 bytes each), current index at +0x864. In a slot: +0x1d0 the
// label the GPU writes when the frame is done, +0x1d8 the value it writes,
// +0x1e0 the state (3 = submitted).
#define SLIM_CTX_FN 0x13920
// libSceCompositeExt: sceCompositorInitWithProcessOrder sits at +0x5180 (used
// to recognise the module); the ring's shared block pointer at +0x18030.
#define CEXT_INIT_OFF 0x5180

static int g_log_fd = -1;
static int g_slim = -1;  // module handle from sceKernelLoadStartModule
static int g_open_rc = 1;  // 1: not tried
static void* (*g_slim_ctx)(void);
static int (*g_comp_flush)(void);

static struct sockaddr_un g_comp_addr;
static int g_comp_addr_ok;
static volatile sig_atomic_t g_armed;
static volatile sig_atomic_t g_done;

static void raw(const char* s) {
  if (g_log_fd >= 0) {
    (void)write(g_log_fd, s, strlen(s));
  }
}

static void raw_hex(const char* label, unsigned long long v) {
  char b[24];
  int i = sizeof b;
  b[--i] = 0;
  do {
    b[--i] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while (v && i > 3);
  b[--i] = 'x';
  b[--i] = '0';
  raw(label);
  raw(b + i);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void ps5_glslim_log(const char* fmt, ...) {
  if (g_log_fd < 0) {
    return;
  }
  char line[512];
  int n = snprintf(line, sizeof line, "[%d %.1f] ", (int)getpid(), now_ms());
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) {
    m = 0;
  }
  n += m;
  if (n > (int)sizeof line - 2) {
    n = sizeof line - 2;
  }
  line[n++] = '\n';
  (void)write(g_log_fd, line, (size_t)n);
}

// --- Quit on every exit path ----------------------------------------------
// The service is reached over a Unix socket, one connection per request:
// path from sceKernelGetIpcPath("GnmCompositor"), a 16-byte header
// {1, command, payload bytes, status}. Quit is command 1 with no payload. The
// service knows the client by the socket's peer, so only this process can
// release its own slot. Plain syscalls only: this runs in signal handlers.
void ps5_glslim_bye(const char* why) {
  if (!g_armed || g_done) {
    return;
  }
  g_done = 1;
  raw("bye: ");
  raw(why);
  if (!g_comp_addr_ok) {
    raw(", no ipc path: CANNOT release\n");
    return;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    raw(", socket failed: CANNOT release\n");
    return;
  }
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  if (connect(fd, (const struct sockaddr*)&g_comp_addr, sizeof g_comp_addr) != 0) {
    close(fd);
    raw(", connect failed: CANNOT release\n");
    return;
  }
  const uint32_t hdr[4] = {1, 1, 0, 0};
  ssize_t w = write(fd, hdr, sizeof hdr);
  uint32_t rep[4] = {0, 0, 0, 0};
  ssize_t got = 0;
  struct pollfd p = {fd, POLLIN, 0};
  while (w == (ssize_t)sizeof hdr && got < (ssize_t)sizeof rep && poll(&p, 1, 2000) > 0) {
    ssize_t n = read(fd, (char*)rep + got, sizeof rep - got);
    if (n <= 0) {
      break;
    }
    got += n;
  }
  close(fd);
  raw(w == (ssize_t)sizeof hdr ? ", Quit sent" : ", Quit write FAILED");
  raw_hex(", reply bytes ", (unsigned long long)got);
  raw_hex(" status ", rep[3]);
  raw("\n");
}

static void on_signal(int sig) {
  raw_hex("glslim: signal ", (unsigned)sig);
  raw("\n");
  ps5_glslim_bye(sig == SIGTERM ? "SIGTERM" : "fatal signal");
  _exit(sig == SIGTERM ? 0 : 128 + sig);
}

static void on_exit_hook(void) {
  ps5_glslim_bye("exit()");
}

static void install_guards(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS, SIGTERM, SIGINT, SIGHUP};
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sa.sa_flags = SA_RESETHAND;  // a second fault in the handler kills us
  sigemptyset(&sa.sa_mask);
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; ++i) {
    sigaction(sigs[i], &sa, NULL);
  }
  atexit(on_exit_hook);
}

// --- Loading ----------------------------------------------------------------
static int load_system_module(const char* name) {
  static const char* dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
                               "/system_ex/priv_ex/lib/"};
  for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
    char path[160];
    snprintf(path, sizeof path, "%s%s", dirs[i], name);
    int res = 0;
    int h = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
    if (h >= 0) {
      ps5_glslim_log("loaded %s: handle %d", path, h);
      return h;
    }
  }
  ps5_glslim_log("could not load %s", name);
  return -1;
}

// sceKernelDlsym does not find GLSlim's exports by name, so they are looked
// up by NID: base64 of the first 8 bytes of SHA-1(name + salt), reversed,
// 11 characters, '/' -> '-'.
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1(const unsigned char* msg, size_t len, unsigned char out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  unsigned char block[64];
  size_t total = ((len + 8) / 64 + 1) * 64;
  for (size_t off = 0; off < total; off += 64) {
    for (int i = 0; i < 64; ++i) {
      size_t k = off + (size_t)i;
      block[i] = k < len ? msg[k] : k == len ? 0x80 : 0;
    }
    if (off + 64 == total) {
      unsigned long long bits = (unsigned long long)len * 8;
      for (int i = 0; i < 8; ++i) {
        block[63 - i] = (unsigned char)(bits >> (8 * i));
      }
    }
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
             block[4 * i + 3];
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    out[4 * i] = (unsigned char)(h[i] >> 24);
    out[4 * i + 1] = (unsigned char)(h[i] >> 16);
    out[4 * i + 2] = (unsigned char)(h[i] >> 8);
    out[4 * i + 3] = (unsigned char)h[i];
  }
}

static void nid_of(const char* name, char out[12]) {
  static const unsigned char salt[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                         0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  unsigned char buf[256], d[20], r[9];
  size_t n = strlen(name);
  if (n > sizeof buf - 16) {
    n = sizeof buf - 16;
  }
  memcpy(buf, name, n);
  memcpy(buf + n, salt, 16);
  sha1(buf, n + 16, d);
  for (int i = 0; i < 8; ++i) {
    r[i] = d[7 - i];
  }
  r[8] = 0;
  // 8 bytes -> 11 base64 characters (the last one from 2 bits)
  int o = 0;
  for (int i = 0; i < 9 && o < 11; i += 3) {
    uint32_t v = (uint32_t)r[i] << 16 | (uint32_t)(i + 1 < 9 ? r[i + 1] : 0) << 8 | (i + 2 < 9 ? r[i + 2] : 0);
    for (int j = 3; j >= 0 && o < 11; --j) {
      out[o++] = b64[(v >> (6 * j)) & 63];
    }
  }
  out[11] = 0;
}

void* ps5_glslim_proc(const char* name) {
  static uint32_t kh;
  static int have;
  if (g_slim < 0) {
    return NULL;
  }
  if (!have) {
    if (kernel_dynlib_handle(getpid(), "libSceGLSlimVSH.sprx", &kh) != 0) {
      return NULL;
    }
    have = 1;
  }
  char nid[12];
  nid_of(name, nid);
  return (void*)kernel_dynlib_resolve(getpid(), kh, nid);
}

// GLSlim's own dependency: sceCompositorFlush lives in libSceCompositeExt,
// which GLSlim loads. Its base comes from the kernel's module list.
static void find_composite_ext(void) {
  static const char* names[] = {"libSceCompositeExt.sprx", "libSceCompositeExt"};
  pid_t me = getpid();
  for (unsigned i = 0; i < sizeof names / sizeof names[0] && !g_comp_flush; ++i) {
    uint32_t h = 0;
    if (kernel_dynlib_handle(me, names[i], &h) != 0) {
      continue;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(me, h);
    intptr_t init = kernel_dynlib_resolve(me, h, "IUlpGnuoR1c");
    if (!init || init - base != CEXT_INIT_OFF) {
      ps5_glslim_log("%s: init at +%#lx, not the 13.60 layout: no flush", names[i], (long)(init - base));
      continue;
    }
    g_comp_flush = (int (*)(void))kernel_dynlib_resolve(me, h, "qZNF03+ghLI");  // sceCompositorFlush
  }
  uint32_t h = 0;
  if (kernel_dynlib_handle(me, "libSceGLSlimVSH.sprx", &h) == 0) {
    g_slim_ctx = (void* (*)(void))(kernel_dynlib_mapbase_addr(me, h) + SLIM_CTX_FN);
  }
  ps5_glslim_log("flush %p, slim ctx getter %p", (void*)g_comp_flush, (void*)g_slim_ctx);
}

int ps5_glslim_open(void) {
  if (g_open_rc != 1) {
    return g_open_rc;
  }
  g_open_rc = -1;
  g_log_fd = open(GLSLIM_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
  {
    char nid[12];
    nid_of("sceCompositorFlush", nid);
    ps5_glslim_log("open: start (nid check %s, want qZNF03+ghLI)", nid);
    if (strcmp(nid, "qZNF03+ghLI") != 0) {
      return -1;
    }
  }

  static char ipc[256];
  memset(ipc, 0, sizeof ipc);
  int r = sceKernelGetIpcPath("GnmCompositor", ipc);
  memset(&g_comp_addr, 0, sizeof g_comp_addr);
  g_comp_addr.sun_family = AF_UNIX;
  strncpy(g_comp_addr.sun_path, ipc, sizeof g_comp_addr.sun_path - 1);
  g_comp_addr_ok = r == 0 && ipc[0];
  ps5_glslim_log("compositor ipc path \"%s\" (rc %#x)", ipc, (unsigned)r);
  if (!g_comp_addr_ok) {
    return -1;  // no way to say Quit later: do not start
  }

  // GLSlim's three system modules and the one the compositor client loads,
  // as ourselves (under another identity they fail with 0x80020002).
  static const uint32_t mods[] = {0x80000086, 0x8000008b, 0x80000004, 0x8000001f};
  for (unsigned i = 0; i < 4; ++i) {
    ps5_glslim_log("sysmodule %#x: %#x", mods[i], (unsigned)sceSysmoduleLoadModuleInternal(mods[i]));
  }

  pid_t me = getpid();
  intptr_t proc = kernel_get_proc(me);
  uint32_t cat = 0, want = CATEGORY_COMMON_DIALOG;
  if (!proc || kernel_copyout(proc + PROC_APP_CATEGORY, &cat, sizeof cat) != 0) {
    ps5_glslim_log("no proc / kernel access");
    return -1;
  }
  int pr = kernel_copyin(&want, proc + PROC_APP_CATEGORY, sizeof want);
  ps5_glslim_log("category %#x -> %#x (rc %d)", cat, want, pr);

  g_slim = load_system_module("libSceGLSlimVSH.sprx");
  if (g_slim < 0) {
    return -1;
  }
  int (*set_cfg)(const void*) = (int (*)(const void*))ps5_glslim_proc("scePigletSetConfigurationVSH");
  if (!set_cfg) {
    ps5_glslim_log("no scePigletSetConfigurationVSH");
    return -1;
  }
  // GLSlim's configuration as Sony's NKWebProcess builds it (0xa8 bytes,
  // its allocator callbacks left out: GLSlim then uses its own heaps).
  static unsigned char cfg[0xa8];
  memset(cfg, 0, sizeof cfg);
  unsigned long long sys = 0x200000, vid = 0x3000000;  // 48 MB: 64 MB is refused
  FILE* f = fopen(GLSLIM_SIZES, "r");
  if (f) {
    unsigned long long s = 0, v = 0;
    if (fscanf(f, "%llx %llx", &s, &v) == 2) {
      sys = s;
      vid = v;
    }
    fclose(f);
  }
  *(uint32_t*)(cfg + 0x00) = sizeof cfg;
  *(uint32_t*)(cfg + 0x04) = 0x78;
  *(uint32_t*)(cfg + 0x08) = 1;
  *(uint64_t*)(cfg + 0x18) = sys;
  *(uint64_t*)(cfg + 0x28) = vid;
  *(uint64_t*)(cfg + 0x30) = 0xa000000;
  *(uint32_t*)(cfg + 0x40) = 1920;
  *(uint32_t*)(cfg + 0x44) = 1080;
  *(uint64_t*)(cfg + 0x54) = 0x10000000c0000ull;
  *(uint32_t*)(cfg + 0x5c) = 2;
  // Piglet calls return a bool in the low byte.
  int cr = set_cfg(cfg);
  ps5_glslim_log("configuration: system %#llx video %#llx: %d", sys, vid, cr & 0xff);

  // eglGetDisplay runs GLSlim's library init, which connects to the
  // compositor: from here on every exit says Quit.
  install_guards();
  g_armed = 1;
  g_open_rc = 0;
  return 0;
}

// --- Frame end --------------------------------------------------------------
long ps5_glslim_frame_end(void* dpy, void* surf) {
  static EGLBoolean (*swap)(EGLDisplay, EGLSurface);
  if (!swap) {
    swap = (EGLBoolean(*)(EGLDisplay, EGLSurface))ps5_glslim_proc("eglSwapBuffers");
    find_composite_ext();
  }
  if (!swap) {
    return -1;
  }
  unsigned char* slot = NULL;
  if (g_slim_ctx) {
    unsigned char* ctx = (unsigned char*)g_slim_ctx();
    if (ctx) {
      unsigned idx = *(unsigned*)(ctx + 0x864);
      slot = *(unsigned char**)(ctx + 0x8e8) + (size_t)idx * 0x200;
    }
  }
  swap((EGLDisplay)dpy, (EGLSurface)surf);
  if (g_comp_flush) {
    g_comp_flush();
  }
  if (!slot) {
    return -1;
  }
  volatile uint64_t* label = *(volatile uint64_t**)(slot + 0x1d0);
  uint64_t target = *(uint64_t*)(slot + 0x1d8);
  if (!label) {
    return -1;
  }
  double t0 = now_ms();
  while (*label < target) {
    if (now_ms() - t0 > 500) {
      return -1;
    }
    sched_yield();
  }
  return (long)((now_ms() - t0) * 1000.0);
}

// --- Self-test --------------------------------------------------------------
#define P(name) __typeof__(&name) p_##name = (__typeof__(&name))ps5_glslim_proc(#name)

int ps5_glslim_selftest(void) {
  if (ps5_glslim_open() != 0) {
    return -1;
  }
  P(eglGetDisplay);
  P(eglInitialize);
  P(eglBindAPI);
  P(eglChooseConfig);
  P(eglCreatePbufferSurface);
  P(eglCreateContext);
  P(eglMakeCurrent);
  P(eglGetError);
  P(glGetString);
  P(glGenTextures);
  P(glBindTexture);
  P(glTexImage2D);
  P(glTexParameteri);
  P(glGenFramebuffers);
  P(glBindFramebuffer);
  P(glFramebufferTexture2D);
  P(glCheckFramebufferStatus);
  P(glViewport);
  P(glClearColor);
  P(glClear);
  P(glReadPixels);
  P(glGetError);
  if (!p_eglGetDisplay || !p_eglInitialize || !p_eglCreatePbufferSurface || !p_glReadPixels || !p_glFramebufferTexture2D) {
    ps5_glslim_log("selftest: missing exports");
    return -1;
  }

  double t0 = now_ms();
  EGLDisplay dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj = 0, min = 0;
  EGLBoolean ok = dpy != EGL_NO_DISPLAY && p_eglInitialize(dpy, &maj, &min);
  ps5_glslim_log("selftest: display %p init %d EGL %d.%d (%.1f ms) error %#x", dpy, ok, maj, min, now_ms() - t0,
                 p_eglGetError());
  if (!ok) {
    return -1;
  }
  p_eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!p_eglChooseConfig(dpy, want, &cfg, 1, &n) || n < 1) {
    ps5_glslim_log("selftest: no config");
    return -1;
  }
  const EGLint pb[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  EGLSurface surf = p_eglCreatePbufferSurface(dpy, cfg, pb);
  const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
  ok = surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && p_eglMakeCurrent(dpy, surf, surf, ctx);
  ps5_glslim_log("selftest: pbuffer %p context %p current %d error %#x", surf, ctx, ok, p_eglGetError());
  if (!ok) {
    return -1;
  }
  ps5_glslim_log("selftest: GL %s / %s", (const char*)p_glGetString(GL_RENDERER), (const char*)p_glGetString(GL_VERSION));

  const int W = 1792, H = 944;
  GLuint tex = 0, fbo = 0;
  p_glGenTextures(1, &tex);
  p_glBindTexture(GL_TEXTURE_2D, tex);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  p_glGenFramebuffers(1, &fbo);
  p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  ps5_glslim_log("selftest: fbo %ux%u status %#x error %#x", W, H, p_glCheckFramebufferStatus(GL_FRAMEBUFFER),
                 p_glGetError());
  p_glViewport(0, 0, W, H);

  unsigned char* px = (unsigned char*)malloc((size_t)W * H * 4);
  if (!px) {
    return -1;
  }
  int stale = 0, timeouts = 0;
  double wait_sum = 0, read_sum = 0, t_loop = now_ms();
  for (int i = 0; i < 30; ++i) {
    const unsigned char red = (unsigned char)(i * 8 + 7);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glClearColor(red / 255.0f, 0.5f, 0.25f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    long us = ps5_glslim_frame_end(dpy, surf);
    if (us < 0) {
      timeouts++;
    } else {
      wait_sum += us / 1000.0;
    }
    double tr = now_ms();
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    read_sum += now_ms() - tr;
    const unsigned char* mid = px + ((size_t)(H / 2) * W + W / 2) * 4;
    int d = (int)mid[0] - (int)red;
    if (d < -2 || d > 2) {
      stale++;
      if (stale <= 3) {
        ps5_glslim_log("selftest: frame %d stale: red %#x want %#x", i, mid[0], red);
      }
    }
  }
  free(px);
  ps5_glslim_log("selftest: 30 frames in %.1f ms, label wait avg %.2f ms, readback avg %.2f ms, %d stale, %d timeouts",
                 now_ms() - t_loop, wait_sum / 30, read_sum / 30, stale, timeouts);
  // Give everything back: WebKit makes its own contexts from the same 48 MB.
  P(glDeleteFramebuffers);
  P(glDeleteTextures);
  P(eglDestroyContext);
  P(eglDestroySurface);
  p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (p_glDeleteFramebuffers && p_glDeleteTextures) {
    p_glDeleteFramebuffers(1, &fbo);
    p_glDeleteTextures(1, &tex);
  }
  p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (p_eglDestroyContext && p_eglDestroySurface) {
    p_eglDestroyContext(dpy, ctx);
    p_eglDestroySurface(dpy, surf);
  }
  return stale || timeouts ? -1 : 0;
}
