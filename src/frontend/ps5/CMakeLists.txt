# PS5 frontend (prosperoweb executable). Activated by -DPROSPEROWEB_FRONTEND=ps5.
#
# The WebKit static library is built separately by scripts/ps5/build-webkit.sh,
# whose "package" stage writes ${PS5_WEBKIT_BUILD_DIR}/ps5webkit-link.cmake:
# the C API include directories and the full link line (webkit-port/README.md,
# "Link contract"). When that file exists, the prosperoweb executable links
# against it. Before it exists the frontend can still be compile-checked
# (target prosperoweb_frontend_ps5_objs) against a generated forwarding-header
# directory that symlinks WebKit/<name>.h to the public C API headers in
# third_party/webkit.

if(NOT PS5)
  message(FATAL_ERROR "The ps5 frontend requires the PS5 toolchain "
                      "(-DCMAKE_TOOLCHAIN_FILE=.../prospero.cmake)")
endif()

set(PS5_WEBKIT_BUILD_DIR "${CMAKE_SOURCE_DIR}/build-wk-ps5" CACHE PATH
    "WebKit build directory (scripts/ps5/build-webkit.sh)")
set(PS5_WEBKIT_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ps5-prefix" CACHE PATH
    "Install prefix of the cross-built WebKit dependencies")

# Overrides the exported link line when non-empty.
set(PS5_WEBKIT_LINK_LIBS "" CACHE STRING
    "Libraries to link into prosperoweb instead of ps5webkit-link.cmake's list")

get_filename_component(_wk_build_dir "${PS5_WEBKIT_BUILD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
set(_wk_link_file "${_wk_build_dir}/ps5webkit-link.cmake")
if(EXISTS "${_wk_link_file}")
  include("${_wk_link_file}")
  # Re-run CMake when build-webkit.sh rewrites the link file.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wk_link_file}")
  message(STATUS "ps5 frontend: WebKit link line from ${_wk_link_file}")
else()
  set(PS5WEBKIT_INCLUDE_DIRS "")
  set(PS5WEBKIT_LINK_LIBRARIES "")
  message(STATUS "ps5 frontend: ${_wk_link_file} not found, prosperoweb cannot link "
                 "(run scripts/ps5/build-webkit.sh package)")
endif()
if(PS5_WEBKIT_LINK_LIBS)
  set(PS5WEBKIT_LINK_LIBRARIES ${PS5_WEBKIT_LINK_LIBS})
endif()

# --- Generate the WebKit C API forwarding headers --------------------------
set(_wk_src "${CMAKE_SOURCE_DIR}/third_party/webkit/Source/WebKit")
set(_wk_fwd "${CMAKE_BINARY_DIR}/ps5-webkit-forwarding")
if(EXISTS "${_wk_src}")
  file(MAKE_DIRECTORY "${_wk_fwd}/WebKit")
  file(GLOB _wk_headers
    "${_wk_src}/Shared/API/c/*.h"
    "${_wk_src}/Shared/API/c/playstation/*.h"
    "${_wk_src}/UIProcess/API/C/*.h"
    "${_wk_src}/UIProcess/API/C/playstation/*.h")
  foreach(_h ${_wk_headers})
    get_filename_component(_n "${_h}" NAME)
    # Generic dirs are globbed first, so on a basename clash the generic header
    # wins (playstation headers have distinct names in practice).
    if(NOT EXISTS "${_wk_fwd}/WebKit/${_n}")
      file(CREATE_LINK "${_h}" "${_wk_fwd}/WebKit/${_n}" SYMBOLIC)
    endif()
  endforeach()
endif()


find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(_frontend_sources
  main.cpp
  evo_host.cpp
  ps5_addons.cpp
  ps5_app.cpp
  ps5_audio_server.cpp
  ps5_chrome_ui.cpp
  ps5_config.cpp
  ps5_diagnostics.cpp
  ps5_engine_view.cpp
  ps5_cpu_affinity.cpp
  ps5_hwdec.cpp
  ps5_cpu_monitor.cpp
  ps5_ime.cpp
  ps5_presenter.cpp
  ps5_log.cpp
  ps5_runtime_env.cpp
  ps5_splash_text.cpp
  ps5_touchpad.cpp
  ps5_video_overlay.cpp
  wk_web_view.cpp
  wpe_loader.cpp
)

# OBJECT library so the frontend can be compile-checked without linking WebKit.
add_library(prosperoweb_frontend_ps5_objs OBJECT ${_frontend_sources})
target_compile_features(prosperoweb_frontend_ps5_objs PRIVATE cxx_std_20)
target_link_libraries(prosperoweb_frontend_ps5_objs PRIVATE prosperoweb_warnings)
target_include_directories(prosperoweb_frontend_ps5_objs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/webkit-port/include")
# WebKit, SDL2 and nlohmann headers are third-party: include as SYSTEM so their
# warnings do not trip -Werror on our code.
# The WebKit build's own header copy (PS5WEBKIT_INCLUDE_DIRS) wins over the
# forwarding headers, which only serve compile checks before it exists.
target_include_directories(prosperoweb_frontend_ps5_objs SYSTEM PRIVATE
  ${PS5WEBKIT_INCLUDE_DIRS}
  "${_wk_fwd}"
  "${PS5_WEBKIT_PREFIX}/include/wpe-1.0"
  "${PS5_WEBKIT_PREFIX}/include/freetype2"  # ps5_splash_text.cpp; libfreetype.a is on WebKit's link line
  "${PS5_PAYLOAD_SDK}/target/user/homebrew/include"  # ps5_addons.cpp uses libcurl
  "${SDL2_INCLUDE_DIRS}"
  "${PROSPEROWEB_THIRD_PARTY_DIR}")
# prosperoweb::core / ::shell provide the public headers the frontend consumes.
target_link_libraries(prosperoweb_frontend_ps5_objs PRIVATE prosperoweb::core prosperoweb::shell
  prosperoweb::spawn_util prosperoweb::spawn_broker prosperoweb::audio_server  # prosperoweb/spawn/spawn.h (the broker role entry)
  prosperoweb::display prosperoweb::video_server prosperoweb::cpu prosperoweb::evohost)

# The final executable. It cannot link until the WebKit archives exist, so it
# is excluded from the default build: cmake --build <dir> --target prosperoweb.
add_executable(prosperoweb EXCLUDE_FROM_ALL
  $<TARGET_OBJECTS:prosperoweb_frontend_ps5_objs>)
target_link_libraries(prosperoweb PRIVATE
  prosperoweb::shell
  prosperoweb::core
  prosperoweb::spawn
  prosperoweb::audio_server
  prosperoweb::display
  prosperoweb::video_server
  prosperoweb::cpu
  prosperoweb::evohost
  ${PS5WEBKIT_LINK_LIBRARIES}
  ${SDL2_LIBRARIES}
  # ps5_ime.cpp, ps5_touchpad.cpp (also on SDL2's own link line)
  SceImeDialog SceUserService ScePad)
target_link_directories(prosperoweb PRIVATE ${SDL2_LIBRARY_DIRS})
# libWebKitPS5.a holds every WebKit object; drop what the browser never reaches.
target_link_options(prosperoweb PRIVATE -Wl,--gc-sections)
# Every thread passes through ps5_cpu_affinity.cpp (mask at start, registry
# for the CPU monitor).
target_link_options(prosperoweb PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=pthread_exit)
set_target_properties(prosperoweb PROPERTIES OUTPUT_NAME "prosperoweb" SUFFIX ".elf")

# The spawn broker alone (broker_main.cpp): what the native title asks
# elfldr or websrv to start, instead of the whole multi-call ELF.
add_executable(prosperoweb_broker EXCLUDE_FROM_ALL broker_main.cpp ps5_log.cpp)
target_compile_features(prosperoweb_broker PRIVATE cxx_std_20)
target_include_directories(prosperoweb_broker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prosperoweb_broker PRIVATE prosperoweb_warnings prosperoweb::spawn
  prosperoweb::cpu)
target_link_options(prosperoweb_broker PRIVATE -Wl,--gc-sections)
set_target_properties(prosperoweb_broker PROPERTIES OUTPUT_NAME "prosperoweb-broker" SUFFIX ".elf")

# --- The native title's eboot.bin (docs/native-title.md) --------------------
#
# The same frontend, linked a second time as a PS5 native application: the
# boilerplate's app_crt (entry _start(process_parameters, teardown)) instead
# of the payload crt1.o, the native PIE layout, and the native runtime
# shims (native/). scripts/ps5/native/make-title.sh converts and signs the
# result into PRWB00001/eboot.bin with third_party/ps5-native-tool. The
# payload ELF (prosperoweb above) stays what the broker and the children run.
option(PROSPEROWEB_NATIVE_TITLE_TARGET "Define the native title eboot target (prosperoweb_native)" ON)
if(PROSPEROWEB_NATIVE_TITLE_TARGET)
  set(_native_tool "${CMAKE_SOURCE_DIR}/third_party/ps5-native-tool/tooling/native")
  file(READ "${CMAKE_SOURCE_DIR}/assets/app.json" _app_json)
  string(JSON PROSPEROWEB_TITLE_ID GET "${_app_json}" titleId)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/app.json")

  # The frontend objects again, with the native code paths.
  add_library(prosperoweb_frontend_ps5_native_objs OBJECT ${_frontend_sources})
  target_compile_features(prosperoweb_frontend_ps5_native_objs PRIVATE cxx_std_20)
  target_compile_definitions(prosperoweb_frontend_ps5_native_objs PRIVATE
    PROSPEROWEB_NATIVE_TITLE=1 PROSPEROWEB_TITLE_ID="${PROSPEROWEB_TITLE_ID}")
  target_link_libraries(prosperoweb_frontend_ps5_native_objs PRIVATE prosperoweb_warnings)
  target_include_directories(prosperoweb_frontend_ps5_native_objs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/webkit-port/include")
  target_include_directories(prosperoweb_frontend_ps5_native_objs SYSTEM PRIVATE
    ${PS5WEBKIT_INCLUDE_DIRS}
    "${_wk_fwd}"
    "${PS5_WEBKIT_PREFIX}/include/wpe-1.0"
    "${PS5_WEBKIT_PREFIX}/include/freetype2"
    "${PS5_PAYLOAD_SDK}/target/user/homebrew/include"  # ps5_addons.cpp uses libcurl
    "${SDL2_INCLUDE_DIRS}"
    "${PROSPEROWEB_THIRD_PARTY_DIR}")
  target_link_libraries(prosperoweb_frontend_ps5_native_objs PRIVATE prosperoweb::core
    prosperoweb::shell prosperoweb::spawn_util prosperoweb::spawn_broker
    prosperoweb::audio_server prosperoweb::display prosperoweb::video_server prosperoweb::cpu
    prosperoweb::evohost)

  # What the payload crt and libc.a gave the payload (native/*.c).
  add_library(prosperoweb_native_runtime OBJECT
    native/native_crt.cpp
    native/native_runtime.c
    native/native_libc.c
    native/native_trace.c
    native/native_fork_probe.c
    native/native_memory.c
    native/native_title.c)
  # native_fork_probe.c links rfork_thread as a real libkernel NID import and
  # calls it only under this define; without it the file still builds (levels
  # 1/2, resolved by name) but the rfork levels 3/4 are compiled out.
  target_compile_definitions(prosperoweb_native_runtime PRIVATE PROSPEROWEB_NATIVE_TITLE=1)
  target_link_libraries(prosperoweb_native_runtime PRIVATE prosperoweb_warnings)

  # Third-party code: dlmalloc, without -Werror.
  add_library(prosperoweb_native_vendor OBJECT native/dlmalloc_unit.c)
  target_include_directories(prosperoweb_native_vendor PRIVATE "${CMAKE_SOURCE_DIR}/third_party/dlmalloc")
  target_compile_options(prosperoweb_native_vendor PRIVATE -w)

  # Link stub for libSceVideodec2 (the payload SDK has none).
  add_library(prosperoweb_videodec2_stub SHARED native/videodec2_stub.c)
  target_link_libraries(prosperoweb_videodec2_stub PRIVATE prosperoweb_warnings)
  set_target_properties(prosperoweb_videodec2_stub PROPERTIES
    OUTPUT_NAME SceVideodec2 NO_SONAME ON)
  target_link_options(prosperoweb_videodec2_stub PRIVATE
    -nodefaultlibs -Wl,-soname,libSceVideodec2.sprx)

  # libc.a without its raw-syscall objects: a title may not execute the
  # syscall instruction itself. Those calls then resolve to libkernel, or
  # to native/native_runtime.c.
  set(_libc_native "${CMAKE_CURRENT_BINARY_DIR}/libc-native.a")
  add_custom_command(OUTPUT "${_libc_native}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PS5_PAYLOAD_SDK}/target/lib/libc.a" "${_libc_native}"
    COMMAND "${CMAKE_AR}" d "${_libc_native}" syscalls.o mman.o
    DEPENDS "${PS5_PAYLOAD_SDK}/target/lib/libc.a"
    COMMENT "libc-native.a: the SDK libc.a without syscalls.o and mman.o"
    VERBATIM)
  add_custom_target(prosperoweb_libc_native DEPENDS "${_libc_native}")

  add_executable(prosperoweb_native EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:prosperoweb_frontend_ps5_native_objs>
    $<TARGET_OBJECTS:prosperoweb_native_runtime>
    $<TARGET_OBJECTS:prosperoweb_native_vendor>)
  add_dependencies(prosperoweb_native prosperoweb_libc_native prosperoweb_videodec2_stub)
  target_link_libraries(prosperoweb_native PRIVATE
    prosperoweb::shell
    prosperoweb::core
    prosperoweb::spawn
    prosperoweb::audio_server
    prosperoweb::display
    prosperoweb::video_server
    prosperoweb::cpu
    prosperoweb::evohost
    ${PS5WEBKIT_LINK_LIBRARIES}
    ${SDL2_LIBRARIES}
    SceImeDialog SceUserService ScePad
    "$<TARGET_FILE:prosperoweb_videodec2_stub>"
    # -nodefaultlibs drops the toolchain's own runtime: name it explicitly,
    # with libc-native.a in place of libc.a.
    "${PS5_PAYLOAD_SDK}/target/lib/libc++.a"
    "${PS5_PAYLOAD_SDK}/target/lib/libc++abi.a"
    "${PS5_PAYLOAD_SDK}/target/lib/libunwind.a"
    "${_libc_native}"
    # libkernel, as the working native titles bind it (syobon-action-ps5 and
    # the Stremio port link the SDK's stubs in name order, libkernel.so
    # first). Imports bound to libkernel_web stayed at 0 in the title on the
    # console (run 15: clock_gettime, sceKernelDebugOutText).
    kernel SceLibcInternal SceNet SceSysmodule)
  target_link_directories(prosperoweb_native PRIVATE ${SDL2_LIBRARY_DIRS})
  set(_native_wraps pthread_create pthread_exit
    malloc free calloc realloc posix_memalign aligned_alloc malloc_usable_size
    mmap munmap pipe
    sceImeDialogGetStatus sceImeDialogInit
    # native/native_trace.c: access() past EPERM, the IPC calls logged.
    access socketpair sendmsg recvmsg select
    # fcntl on sockets and dup() are refused in a title (native_trace.c).
    fcntl dup
    # poll() falls back to select() if it misses data in the title.
    poll)
  list(TRANSFORM _native_wraps PREPEND "-Wl,--wrap=")
  target_link_options(prosperoweb_native PRIVATE
    -nostartfiles -nodefaultlibs
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/native/ps5-native-app.ld"
    "-Wl,--version-script,${_native_tool}/app-symbols.map"
    -Wl,-e,_start
    # Weak references nobody defines (WKBundleInitialize, zstd's trace
    # hooks, ...) stay 0 instead of becoming imports the converter refuses.
    -Wl,--no-dynamic-linker
    # Only the system modules something still calls are needed at start-up
    # (libSceKeyboard is replaced by native/native_title.c).
    -Wl,--as-needed
    -Wl,--gc-sections
    ${_native_wraps})
  set_property(TARGET prosperoweb_native APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/native/ps5-native-app.ld" "${_libc_native}")
  set_target_properties(prosperoweb_native PROPERTIES OUTPUT_NAME "prosperoweb-native" SUFFIX ".elf")
endif()
