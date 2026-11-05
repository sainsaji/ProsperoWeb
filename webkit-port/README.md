# webkit-port

Overlay that builds the upstream WebKit **PlayStation port** (`-DPORT=PlayStation`) with the open [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) instead of Sony's SDK. Upstream WebKit is fetched at a pinned commit into `third_party/webkit/` (never committed) and patched with the files in `patches/`. All patches are guarded by `PS5_HOMEBREW`, so the upstream behaviour is unchanged without it.

## Layout

| Path | Purpose |
|---|---|
| `include/ps5webkit/entry.h` | `ps5_webkit_{web,network}_process_main()`: auxiliary process entry points, implemented in libWebKit, called by the shell's `main()` |
| `include/ps5webkit/spawn.h` | `ps5_spawn_self()` / `ps5_terminate_process()`: called by the WebKit process launcher, implemented by the shell |
| `cmake/PlayStationModule.cmake`, `cmake/Sign.cmake` | no-op stand-ins for Sony's proprietary CMake modules |
| `cmake/WebKitRequirementsConfig.cmake` | stand-in for Sony's `WebKitRequirements` package: empty `WebKitResources`/`ProcessLauncher`, `libwpe` (+ `WPE::libwpe`) from the dependency prefix |
| `cmake/Findlibdl.cmake` | reports libdl as absent (the SDK libc has `dlopen`) |
| `cmake/ps5-project-include.cmake` | appends `cmake/` to `CMAKE_MODULE_PATH` after `project(WebKit)` |
| `toolchain/ps5.cmake`, `toolchain/ps5-clang{,++}` | SDK toolchain with compiler wrappers (no `crt1.o` on `-E` probes) and `CMAKE_FIND_PACKAGE_PREFER_CONFIG` off |
| `compat/libcxx18-compat.h` | force-included `std::atomic_ref` for the SDK's libc++ 18 |
| `stubs/` | generated EGL/GLESv2 stubs (`libEGL.a`, `libGLESv2.a`): every call fails, see below |
| `patches/` | WebKit patches, applied in order by `scripts/ps5/fetch-webkit.sh` |
| `smoke/wk_smoke.cpp` | link test of the WebKit C API against the static libraries |
| `media/` | source of truth of the ffmpeg media engine (patch 0900), see `media/README.md` |

## Patches

| Patch | Why |
|---|---|
| `0001-cmake-playstation-homebrew-options` | `OptionsPlayStation.cmake`: `PS5_HOMEBREW` option; no `-sce-stdlib=v2`, no forced `-g`, no libc/libkernel lookup, no `--wrap=mmap`/`sce_sys` copy, `ENABLE_RESOURCE_USAGE` off, `ENABLE_GAMEPAD` allowed without the WPE backend, static `libWebKit` and no auxiliary executables; the `USE_FFMPEG` option of patch 0900; `USE_THEME_ADWAITA` (patch 0012); `ENABLE_USER_MESSAGE_HANDLERS` on (native `window.webkit.messageHandlers` for the shell's page bridge) |
| `0002-wtf-posix-osallocator` | `OSAllocatorPOSIX.cpp` instead of the memory-extra based PlayStation allocator |
| `0003-jsc-generic-shell` | generic `jsc.cpp` shell (no Sony libtestwrappers), MemoryExtra only if found |
| `0004-webcore-curl-ca-bundle` | TLS roots from a PEM file (`$PROSPEROWEB_CA_BUNDLE`, default `/data/prosperoweb/cacert.pem`) instead of Sony's CertificateStore |
| `0005-webkit-static-multicall-processes` | launcher calls `ps5_spawn_self()`/`ps5_terminate_process()` (a failed spawn is logged and reported as failed-to-launch); entry points become `ps5_webkit_*_process_main()` inside libWebKit; no WebProcess/NetworkProcess executables; `WKRunLoop` file name case |
| `0006-wtf-payload-sdk-compat` | no Sony libc extensions (`getenv_np`, `SO_NBIO`, `<sys/sysinfo.h>`, `<malloc.h>`); no `[[clang::suppress]]` on functions with clang 18 |
| `0007-webcore-playstation-build-fixes` | upstream PlayStation+Skia build breaks (`PlatformDisplaySkia` per-thread GL context, `HardwareAccelerationEnabled` setting), `AF_INET` on FreeBSD |
| `0008-skia-freebsd-malloc-usable-size` | `malloc_usable_size()` from `<malloc_np.h>` on FreeBSD |
| `0009-webkit-static-injected-bundle` | injected bundle = weak `WKBundleInitialize` linked into the executable (no Sony `library-bundle.h`) |
| `0010-webcore-timing-function-destructors` | out-of-line `*TimingFunction` destructors: clang 18 / PS5 target leaves the vtable-referenced destructors undefined |
| `0011-webkit-playstation-page-api` | `WKPagePrivatePlayStation`: `WKPageSetDrawsTransparentBackground()` (transparent chrome view); `WKPageGetWebPageIdentifierPlayStation()` (the page's WebCore `PageIdentifier`, which the video overlay's players report); the Skia background fill in `WKPagePaint` clipped to the unpainted rect; `WKPageHandleKeyboardEvent` forwards modifiers and event text (keysym in `virtualKeyCode`); `WKPageHandleWheelEvent` sends one precise-pixel 2D event, none for a zero delta; `WKPageHandleMouseEvent` gives events a real monotonic time (libwpe derives `clickCount` from it) and the pressed-button bits (`MouseEvent.buttons`); `PlayStationWebView` records element fullscreen when the client accepts it (exit callbacks and `WKViewRequestExitFullScreen` work) |
| `0012-webcore-playstation-adwaita-theme` | form controls and scrollbars drawn by the Adwaita theme (`USE_THEME_ADWAITA`, set by patch 0001; `platform/Adwaita.cmake` and `themeAdwaita.css` included from `WebCore/CMakeLists.txt`): the upstream PlayStation `RenderTheme`/`Theme` are empty and `ControlFactory` falls back to `EmptyControlFactory`; the PlayStation theme singletons are compiled out, Adwaita's media controls and the coordinated async scrollbars (they need async scrolling) stay off |
| `0013-webcore-no-gl-compositing` | no accelerated compositing without a GL display: `RenderLayerCompositor::enableCompositingMode()` refuses when `AcceleratedCompositingEnabled` is off (view transitions forced it, and the drawing area's `LayerTreeHost` hit the `PlatformDisplay::sharedDisplay()` RELEASE_ASSERT), view transitions are skipped (update callback still runs), `DrawingAreaCoordinatedGraphics` never builds a `LayerTreeHost` without a shared display, accelerated buffers off in the WebProcess |
| `0014-wtf-redblacktree-unlink-removed-node` | `RedBlackTree::remove()` clears the removed node's `CheckedPtr` links: stale links made freeing a timer of the generic `RunLoop` crash in `crashDueToCheckedPtrToDeadObject()` (UI process died when a crashed WebProcess's proxy went away) |
| `0015-webcore-skia-last-resort-font` | `FontCache::lastResortFallbackFont()` never builds the font data cache's empty key: when every fontconfig lookup fails (console under memory pressure), it falls back to `DejaVu Sans`, a typeface reserved at the first call, then the packaged `$FONTCONFIG_PATH/DejaVuSans.ttf`; a glyphless font at size 0 gets a non-zero size |
| `0016-webkit-playstation-user-agent` | `WebPage::platformUserAgent()` no longer returns the port's static user agent for every URL (it overrode `WKPageSetCustomUserAgent()` in the WebProcess, so `navigator.userAgent` and the `User-Agent` header stayed `PlayStation/0.00`); the built-in default is a desktop Safari string |
| `0022-webkit-ps5-glslim-compositing` | GPU compositing on Sony's GLES library (`libSceGLSlimVSH`), applied after every other patch (also touches 0013's files; not in `export-webkit-patches.sh`'s topics). With `PROSPEROWEB_GPU_READY=1` (set by the shell after `src/ps5/glslim`'s self-test drew frames) the WebProcess gets a shared display; `GLContext` makes pbuffer contexts and ends frames with the shell's `ps5_glslim_frame_end`; `TextureMapperShaderProgram` loads the console's own precompiled TextureMapper binaries (`$PROSPEROWEB_SHADER_DIR`, default `/data/prosperoweb/shaders/616_texmap_*.bin`, not distributed) instead of compiling GLSL; the compositor draws into an FBO that is read back and sent as an ordinary `Update`, acknowledged by `DisplayDidRefresh`, so the UI process keeps its bitmap path. Layers stay CPU-painted (accelerated buffers off) |
| `0900-ffmpeg-media-player` | `MediaPlayerPrivateFFmpeg` (`USE_FFMPEG`): software `<video>`/`<audio>` (progressive + HLS) on the SDK's ffmpeg; generated from `media/ffmpeg/`, edit there and run `media/sync-to-tree.sh` |

Editing patches: change files in `third_party/webkit/`, then run `scripts/ps5/export-webkit-patches.sh`, which rewrites `patches/*.patch` (keeping each patch's header text) from the tree. Every changed path must belong to one topic listed in that script.

## Build

```sh
scripts/ps5/build-deps.sh          # dependency prefix third_party/ps5-prefix (ICU, harfbuzz-icu, libwpe, ...)
scripts/ps5/fetch-webkit.sh        # third_party/webkit at the pinned commit + patches
scripts/ps5/build-webkit.sh all    # or: configure | wtf | jsc | webcore | webkit | package | smoke
```

`JOBS` (default 4) sets the ninja parallelism; the build directory is `build-wk-ps5/`. Release builds use `-O2 -DNDEBUG -g0`. `USE_FFMPEG` (default `ON`) builds the ffmpeg media engine.

## EGL / GLES entry points

The PlayStation port compiles TextureMapper/CoordinatedGraphics unconditionally, so libWebKit references EGL and GLES symbols. `stubs/gen-gl-stubs.py` generates a definition for every prototype in the SDK's `EGL/egl.h`, `EGL/eglext.h`, `GLES3/gl3.h`, `GLES2/gl2.h` and `GLES2/gl2ext.h`. Each forwards to Sony's GLES library once the shell has started it in that process (`ps5_glslim_proc()`, `src/ps5/glslim`, linked weakly; see patch 0022). Until then, and in processes that never start it (UI, NetworkProcess), EGL calls fail (`EGL_NO_DISPLAY`, `EGL_FALSE`, `NULL`, `eglGetError() == EGL_NOT_INITIALIZED`) and GLES calls do nothing.

## Link contract (for the shell)

The build produces, under `build-wk-ps5/`:

| File | Content |
|---|---|
| `lib/libWebKitPS5.a` | every object of bmalloc, WTF, JavaScriptCore (LLInt, baseline/DFG JIT and IPInt WebAssembly compiled in, on at run time unless `JSC_useJIT`/`JSC_useWasm` say otherwise: `docs/ps5-runtime.md`), PAL, WebCore and WebKit (about 1500 objects, about 490 MB) |
| `lib/libSkia.a` | in-tree Skia (CPU raster only) |
| `gl-stubs/libEGL.a`, `gl-stubs/libGLESv2.a` | failing EGL/GLES stubs |
| `WebKit/Headers/WebKit/` | public C API headers (`#include <WebKit/WKContext.h>`, `<WebKit/WKView.h>`, `<WebKit/WKPagePrivatePlayStation.h>`, ...) |
| `ps5webkit-link.cmake` | written by `build-webkit.sh package`: `PS5WEBKIT_INCLUDE_DIRS` and `PS5WEBKIT_LINK_LIBRARIES` (the link line below, absolute paths), consumed by `src/frontend/ps5/CMakeLists.txt` |

Compile flags for code using the C API: `-std=c++23` (or C), `-Ibuild-wk-ps5/WebKit/Headers -Iwebkit-port/include`, and `-Ithird_party/ps5-prefix/include/wpe-1.0` if you include `<wpe/wpe.h>`. The C API headers need no WebKit defines (`__SCE__` from the SDK compiler selects the PlayStation variants).

Link line (as used by `build-webkit.sh smoke`; lld does not care about archive order, but keep it for other linkers):

```sh
prospero-clang++ -Wl,--gc-sections -o browser.elf <shell objects> \
    build-wk-ps5/lib/libWebKitPS5.a build-wk-ps5/lib/libSkia.a \
    build-wk-ps5/gl-stubs/libEGL.a build-wk-ps5/gl-stubs/libGLESv2.a \
    third_party/ps5-prefix/lib/libwpe-1.0.a third_party/ps5-prefix/lib/libxkbcommon.a \
    third_party/ps5-prefix/lib/libharfbuzz-icu.a third_party/ps5-prefix/lib/libharfbuzz.a \
    third_party/ps5-prefix/lib/libfreetype.a \
    third_party/ps5-prefix/lib/libbrotlidec.a third_party/ps5-prefix/lib/libbrotlicommon.a \
    third_party/ps5-prefix/lib/libicui18n.a third_party/ps5-prefix/lib/libicuuc.a third_party/ps5-prefix/lib/libicudata.a \
    -lavformat -lbz2 -lavcodec -llzma -lx264 -lswscale -lswresample -lavutil \
    -lSDL2 -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lsamplerate \
    -lSceVideoOut -lSceKeyboard -lSceImeDialog \
    -lcurl -lpsl -lssl -lcrypto -lzstd -lxml2 -liconv -lsqlite3 -lfontconfig -lexpat \
    -lpng16 -ljpeg -lwebpdemux -lwebp -lsharpyuv -lz -lm -lpthread
```

The ffmpeg and SDL2 group is only needed with `USE_FFMPEG` (ffmpeg's static dependencies per `pkg-config --static`, SDL2 for the engine's audio sink); `build-webkit.sh` (`link_libs`) is the single definition of this list for both the smoke test and `ps5webkit-link.cmake`. (`-l` libraries come from the SDK's `target/user/homebrew/lib`, which the SDK driver already searches.) Without ffmpeg/SDL2 the result needs only the SDK's default `libkernel_web.sprx`, `libSceLibcInternal.sprx` and `libSceNet.sprx`; SDL2 adds `libSceSystemService`, `libSceUserService`, `libScePad`, `libSceAudioOut`, `libSceVideoOut`, `libSceKeyboard` and `libSceImeDialog.sprx` (all system modules). No `--wrap` options, no `libScePosixForWebKit`. Do not link the SDK's `libEGL.so`/`libGLESv2.so` (pkg-config for `wpe-1.0` adds `-lEGL`; do not use it). Strip the result (`prospero-strip`) before deploying: the dependency archives carry debug info.

Symbols the shell must define (C linkage):

| Symbol | Why |
|---|---|
| `ps5_spawn_self`, `ps5_terminate_process` | `include/ps5webkit/spawn.h`; called by the WebKit process launcher |
| `_wpe_loader_interface` | libwpe is static, so the embedder is its backend: `struct wpe_loader_interface { void* (*load_object)(const char*); ... }`. `load_object` may return `NULL` for everything (generic pasteboard, no gamepads), or return a `struct wpe_gamepad_provider_interface*` for `"_wpe_gamepad_provider_interface"` to feed `ENABLE_GAMEPAD` from scePad |
| optional `WKBundleInitialize` | statically linked injected bundle; enabled when the context configuration has a non-empty injected bundle path (patch 0009) |

`main()` must dispatch on `argv[0]`: `"WebProcess"` → `ps5_webkit_web_process_main()`, `"NetworkProcess"` → `ps5_webkit_network_process_main()` (see `include/ps5webkit/entry.h`), anything else runs the UI process.

Runtime files:
- **TLS roots**: a PEM bundle (curl's `cacert.pem`) at `/data/prosperoweb/cacert.pem`, or the path in `$PROSPEROWEB_CA_BUNDLE` in the NetworkProcess environment.
- **Fonts**: fontconfig is compiled with `/user/homebrew/etc/fonts/fonts.conf`; ship fonts plus a `fonts.conf` whose `<dir>` points at them, and set `FONTCONFIG_FILE` (and a writable `<cachedir>`) for the WebProcess. The console has no system fonts for fontconfig.
- **Language**: `$SYSTEM_LANGUAGE` (e.g. `en-US`) sets the preferred language list.
- Drawing is the non-accelerated path: keep accelerated compositing off (the PlayStation default in `WebPreferencesPlayStation.cpp`) and paint with `WKPagePaint(page, argb32Pixels, size, rect)` from `WKViewClient.setViewNeedsDisplay`.

`prosperoweb.elf` (the real shell, `-Wl,--gc-sections`, with ffmpeg): 162 MiB as linked, about 87 MB `.text`, 19 MB `.rodata`, 22 MB `.bss`; see `docs/ps5-runtime.md` for the stripped sizes.

Measured with the smoke test (`smoke/wk_smoke.cpp`, links `WKContextCreateWithConfiguration`, `WKViewCreate`, `WKPagePaint` and both entry points): `ELF 64-bit LSB pie executable, x86-64`, 142 MB with `--gc-sections` (136 MB with `-Wl,--strip-debug`; about 68 MB `.text`, 17 MB `.rodata`), `NEEDED` = `libkernel_web.sprx`, `libSceLibcInternal.sprx`, `libSceNet.sprx`.
