// SPDX-License-Identifier: GPL-3.0-or-later
//
// The --evo-host role: ProsperoWeb as the out-of-process web engine of EVO
// Player's Browser section (EVO issue #237, EVO's docs/ui/browser.md).
//
// No SDL window, no chrome.html, no shell controller: one web view, painted
// into a private ARGB32 cache and published to EVO through two shared RGBA
// buffers. EVO starts this process through the spawn broker with its end of
// an AF_UNIX SOCK_SEQPACKET pair at fd 3 and speaks evo_browser_wire.h on it:
//
//   host -> EVO   HELLO (+ the two buffer fds), FRAME, EVENT, BYE
//   EVO -> host   RELEASE, LOAD, NAV, POINTER, SCROLL, KEY, TEXT, RESIZE,
//                 VISIBLE, JS, BYE
//
// Lifetime: EVO's end of fd 3 is the only thing that keeps this process
// alive. When it closes (EVO's soft close, a kill or a crash) the host
// exits; its own broker connection (the one its WebKit opened to spawn
// WebProcess and NetworkProcess, which replaced EVO's) closes with it, and
// the broker kills the children and exits.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "evo_host_frames.h"
#include "prosperoweb/core/file_storage.h"
#include "prosperoweb/shell/engine_view.h"
#include "ps5_config.h"
#include "ps5_engine_view.h"

namespace prosperoweb::frontend {

class EvoHost final : public shell::EngineViewClient {
 public:
  // The channel EVO handed over (the broker puts it at fd 3).
  static constexpr int kChannelFd = 3;

  EvoHost() = default;
  ~EvoHost() override;

  // Runs until EVO closes the channel. Main thread only. 0 after an orderly
  // end, 2 when fd 3 is not EVO's channel, 3 when the buffers cannot be made.
  int run();

  // shell::EngineViewClient
  void url_changed(const std::string& url) override;
  void title_changed(const std::string& title) override;
  void load_started() override;
  void load_finished() override;
  void load_failed(const std::string& url, const std::string& error_text) override;
  void nav_state_changed(bool can_go_back, bool can_go_forward) override;
  void fullscreen_entered() override;
  void fullscreen_exited() override;
  void script_message(const std::string& json) override;
  void new_window_requested(const std::string& url) override;
  void text_input_focused(bool focused) override;
  void cursor_changed(shell::PageCursor cursor) override;

 private:
  bool check_channel();
  bool make_buffers();
  bool send_hello(bool with_fds);
  // Any thread: one packet, the fds as SCM_RIGHTS. Bounded by SO_SNDTIMEO.
  bool send_packet(const std::vector<std::uint8_t>& pkt, const int* fds = nullptr, int nfds = 0);
  void send_event(std::uint32_t kind, std::uint32_t arg, const std::string& text = {});

  void reader_main();
  void ticker_main();
  void request_frame();
  static void frame_trampoline(void* self);
  void frame();
  void handle(const std::vector<std::uint8_t>& pkt);
  void resize(int width, int height);
  void paint_and_publish();
  void begin_exit(const char* why);

  static void spawn_progress(const char* event, void* self);

  Ps5Config config_;
  std::unique_ptr<core::FileStorage> storage_;
  std::unique_ptr<Ps5EngineFactory> factory_;
  std::unique_ptr<shell::EngineView> view_holder_;
  Ps5EngineView* view_ = nullptr;

  // The shared buffers: made once at the start size (the most a RESIZE can
  // ask for), RGBA8 premultiplied, stride cap_w_ * 4.
  int cap_w_ = 1920;
  int cap_h_ = 1080;
  int shm_fd_[2] = {-1, -1};
  std::uint32_t* shm_[2] = {nullptr, nullptr};
  std::size_t shm_bytes_ = 0;

  // Page size and the ARGB32 cache WKPagePaint draws into (stride width_).
  int width_ = 1920;
  int height_ = 1080;
  std::vector<std::uint32_t> cache_;
  evohost::FramePair frames_;
  bool force_full_ = true;   // repaint the whole page on the next frame
  bool painted_once_ = false;  // WebKit has drawn something real
  bool visible_ = true;

  // Idle throttle (EVO #237): a page with endless animations repaints the
  // whole view on the CPU every rendering update. With no input, no load
  // and no fullscreen for kIdleAfterMs, the host publishes at most one frame
  // per kIdleFrameMs; the damage waits in WebKit's dirty region meanwhile.
  bool throttled(std::chrono::steady_clock::time_point now) const;
  void note_activity() { last_activity_ = std::chrono::steady_clock::now(); }
  std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_publish_{};
  bool loading_ = false;
  bool fullscreen_ = false;
  bool was_throttled_ = false;
  std::uint64_t throttled_skips_ = 0;

  // Packets from EVO, queued by the reader thread for the main thread.
  std::mutex in_mu_;
  std::deque<std::vector<std::uint8_t>> inbox_;
  std::mutex send_mu_;

  std::thread reader_;
  std::thread ticker_;
  std::atomic<bool> frame_queued_{false};
  std::atomic<bool> evo_gone_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> visible_tick_{true};
  bool running_ = false;
  std::uint64_t frames_sent_ = 0;
  std::uint64_t packets_in_ = 0;

  // Damage report (EVO #237: a page that repaints while idle). Every
  // kDamageLogMs the host logs WebKit's setViewNeedsDisplay count and area,
  // the packets EVO sent, and the rects that came back most often.
  void note_damage_rects(const display::Region& dirty);
  std::map<std::array<int, 4>, std::uint32_t> damage_seen_;
  std::uint64_t damage_frames_ = 0;
  std::uint64_t damage_packets_at_ = 0;
  std::chrono::steady_clock::time_point damage_since_ = std::chrono::steady_clock::now();
};

}  // namespace prosperoweb::frontend
