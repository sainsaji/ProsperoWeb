add_executable(prosperoweb_ps5_tests
  main.cpp
  test_spawn_util.cpp
  test_spawn_broker.cpp
  test_audio_server.cpp
  test_media_control.cpp
  test_display.cpp
  test_video_overlay.cpp
  test_hwdec.cpp
  test_hwdec_probe.cpp
  test_cpu_mask.cpp
  test_cpuset_assign.cpp
  test_evo_host.cpp
)
target_link_libraries(prosperoweb_ps5_tests PRIVATE prosperoweb::spawn_util
  prosperoweb::spawn_broker prosperoweb::audio_server prosperoweb::display prosperoweb::video_server prosperoweb::cpu
  prosperoweb::evohost prosperoweb_warnings)
target_include_directories(prosperoweb_ps5_tests SYSTEM PRIVATE "${PROSPEROWEB_THIRD_PARTY_DIR}")

add_test(NAME ps5.spawn_util
         COMMAND prosperoweb_ps5_tests --test-suite=spawn_self_path,spawn_fd_table --no-version)
add_test(NAME ps5.spawn_broker
         COMMAND prosperoweb_ps5_tests
                 --test-suite=broker_proto,broker_endpoints,broker_fd_passing,broker_e2e,broker_tcp,broker_websrv,broker_elfldr,broker_loader_order,spawn_poll,broker_launch_policy,etahen_ipc,spawn_tcp_probe,broker_async_launch,title_paths --no-version)
add_test(NAME ps5.audio_server
         COMMAND prosperoweb_ps5_tests
                 --test-suite=audio_wire,audio_clock,audio_mixer,audio_ring,audio_e2e --no-version)
add_test(NAME ps5.media_control
         COMMAND prosperoweb_ps5_tests
                 --test-suite=media_control,media_audio_control,media_video_control --no-version)
add_test(NAME ps5.display
         COMMAND prosperoweb_ps5_tests
                 --test-suite=display_region,display_pixels,display_stats,display_splash,display_pointer --no-version)
add_test(NAME ps5.video_overlay
         COMMAND prosperoweb_ps5_tests
                 --test-suite=video_wire,video_geometry,video_scale,video_compose,video_e2e --no-version)
add_test(NAME ps5.hwdec
         COMMAND prosperoweb_ps5_tests
                 --test-suite=hwdec_bitstream,hwdec_policy,hwdec_convert,hwdec_wire,hwdec_e2e,hwdec_geometry,hwdec_visibility,hwdec_probe
                 --no-version)
add_test(NAME ps5.cpu_mask
         COMMAND prosperoweb_ps5_tests --test-suite=cpu_mask --no-version)
add_test(NAME ps5.cpuset_assign
         COMMAND prosperoweb_ps5_tests --test-suite=cpuset_assign --no-version)
add_test(NAME ps5.evo_host
         COMMAND prosperoweb_ps5_tests --test-suite=evo_wire,evo_frames --no-version)
