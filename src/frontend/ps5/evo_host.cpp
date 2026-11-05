// SPDX-License-Identifier: GPL-3.0-or-later
#include "evo_host.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <WebKit/WKRunLoop.h>

#include "prosperoweb/core/page_scripts.h"
#include "prosperoweb/spawn/spawn.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"

namespace prosperoweb::frontend {

namespace {

constexpr const char* kBaseDir = kPs5DataDir;
// The frame loop's tick: page script messages (WkWebView::poll_all) and
// buffers EVO released need a frame even when WebKit reports no damage.
constexpr int kTickVisibleMs = 16;
constexpr int kTickHiddenMs = 200;
// EVO is gone: leave after this long whatever WebKit's teardown is doing.
// Short, because EVO waits (1.5 s) at its soft close for the broker to go,
// and the broker goes only once this process has.
constexpr int kExitGraceMs = 200;
// How often the damage report is logged, and how many rects it names.
constexpr int kDamageLogMs = 5000;
constexpr std::size_t kDamageTopRects = 4;
// Idle throttle: after this long with no input, load or fullscreen, publish
// at most one frame per kIdleFrameMs (10/s).
constexpr int kIdleAfterMs = 2000;
constexpr int kIdleFrameMs = 100;
// Idle: pause every running animation of the page (CSS transitions aside).
// Size is no guide: without compositing, a small icon bobbing over blurred
// background art makes WebKit redo the blur, a full core on
// prosperostore.net. A spinner freezes too, until the next input. Resumed on
// any activity.
constexpr const char* kPauseIdleAnimations =
    "(function(){try{if(!document.getAnimations)return;"
    "var p=window.__evoPaused||(window.__evoPaused=[]);"
    "document.getAnimations().forEach(function(a){"
    "if(a.playState!=='running'||(window.CSSTransition&&a instanceof CSSTransition))return;"
    "a.pause();p.push(a);});}catch(x){}})();";
constexpr const char* kResumeIdleAnimations =
    "(function(){var p=window.__evoPaused;if(!p||!p.length)return;window.__evoPaused=[];"
    "p.forEach(function(a){try{a.play();}catch(x){}});})();";
// The largest view (and so the shared buffers), and the largest page scale.
constexpr int kMaxW = 3840;
constexpr int kMaxH = 2160;
constexpr int kMaxScale = 2;

}  // namespace

EvoHost::~EvoHost() {
  stopping_.store(true, std::memory_order_release);
  if (ticker_.joinable()) {
    ticker_.join();
  }
  if (reader_.joinable()) {
    // Blocked in recvmsg until EVO closes its end; shutting ours down wakes it.
    shutdown(kChannelFd, SHUT_RDWR);
    reader_.join();
  }
  ps5_spawn_set_progress(nullptr, nullptr);
  WkWebView::set_damage_hook(nullptr, nullptr);
  for (int i = 0; i < 2; i++) {
    if (shm_[i]) {
      munmap(shm_[i], shm_bytes_);
    }
    if (shm_fd_[i] >= 0) {
      close(shm_fd_[i]);
    }
  }
}

bool EvoHost::check_channel() {
  struct stat st {};
  if (fstat(kChannelFd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
    log_printf("evo-host: fd %d is not a socket (%s): start me from EVO Player", kChannelFd,
               std::strerror(errno));
    return false;
  }
  int type = 0;
  socklen_t len = sizeof type;
  if (getsockopt(kChannelFd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET) {
    log_printf("evo-host: fd %d is a socket of type %d, not SOCK_SEQPACKET", kChannelFd, type);
    return false;
  }
  // Every send is bounded: a stalled EVO must not freeze WebKit's main loop.
  struct timeval tv {1, 0};
  setsockopt(kChannelFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return true;
}

bool EvoHost::make_buffers() {
  shm_bytes_ = static_cast<std::size_t>(cap_w_) * cap_h_ * 4;
  for (int i = 0; i < 2; i++) {
    shm_fd_[i] = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (shm_fd_[i] < 0 || ftruncate(shm_fd_[i], static_cast<off_t>(shm_bytes_)) != 0) {
      log_printf("evo-host: shared buffer %d: %s", i, std::strerror(errno));
      return false;
    }
    void* p = mmap(nullptr, shm_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_[i], 0);
    if (p == MAP_FAILED) {
      log_printf("evo-host: mapping shared buffer %d (%zu bytes): %s", i, shm_bytes_,
                 std::strerror(errno));
      return false;
    }
    shm_[i] = static_cast<std::uint32_t*>(p);
  }
  log_printf("evo-host: two shared buffers of %dx%d RGBA (%zu MiB each)", cap_w_, cap_h_,
             shm_bytes_ >> 20);
  return true;
}

bool EvoHost::send_packet(const std::vector<std::uint8_t>& pkt, const int* fds, int nfds) {
  if (evo_gone_.load(std::memory_order_acquire)) {
    return false;
  }
  struct iovec iov {const_cast<std::uint8_t*>(pkt.data()), pkt.size()};
  union {
    struct cmsghdr h;
    char buf[CMSG_SPACE(sizeof(int) * EVOB_MAX_BUFS)];
  } ctl;
  std::memset(&ctl, 0, sizeof ctl);
  struct msghdr m {};
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  if (fds && nfds > 0 && nfds <= EVOB_MAX_BUFS) {
    m.msg_control = ctl.buf;
    m.msg_controllen = CMSG_SPACE(sizeof(int) * static_cast<std::size_t>(nfds));
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<std::size_t>(nfds));
    std::memcpy(CMSG_DATA(c), fds, sizeof(int) * static_cast<std::size_t>(nfds));
  }
  ssize_t n;
  {
    std::lock_guard<std::mutex> lock(send_mu_);
    n = sendmsg(kChannelFd, &m, 0);
  }
  if (n == static_cast<ssize_t>(pkt.size())) {
    return true;
  }
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    log_printf("evo-host: EVO did not take a packet within 1 s (type %u dropped)",
               pkt.size() >= sizeof(evob_hdr) ? reinterpret_cast<const evob_hdr*>(pkt.data())->type
                                              : 0u);
    return false;
  }
  log_printf("evo-host: send to EVO failed: %s", n < 0 ? std::strerror(errno) : "short write");
  begin_exit("send failed");
  return false;
}

bool EvoHost::send_hello(bool with_fds) {
  const evob_hello h{static_cast<std::uint32_t>(width_), static_cast<std::uint32_t>(height_),
                     static_cast<std::uint32_t>(cap_w_ * 4), EVOB_RGBA8_PREMUL,
                     with_fds ? 2u : 0u};
  return send_packet(evohost::pack(EVOB_HELLO, &h, sizeof h), with_fds ? shm_fd_ : nullptr,
                     with_fds ? 2 : 0);
}

void EvoHost::send_event(std::uint32_t kind, std::uint32_t arg, const std::string& text) {
  const evob_event ev{kind, arg};
  std::size_t cut = 0;
  const auto pkt = evohost::pack(EVOB_EVENT, &ev, sizeof ev, text, &cut);
  if (cut) {
    log_printf("evo-host: event %u cut by %zu of %zu bytes to fit one packet", kind, cut,
               text.size());
  }
  send_packet(pkt);
}

int EvoHost::run() {
  log_line("evo-host: starting (EVO Player browser engine)");
  if (!check_channel()) {
    return 2;
  }

  {
    struct rlimit rl {};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
      rl.rlim_cur = rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
    }
  }

  storage_ = std::make_unique<core::FileStorage>(kBaseDir);
  config_ = Ps5Config::load(*storage_);
  width_ = std::clamp(config_.width, 320, kMaxW);
  height_ = std::clamp(config_.height, 180, kMaxH);
  // The buffers are made once, so they take the most a RESIZE can ask for:
  // EVO's 2x page on its 3840x2160 output.
  cap_w_ = kMaxW;
  cap_h_ = kMaxH;
  if (!make_buffers()) {
    return 3;
  }
  cache_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
  frames_.reset(width_, height_);
  if (!send_hello(true)) {
    log_line("evo-host: HELLO not delivered, EVO is gone");
    return 0;
  }
  log_printf("evo-host: HELLO sent (%dx%d)", width_, height_);

  apply_language_env(config_.language);
  WKRunLoopInitializeMain();
  ps5_spawn_set_progress(&EvoHost::spawn_progress, this);

  // No gamepad bridge and no input trace: EVO owns the pad and sends only
  // pointer, wheel and key events.
  const std::vector<std::string> user_scripts = {core::kEmeDetectorScript,
                                                 core::kFocusReporterScript};
  factory_ = std::make_unique<Ps5EngineFactory>(kBaseDir, user_scripts, config_.user_agent);
  view_holder_ = factory_->create(*this);
  view_ = static_cast<Ps5EngineView*>(view_holder_.get());
  view_->set_size(width_, height_);
  view_->set_visible(true);
  log_line("evo-host: web view ready, waiting for EVO's LOAD");

  running_ = true;
  WkWebView::set_damage_hook([](void* self) { static_cast<EvoHost*>(self)->request_frame(); },
                             this);
  reader_ = std::thread(&EvoHost::reader_main, this);
  ticker_ = std::thread(&EvoHost::ticker_main, this);
  request_frame();
  WKRunLoopRunMain();

  log_printf("evo-host: stopped (%llu frames sent, %llu packets from EVO)",
             static_cast<unsigned long long>(frames_sent_),
             static_cast<unsigned long long>(packets_in_));
  return 0;
}

void EvoHost::reader_main() {
  std::vector<std::uint8_t> buf(EVOB_MAX_PACKET);
  const char* why = "EVO closed the channel";
  for (;;) {
    const ssize_t n = recv(kChannelFd, buf.data(), buf.size(), 0);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        if (stopping_.load(std::memory_order_acquire)) {
          why = "host stopping";
          break;
        }
        continue;
      }
      why = std::strerror(errno);
      break;
    }
    evohost::Packet p;
    if (evohost::parse(buf.data(), static_cast<std::size_t>(n), &p) && p.type == EVOB_BYE) {
      why = "EVO said BYE";
      break;
    }
    {
      std::lock_guard<std::mutex> lock(in_mu_);
      inbox_.emplace_back(buf.begin(), buf.begin() + n);
    }
    request_frame();
  }
  log_printf("evo-host: channel ended: %s", why);
  begin_exit(why);
}

void EvoHost::begin_exit(const char* why) {
  if (evo_gone_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  log_printf("evo-host: exiting (%s)", why);
  // WebKit's teardown can block; the broker cleans up the children either
  // way once this process is gone.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(kExitGraceMs));
    log_line("evo-host: leaving without WebKit's teardown (the broker kills the children)");
    _exit(0);
  }).detach();
  request_frame();
}

void EvoHost::ticker_main() {
  while (!stopping_.load(std::memory_order_acquire) &&
         !evo_gone_.load(std::memory_order_acquire)) {
    const int ms = visible_tick_.load(std::memory_order_relaxed) ? kTickVisibleMs : kTickHiddenMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    request_frame();
  }
}

void EvoHost::request_frame() {
  if (!frame_queued_.exchange(true, std::memory_order_acq_rel)) {
    WKRunLoopCallOnMainThread(&EvoHost::frame_trampoline, this);
  }
}

void EvoHost::frame_trampoline(void* self) { static_cast<EvoHost*>(self)->frame(); }

void EvoHost::frame() {
  frame_queued_.store(false, std::memory_order_release);
  if (!running_) {
    return;
  }
  if (evo_gone_.load(std::memory_order_acquire)) {
    running_ = false;
    stopping_.store(true, std::memory_order_release);
    WKRunLoopStopMain();
    return;
  }
  std::deque<std::vector<std::uint8_t>> in;
  {
    std::lock_guard<std::mutex> lock(in_mu_);
    in.swap(inbox_);
  }
  for (const auto& pkt : in) {
    handle(pkt);
  }
  WkWebView::poll_all();
  if (visible_) {
    paint_and_publish();
  }
}

void EvoHost::handle(const std::vector<std::uint8_t>& pkt) {
  packets_in_++;
  evohost::Packet p;
  if (!evohost::parse(pkt.data(), pkt.size(), &p)) {
    log_printf("evo-host: ignored a malformed packet of %zu bytes", pkt.size());
    return;
  }
  if (p.type != EVOB_RELEASE) {
    note_activity();  // input, navigation, resize: full frame rate again
  }
  switch (p.type) {
    case EVOB_RELEASE: {
      evob_release r{};
      if (p.body(&r)) {
        frames_.released(r.buf);
      }
      break;
    }
    case EVOB_LOAD: {
      const std::string url(p.text());
      log_printf("evo-host: load %s", url.c_str());
      view_->load(url);
      break;
    }
    case EVOB_NAV: {
      evob_nav n{};
      if (!p.body(&n)) break;
      switch (n.op) {
        case EVOB_NAV_BACK: view_->go_back(); break;
        case EVOB_NAV_FORWARD: view_->go_forward(); break;
        case EVOB_NAV_RELOAD: view_->reload(); break;
        case EVOB_NAV_STOP: view_->stop(); break;
        default: log_printf("evo-host: unknown NAV op %u", n.op); break;
      }
      break;
    }
    case EVOB_POINTER: {
      evob_pointer e{};
      if (!p.body(&e)) break;
      // EVO speaks UI pixels; the view is scale_ times larger.
      const int x = e.x * scale_, y = e.y * scale_;
      if (e.action == EVOB_PTR_MOVE) {
        view_->pointer_move(x, y);
      } else if (e.button >= 1 && e.button <= 3) {
        view_->pointer_button(x, y, static_cast<shell::MouseButton>(e.button),
                              e.action == EVOB_PTR_DOWN);
      }
      break;
    }
    case EVOB_SCROLL: {
      evob_scroll s{};
      if (p.body(&s)) {
        view_->scroll(s.x * scale_, s.y * scale_, s.dx * scale_, s.dy * scale_);
      }
      break;
    }
    case EVOB_KEY: {
      evob_key k{};
      if (p.body(&k)) {
        view_->key(k.key_code, k.modifiers, k.pressed != 0, std::string(p.text(sizeof k)));
      }
      break;
    }
    case EVOB_TEXT:
      view_->insert_text(std::string(p.text()));
      break;
    case EVOB_RESIZE: {
      // An older EVO sends width and height only: scale 1.
      evob_size s{};
      if (p.len >= EVOB_SIZE_V1_BYTES) {
        std::memcpy(&s, p.payload, std::min<std::size_t>(p.len, sizeof s));
        resize(static_cast<int>(s.width), static_cast<int>(s.height),
               static_cast<int>(s.scale));
      }
      break;
    }
    case EVOB_VISIBLE: {
      evob_flag f{};
      if (!p.body(&f)) break;
      const bool v = f.value != 0;
      if (v != visible_) {
        visible_ = v;
        visible_tick_.store(v, std::memory_order_relaxed);
        view_->set_visible(v);
        if (v) {
          force_full_ = true;  // whatever WebKit did while hidden
        }
        log_printf("evo-host: %s", v ? "visible, painting again" : "hidden, painting paused");
      }
      break;
    }
    case EVOB_JS:
      view_->run_javascript(std::string(p.text()));
      break;
    default:
      log_printf("evo-host: ignored packet type %u", p.type);
      break;
  }
}

void EvoHost::resize(int width, int height, int scale) {
  scale = std::clamp(scale, 1, kMaxScale);
  // The view must fit the buffers: drop to scale 1 for a page too big for 2x.
  if (width * scale > cap_w_ || height * scale > cap_h_) {
    scale = 1;
  }
  width = std::clamp(width * scale, 1, cap_w_);
  height = std::clamp(height * scale, 1, cap_h_);
  if (width == width_ && height == height_ && scale == scale_) {
    send_hello(false);  // EVO asked; answer with the same size
    return;
  }
  width_ = width;
  height_ = height;
  cache_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
  frames_.reset(width_, height_);
  force_full_ = true;
  view_->set_size(width_, height_);
  if (scale != scale_) {
    scale_ = scale;
    view_->web().set_page_zoom(static_cast<double>(scale_));
  }
  log_printf("evo-host: page size %dx%d (scale %d)", width_, height_, scale_);
  send_hello(false);
}

bool EvoHost::throttled(std::chrono::steady_clock::time_point now) const {
  if (loading_ || fullscreen_ || force_full_) {
    return false;
  }
  return now - last_activity_ >= std::chrono::milliseconds(kIdleAfterMs);
}

void EvoHost::paint_and_publish() {
  const auto now = std::chrono::steady_clock::now();
  const bool idle = throttled(now);
  if (idle != was_throttled_) {
    was_throttled_ = idle;
    log_printf("evo-host: %s", idle ? "idle, publishing at most 10 frames/s, animations paused"
                                    : "active, full frame rate");
    // WebKit renders an animation on the CPU whatever the host takes, so the
    // throttle alone does not stop a page from using a core while it is read.
    view_->run_javascript(idle ? kPauseIdleAnimations : kResumeIdleAnimations);
  }
  if (idle && now - last_publish_ < std::chrono::milliseconds(kIdleFrameMs)) {
    if (!view_->web().dirty_empty()) {
      throttled_skips_++;
    }
    return;  // the damage stays in WebKit's dirty region for the next frame
  }
  display::Region dirty = view_->web().take_dirty();
  if (!dirty.empty() || force_full_) {
    last_publish_ = now;
  }
  if (!dirty.empty()) {
    painted_once_ = true;
  }
  if (!painted_once_) {
    return;  // no WebProcess frame yet: EVO keeps its own loading view
  }
  if (force_full_) {
    force_full_ = false;
    dirty.clear();
    dirty.add(display::Rect{0, 0, width_, height_});
  }
  dirty.clip(display::Rect{0, 0, width_, height_});
  note_damage_rects(dirty);
  for (const display::Rect& r : dirty.rects()) {
    view_->web().paint(reinterpret_cast<unsigned char*>(cache_.data()), width_, height_, r);
  }
  frames_.note_damage(dirty);

  const int b = frames_.next_buffer();
  if (b < 0) {
    return;
  }
  evob_frame f;
  frames_.publish(static_cast<std::uint32_t>(b), cache_.data(), width_, shm_[b], cap_w_, &f);
  if (send_packet(evohost::pack(EVOB_FRAME, &f, sizeof f))) {
    if (frames_sent_++ == 0) {
      log_printf("evo-host: first frame sent (buffer %d, %u rects)", b, f.nrects);
    }
  } else {
    frames_.released(static_cast<std::uint32_t>(b));  // EVO never got it
  }
}

void EvoHost::note_damage_rects(const display::Region& dirty) {
  if (!dirty.empty()) {
    damage_frames_++;
    for (const display::Rect& r : dirty.rects()) {
      damage_seen_[{r.x, r.y, r.w, r.h}]++;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - damage_since_ < std::chrono::milliseconds(kDamageLogMs)) {
    return;
  }
  damage_since_ = now;
  const display::AreaStat nd = view_->web().take_needs_display_stats();
  const std::uint64_t packets = packets_in_ - damage_packets_at_;
  damage_packets_at_ = packets_in_;
  if (nd.count == 0 && damage_frames_ == 0) {
    damage_seen_.clear();
    return;  // an idle page says nothing
  }
  std::vector<std::pair<std::uint32_t, std::array<int, 4>>> top;
  top.reserve(damage_seen_.size());
  for (const auto& [rect, n] : damage_seen_) {
    top.emplace_back(n, rect);
  }
  const std::size_t keep = std::min(top.size(), kDamageTopRects);
  std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(keep), top.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string rects;
  for (std::size_t i = 0; i < keep; ++i) {
    const auto& r = top[i].second;
    char one[64];
    std::snprintf(one, sizeof one, " %ux[%d,%d %dx%d]", top[i].first, r[0], r[1], r[2], r[3]);
    rects += one;
  }
  log_printf("evo-host: damage %ds: needsDisplay %u (avg %.1f kpx), painted frames %llu, "
             "EVO packets %llu, idle skips %llu, %zu distinct rects, top:%s",
             kDamageLogMs / 1000, nd.count, nd.avg_kpx(),
             static_cast<unsigned long long>(damage_frames_),
             static_cast<unsigned long long>(packets),
             static_cast<unsigned long long>(throttled_skips_), damage_seen_.size(),
             rects.c_str());
  damage_seen_.clear();
  damage_frames_ = 0;
  throttled_skips_ = 0;
}

void EvoHost::spawn_progress(const char* event, void* data) {
  auto* self = static_cast<EvoHost*>(data);
  const std::string name = event ? event : "";
  static constexpr char kNoLoader[] = "no-loader:";
  static constexpr char kUnreachable[] = "broker-unreachable:";
  if (name.rfind(kNoLoader, 0) == 0 || name.rfind(kUnreachable, 0) == 0) {
    log_printf("evo-host: web process start failed: %s", name.c_str());
    self->send_event(EVOB_EV_ENGINE_ERROR, 0, name);
  }
}

// --- shell::EngineViewClient: page events to EVO ---------------------------

void EvoHost::url_changed(const std::string& url) { send_event(EVOB_EV_URL, 0, url); }

void EvoHost::title_changed(const std::string& title) { send_event(EVOB_EV_TITLE, 0, title); }

void EvoHost::load_started() {
  loading_ = true;
  note_activity();
  send_event(EVOB_EV_LOAD_STARTED, 0);
}

void EvoHost::load_finished() {
  loading_ = false;
  note_activity();  // the page settles at full rate, then goes idle
  send_event(EVOB_EV_LOAD_FINISHED, 0);
}

void EvoHost::load_failed(const std::string& url, const std::string& error_text) {
  loading_ = false;
  note_activity();
  send_event(EVOB_EV_LOAD_FAILED, 0, url + "\n" + error_text);
}

void EvoHost::nav_state_changed(bool can_go_back, bool can_go_forward) {
  send_event(EVOB_EV_NAV_STATE, (can_go_back ? 1u : 0u) | (can_go_forward ? 2u : 0u));
}

void EvoHost::fullscreen_entered() {
  fullscreen_ = true;  // page video: never throttled
  send_event(EVOB_EV_FULLSCREEN, 1);
}

void EvoHost::fullscreen_exited() {
  fullscreen_ = false;
  note_activity();
  send_event(EVOB_EV_FULLSCREEN, 0);
}

void EvoHost::script_message(const std::string& json) { send_event(EVOB_EV_SCRIPT, 0, json); }

void EvoHost::new_window_requested(const std::string& url) {
  send_event(EVOB_EV_NEW_WINDOW, 0, url);
}

void EvoHost::text_input_focused(bool focused) {
  send_event(EVOB_EV_TEXT_FOCUS, focused ? 1u : 0u);
}

void EvoHost::cursor_changed(shell::PageCursor cursor) {
  std::uint32_t arg = 0;
  switch (cursor) {
    case shell::PageCursor::Pointer: arg = 0; break;
    case shell::PageCursor::Hand: arg = 1; break;
    case shell::PageCursor::None: arg = 2; break;
  }
  send_event(EVOB_EV_CURSOR, arg);
}

}  // namespace prosperoweb::frontend
