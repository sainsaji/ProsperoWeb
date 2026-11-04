# The portable half of the --evo-host mode: EVO Player's wire messages and the
# shared frame-buffer bookkeeping. No WebKit, no sockets, so the host tests
# cover it; the PS5 frontend links it.
add_library(prosperoweb_evohost STATIC
  evo_host_frames.cpp
)
add_library(prosperoweb::evohost ALIAS prosperoweb_evohost)
target_include_directories(prosperoweb_evohost PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(prosperoweb_evohost PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_evohost PUBLIC prosperoweb::display PRIVATE prosperoweb_warnings)
