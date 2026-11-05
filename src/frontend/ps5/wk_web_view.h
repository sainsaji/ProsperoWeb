// SPDX-License-Identifier: GPL-3.0-or-later
//
// A thin C++ wrapper around one WebKit PlayStation WKView + its WKPage: it
// installs the view/navigation/state/UI clients and a user-content script
// message handler, forwards everything through std::function callbacks, and
// exposes the input, navigation and WKPagePaint() surface the frontend needs.
//
// Both the web-content views (Ps5EngineView) and the HTML chrome
// (Ps5ChromeUi) are built on this.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <WebKit/WKBase.h>
#include <WebKit/WKGeometry.h>
#include <WebKit/WKViewClient.h>

#include "damage_region.h"
#include "frame_stats.h"

namespace prosperoweb::frontend {

// A dirty rectangle in view-local pixels.
struct DirtyRect {
  bool valid = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

class WkWebView {
 public:
  struct Callbacks {
    std::function<void(const DirtyRect&)> needs_display;
    std::function<void()> enter_fullscreen;
    std::function<void()> exit_fullscreen;
    std::function<void()> load_started;
    std::function<void()> load_finished;
    std::function<void(const std::string& url, const std::string& error)> load_failed;
    std::function<void(const std::string& url)> url_changed;
    std::function<void(const std::string& title)> title_changed;
    std::function<void(bool can_back, bool can_forward)> nav_state_changed;
    std::function<void(const std::string& url)> create_new_page;
    // Body of a window.webkit.messageHandlers.<name>.postMessage(), as JSON
    // (a string body is passed as is). Delivered natively when WebKit has
    // user message handlers, otherwise through the bridge (poll_all()).
    std::function<void(const std::string& json)> script_message;
    // WKViewClient setCursor: a WKCursorType (kWKCursorTypePointer, Hand,
    // None; the PlayStation port maps every other cursor to Pointer).
    std::function<void(std::uint32_t cursor_type)> cursor_changed;
  };

  // context/data_store are shared and owned by the caller (the factory).
  // message_handler_name is the script-message handler to register (e.g.
  // "prosperoweb" or "chrome"). user_scripts are injected at document start in
  // all frames. fullscreen_enabled toggles the HTML5 Fullscreen API.
  //
  // shared_controller (optional): when non-null the view USES this externally
  // owned user content controller instead of creating its own, and does NOT
  // add the bridge/base scripts or the message handler (the shared controller
  // already carries them, with a page-routed handler). This lets all content
  // views share one controller so an ad-block content rule list - whose
  // SharedMemory handle can be handed over only once - is attached a single
  // time and covers every page.
  WkWebView(WKContextRef context, WKWebsiteDataStoreRef data_store,
            const std::string& message_handler_name,
            const std::vector<std::string>& user_scripts,
            bool fullscreen_enabled, Callbacks callbacks,
            WKUserContentControllerRef shared_controller = nullptr);
  ~WkWebView();

  WkWebView(const WkWebView&) = delete;
  WkWebView& operator=(const WkWebView&) = delete;

  WKViewRef view() const { return view_; }
  WKPageRef page() const { return page_; }
  const std::string& name() const { return name_; }

  // The full base user-script set (bridge shim + the given user_scripts) a
  // shared controller is seeded with; the add-ons manager re-adds these before
  // its extra scripts when it rebuilds the controller's scripts.
  static std::vector<std::string> base_scripts(const std::string& message_handler_name,
                                               const std::vector<std::string>& user_scripts);
  // Creates a user content controller seeded with base_scripts() and a
  // page-routed script-message handler. Owned (released) by the caller.
  static WKUserContentControllerRef create_shared_controller(
      const std::string& message_handler_name, const std::vector<std::string>& user_scripts);
  // Replace all content filters on a controller (RemoveAll + Add each).
  static void set_controller_filters(WKUserContentControllerRef controller,
                                     const std::vector<WKUserContentFilterRef>& filters);
  // Replace all user scripts on a controller with base then extra (RemoveAll
  // + re-add), so toggling an add-on never drops the base scripts.
  static void set_controller_scripts(WKUserContentControllerRef controller,
                                     const std::vector<std::string>& base,
                                     const std::vector<std::string>& extra);

  // Accessor for the C-API trampolines (which receive this as clientInfo).
  Callbacks& callbacks() { return cb_; }

  // Navigation.
  void load_url(const std::string& url);
  void go_back();
  void go_forward();
  void reload();
  void stop();

  // Geometry / visibility.
  void set_size(int width, int height);
  int width() const { return width_; }
  int height() const { return height_; }
  void set_visible(bool visible);

  // Trusted input (real engine events).
  // Moves carry the button held since pointer_button(.., true), so a press,
  // moves and a release make a drag (text selection, sliders).
  void pointer_move(int x, int y);
  void pointer_button(int x, int y, int wk_mouse_button, bool pressed);
  void scroll(int x, int y, int dx, int dy);
  // keysym is a WPE keysym (WPE_KEY_*), which is what the PlayStation
  // WKPageHandleKeyboardEvent expects in virtualKeyCode.
  void key(std::uint32_t keysym, std::uint32_t wk_modifiers, bool pressed,
           const std::string& text);

  void run_javascript(const std::string& js);
  // Runs body as an async function in the main frame, without a user
  // gesture (`return` a value). done receives the result as a string (a
  // non-string result serialised as JSON), or nullopt if the script failed
  // or the page has no process. done is not called once the view is gone.
  using EvaluateDone = std::function<void(std::optional<std::string> result)>;
  void evaluate(const std::string& body, EvaluateDone done);
  void set_user_agent(const std::string& user_agent);
  void request_exit_fullscreen();

  // Paints rect into an ARGB32 (little-endian: B,G,R,A bytes) buffer of size
  // width*height*4. rect is clamped to the surface by WebKit.
  void paint(unsigned char* argb, int width, int height, const display::Rect& rect);

  // Returns and clears the pending damage: every needs_display rect since
  // the last call, coalesced into a few rects (display::Region).
  display::Region take_dirty();
  // True when no damage is pending (take_dirty would return an empty region).
  bool dirty_empty() const { return dirty_.empty(); }

  // Returns and clears the setViewNeedsDisplay count and area since the last
  // call (the frame-pipeline statistics).
  display::AreaStat take_needs_display_stats();

  // Script-message bridge, the fallback for a WebKit built without
  // ENABLE_USER_MESSAGE_HANDLERS (upstream PlayStation default; the PS5 build
  // turns it on), where window.webkit.messageHandlers does not exist. Every
  // view injects a document-start shim that provides
  // window.webkit.messageHandlers.<name>.postMessage() and queues the bodies
  // (subframes forward theirs to the top frame); poll_all() drains the
  // queues through evaluate() and delivers them to script_message. When
  // native handlers do exist, the shim stands aside and polling stops at the
  // first poll that finds them or the first native message.
  static void poll_all();
  // Called (on the main thread) after any view's setViewNeedsDisplay: the
  // frame loop schedules a frame at once instead of waiting for its tick.
  static void set_damage_hook(void (*hook)(void* data), void* data);
  // Marks the view as having started a navigation (polling starts then).
  void note_navigation() { navigated_ = true; }
  // Called by the native handler trampoline.
  void note_native_message(const char* why);

 private:
  // C-API trampolines (clientInfo == this).
  static void s_needs_display(WKViewRef, WKRect, const void*);
  static void s_enter_fullscreen(WKViewRef, const void*);
  static void s_exit_fullscreen(WKViewRef, const void*);
  static void s_began_enter_fullscreen(WKViewRef, WKRect, WKRect, const void*);
  static void s_began_exit_fullscreen(WKViewRef, WKRect, WKRect, const void*);
  static void s_set_cursor(WKViewRef, WKCursorType, const void*);
  void notify_fullscreen_exited(const char* how);
  void poll_script_messages(std::uint64_t now_ms);
  void deliver_bridged(const std::string& batch);

  Callbacks cb_;
  WKViewRef view_ = nullptr;
  bool owns_controller_ = true;  // false when using a shared controller
  WKPageRef page_ = nullptr;
  WKUserContentControllerRef content_controller_ = nullptr;
  WKPreferencesRef preferences_ = nullptr;
  display::Region dirty_;
  display::AreaStat needs_display_stats_;
  int width_ = 1920;
  int height_ = 1080;
  int held_mouse_button_ = -2;  // kWKEventMouseButtonNoButton
  bool in_fullscreen_ = false;
  // Diagnostics: the view's name in log.txt and one-shot "first" events.
  std::string name_;
  std::string logged_user_agent_;  // last value logged by set_user_agent()
  bool needs_display_logged_ = false;
  bool paint_logged_ = false;
  // When the view (a new tab) was created, for the tab-open-to-first-paint
  // latency logged at the first setViewNeedsDisplay.
  std::chrono::steady_clock::time_point created_at_ = std::chrono::steady_clock::now();

  // Expires with the view: pending evaluate() completions check it.
  std::shared_ptr<char> alive_ = std::make_shared<char>(0);
  bool navigated_ = false;
  bool native_messages_ = false;
  bool bridge_logged_ = false;
  bool poll_in_flight_ = false;
  std::uint64_t poll_started_ms_ = 0;
  std::uint64_t last_poll_ms_ = 0;
};

}  // namespace prosperoweb::frontend
