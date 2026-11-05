// SPDX-License-Identifier: GPL-3.0-or-later
#include "wk_web_view.h"

#include <algorithm>
#include <chrono>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include <WebKit/WKArray.h>
#include <WebKit/WKContext.h>
#include <WebKit/WKDictionary.h>
#include <WebKit/WKNumber.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageFullScreenClient.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageStateClient.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKString.h>
#include <WebKit/WKType.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKUserScriptInjectionTime.h>

#include <WebKit/WKEventPlayStation.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKPagePrivatePlayStation.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>

#include "ps5_log.h"
#include "wk_helpers.h"

namespace prosperoweb::frontend {

namespace {

WkWebView* self_of(const void* ci) {
  return const_cast<WkWebView*>(static_cast<const WkWebView*>(ci));
}

// Every live view, for WkWebView::poll_all().
std::vector<WkWebView*>& all_views() {
  static std::vector<WkWebView*> views;
  return views;
}

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// How often each view's bridge queue is drained, and when a drain that never
// completed (a crashed WebProcess) stops blocking the next one.
constexpr std::uint64_t kBridgePollMs = 40;
constexpr std::uint64_t kBridgePollTimeoutMs = 3000;
// Name of the top frame's drain function (see bridge_script()).
constexpr const char* kBridgeDrain = "__prosperowebBridge";
// Poll answer when the page has native script message handlers (a drained
// batch is a JSON array, so it cannot look like this).
constexpr const char* kBridgeNative = "#native";

// The document-start shim behind WkWebView::poll_all(). JS builtins are
// captured before page scripts run, so a page cannot break the queue by
// replacing them. String bodies are queued as is, anything else as JSON,
// which is what the native handler hands script_message as well.
std::string bridge_script(const std::string& handler_name) {
  return std::string(R"JS((function (name) {
  'use strict';
  var w = window;
  try {
    if (w.webkit && w.webkit.messageHandlers && w.webkit.messageHandlers[name]) return;
  } catch (e) {
    return;
  }
  var stringify = JSON.stringify;
  var resolve = Promise.resolve.bind(Promise);
  var defineProperty = Object.defineProperty;
  var freeze = Object.freeze;
  var DRAIN = ')JS") + kBridgeDrain + R"JS(';
  var isTop = w === w.top;
  var queue = [];
  function enqueue(text) {
    if (queue.length >= 1024) return;  // nobody drains: drop, do not grow
    queue[queue.length] = text;
  }
  function postMessage(body) {
    var text = typeof body === 'string' ? body : stringify(body);
    if (typeof text !== 'string') return resolve();
    if (isTop) {
      enqueue(text);
    } else {
      try { w.top.postMessage({ __prosperowebBridge: name, body: text }, '*'); } catch (e) {}
    }
    return resolve();
  }
  var handlers = {};
  handlers[name] = freeze({ postMessage: postMessage });
  try {
    defineProperty(w, 'webkit', { value: freeze({ messageHandlers: freeze(handlers) }) });
  } catch (e) {
    return;
  }
  if (!isTop) return;
  w.addEventListener('message', function (ev) {
    var d = ev.data;
    if (d && typeof d === 'object' && d.__prosperowebBridge === name &&
        typeof d.body === 'string') {
      ev.stopImmediatePropagation();
      enqueue(d.body);
    }
  }, true);
  defineProperty(w, DRAIN, {
    value: function () {
      if (!queue.length) return '';
      var out = stringify(queue);
      queue = [];
      return out;
    }
  });
})(')JS" + handler_name + "');\n";
}


// Converts a WKTypeRef (produced by WKScriptMessageGetBody) into JSON.
nlohmann::json wk_value_to_json(WKTypeRef value) {
  if (!value) {
    return nullptr;
  }
  WKTypeID id = WKGetTypeID(value);
  if (id == WKStringGetTypeID()) {
    return to_std_string(static_cast<WKStringRef>(value));
  }
  if (id == WKBooleanGetTypeID()) {
    return static_cast<bool>(WKBooleanGetValue(static_cast<WKBooleanRef>(value)));
  }
  if (id == WKDoubleGetTypeID()) {
    return WKDoubleGetValue(static_cast<WKDoubleRef>(value));
  }
  if (id == WKUInt64GetTypeID()) {
    return WKUInt64GetValue(static_cast<WKUInt64Ref>(value));
  }
  if (id == WKArrayGetTypeID()) {
    auto arr = static_cast<WKArrayRef>(value);
    nlohmann::json out = nlohmann::json::array();
    for (size_t i = 0, n = WKArrayGetSize(arr); i < n; i++) {
      out.push_back(wk_value_to_json(WKArrayGetItemAtIndex(arr, i)));
    }
    return out;
  }
  if (id == WKDictionaryGetTypeID()) {
    auto dict = static_cast<WKDictionaryRef>(value);
    nlohmann::json out = nlohmann::json::object();
    WKArrayRef keys = WKDictionaryCopyKeys(dict);
    for (size_t i = 0, n = WKArrayGetSize(keys); i < n; i++) {
      auto key = static_cast<WKStringRef>(WKArrayGetItemAtIndex(keys, i));
      out[to_std_string(key)] = wk_value_to_json(WKDictionaryGetItemForKey(dict, key));
    }
    WKRelease(keys);
    return out;
  }
  return nullptr;
}

struct EvaluateRequest {
  std::weak_ptr<char> alive;
  WkWebView::EvaluateDone done;
};

void evaluate_finished(WKTypeRef result, WKErrorRef error, void* context) {
  std::unique_ptr<EvaluateRequest> req(static_cast<EvaluateRequest*>(context));
  if (req->alive.expired() || !req->done) {
    return;
  }
  std::optional<std::string> out;
  if (result && !error) {
    if (WKGetTypeID(result) == WKStringGetTypeID()) {
      out = to_std_string(static_cast<WKStringRef>(result));
    } else {
      out = wk_value_to_json(result).dump();
    }
  }
  req->done(std::move(out));
}

// --- navigation client ----------------------------------------------------

void nav_did_start(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
  self_of(ci)->note_navigation();
  auto& cb = self_of(ci)->callbacks();
  if (cb.load_started) {
    cb.load_started();
  }
}

void nav_did_finish(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.load_finished) {
    cb.load_finished();
  }
}

void nav_did_fail(WKPageRef page, WKNavigationRef, WKErrorRef error,
                  WKTypeRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (!cb.load_failed) {
    return;
  }
  std::string url = consume_wk_url(WKPageCopyActiveURL(page));
  std::string desc;
  if (error) {
    WKStringRef s = WKErrorCopyLocalizedDescription(error);
    desc = to_std_string(s);
    if (s) {
      WKRelease(s);
    }
  }
  cb.load_failed(url, desc);
}

// --- state client (title/url/nav state/loading) ---------------------------

void state_did_change_title(const void* ci) {
  // The WkWebView stashes its page in the callbacks' owner; we fetch the
  // current title from the page recorded in the UI client. Simpler: the state
  // client's clientInfo is the WkWebView, and we read the page from it.
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (!cb.title_changed || !self->page()) {
    return;
  }
  WKStringRef t = WKPageCopyTitle(self->page());
  cb.title_changed(to_std_string(t));
  if (t) {
    WKRelease(t);
  }
}

void state_did_change_url(const void* ci) {
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (cb.url_changed && self->page()) {
    cb.url_changed(consume_wk_url(WKPageCopyActiveURL(self->page())));
  }
}

void state_did_change_nav(const void* ci) {
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (cb.nav_state_changed && self->page()) {
    cb.nav_state_changed(WKPageCanGoBack(self->page()),
                         WKPageCanGoForward(self->page()));
  }
}

// --- UI client (popups) ----------------------------------------------------

WKPageRef ui_create_new_page(WKPageRef, WKPageConfigurationRef,
                             WKNavigationActionRef action,
                             WKWindowFeaturesRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.create_new_page && action) {
    WKURLRequestRef req = WKNavigationActionCopyRequest(action);
    if (req) {
      std::string url = consume_wk_url(WKURLRequestCopyURL(req));
      WKRelease(req);
      if (!url.empty()) {
        cb.create_new_page(url);
      }
    }
  }
  // We never let the engine open a window; the shell opens a tab instead.
  return nullptr;
}

// --- script message handler ------------------------------------------------

// Resolve the WkWebView that owns the message's page (for the shared,
// page-routed handler where clientInfo is null).
WkWebView* view_for_message(WKScriptMessageRef message) {
  if (!message) {
    return nullptr;
  }
  WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
  if (!frame) {
    return nullptr;
  }
  WKPageRef page = WKFrameInfoGetPage(frame);
  for (WkWebView* v : all_views()) {
    if (v->page() == page) {
      return v;
    }
  }
  return nullptr;
}

void script_message(WKScriptMessageRef message, WKCompletionListenerRef reply,
                     const void* ci) {
  // Per-view handler: clientInfo is the WkWebView. Shared handler: clientInfo
  // is null, route by the message's page.
  WkWebView* self = ci ? self_of(ci) : view_for_message(message);
  if (!self) {
    if (reply) {
      WKCompletionListenerComplete(reply, nullptr);
    }
    return;
  }
  self->note_native_message("message received");
  auto& cb = self->callbacks();
  if (cb.script_message && message) {
    // A string body is passed through as is (it is expected to hold JSON
    // already); any other value is serialised once. Re-encoding a string
    // would hand the receiver a JSON string instead of the object inside it.
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (body && WKGetTypeID(body) == WKStringGetTypeID()) {
      cb.script_message(to_std_string(static_cast<WKStringRef>(body)));
    } else {
      cb.script_message(wk_value_to_json(body).dump());
    }
  }
  if (reply) {
    WKCompletionListenerComplete(reply, nullptr);
  }
}

void (*g_damage_hook)(void*) = nullptr;
void* g_damage_hook_data = nullptr;

WKRect to_wk_rect(const display::Rect& r) {
  return WKRectMake(r.x, r.y, r.w, r.h);
}

}  // namespace

// --- view-client trampolines (static members) ------------------------------

void WkWebView::s_needs_display(WKViewRef, WKRect rect, const void* ci) {
  auto* self = self_of(ci);
  DirtyRect d{true, static_cast<int>(rect.origin.x), static_cast<int>(rect.origin.y),
              static_cast<int>(rect.size.width), static_cast<int>(rect.size.height)};
  if (d.width <= 0 || d.height <= 0) {
    return;
  }
  if (!self->needs_display_logged_) {
    self->needs_display_logged_ = true;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - self->created_at_)
                             .count();
    log_printf("view %s: first setViewNeedsDisplay %d,%d %dx%d (%lld ms after tab open)",
               self->name_.c_str(), d.x, d.y, d.width, d.height,
               static_cast<long long>(elapsed));
  }
  self->needs_display_stats_.add(static_cast<std::uint64_t>(d.width) * d.height);
  // Several invalidations can arrive between two frames; keep them all.
  self->dirty_.add(display::Rect{d.x, d.y, d.width, d.height});
  if (self->cb_.needs_display) {
    self->cb_.needs_display(d);
  }
  if (g_damage_hook) {
    g_damage_hook(g_damage_hook_data);
  }
}

void WkWebView::set_damage_hook(void (*hook)(void* data), void* data) {
  g_damage_hook = hook;
  g_damage_hook_data = data;
}

// Element fullscreen handshake of the PlayStation port: enterFullScreen
// (the shell hides the band and resizes this view to the whole window), then
// beganEnterFullScreen, which must be implemented: without it WKView's client
// wrapper reports failure and WebKit does not complete the transition. On
// exit WebKit calls exitFullScreen (only while PlayStationWebView knows it is
// fullscreen, patch 0011) and/or beganExitFullScreen; the shell hears once.
void WkWebView::s_enter_fullscreen(WKViewRef, const void* ci) {
  auto* self = self_of(ci);
  self->in_fullscreen_ = true;
  if (self->cb_.enter_fullscreen) {
    self->cb_.enter_fullscreen();
  }
  log_printf("fullscreen: entered (content %dx%d)", self->width_, self->height_);
}

// The final frame is the fullscreen element's rect as the WebProcess laid it
// out. The view was resized (enterFullScreen above) before WebKit got the
// client's answer, and the resize reaches the WebProcess first (patch 0011:
// WKViewSetSize updates the drawing area), so it should match the view.
void WkWebView::s_began_enter_fullscreen(WKViewRef, WKRect, WKRect final_frame, const void* ci) {
  auto* self = self_of(ci);
  const int x = static_cast<int>(final_frame.origin.x);
  const int y = static_cast<int>(final_frame.origin.y);
  const int w = static_cast<int>(final_frame.size.width);
  const int h = static_cast<int>(final_frame.size.height);
  const bool fills = x == 0 && y == 0 && w == self->width_ && h == self->height_;
  log_printf("fullscreen: %s began entering (final frame %d,%d %dx%d, view %dx%d%s)",
             self->name_.c_str(), x, y, w, h, self->width_, self->height_,
             fills ? "" : ", does not fill the view");
}

void WkWebView::notify_fullscreen_exited(const char* how) {
  if (!in_fullscreen_) {
    return;
  }
  in_fullscreen_ = false;
  if (cb_.exit_fullscreen) {
    cb_.exit_fullscreen();
  }
  log_printf("fullscreen: exited (%s, content %dx%d)", how, width_, height_);
}

void WkWebView::s_set_cursor(WKViewRef, WKCursorType cursor, const void* ci) {
  auto* self = self_of(ci);
  if (self->cb_.cursor_changed) {
    self->cb_.cursor_changed(cursor);
  }
}

void WkWebView::s_exit_fullscreen(WKViewRef, const void* ci) {
  self_of(ci)->notify_fullscreen_exited("exitFullScreen");
}

void WkWebView::s_began_exit_fullscreen(WKViewRef, WKRect, WKRect, const void* ci) {
  self_of(ci)->notify_fullscreen_exited("beganExitFullScreen");
}

// --- construction -----------------------------------------------------------

WkWebView::WkWebView(WKContextRef context, WKWebsiteDataStoreRef data_store,
                     const std::string& message_handler_name,
                     const std::vector<std::string>& user_scripts,
                     bool fullscreen_enabled, Callbacks callbacks,
                     WKUserContentControllerRef shared_controller)
    : cb_(std::move(callbacks)), name_(message_handler_name) {
  if (shared_controller) {
    // Use the shared controller (already seeded with base scripts and a
    // page-routed handler); do not add scripts/handler or own it.
    content_controller_ = shared_controller;
    owns_controller_ = false;
  } else {
    content_controller_ = WKUserContentControllerCreate();
    owns_controller_ = true;
    for (const auto& src : base_scripts(message_handler_name, user_scripts)) {
      auto s = make_wk_string(src);
      WKUserScriptRef script = WKUserScriptCreateWithSource(
          s.get(), kWKInjectAtDocumentStart, /*forMainFrameOnly=*/false);
      WKUserContentControllerAddUserScript(content_controller_, script);
      WKRelease(script);
    }
    auto name = make_wk_string(message_handler_name);
    WKUserContentControllerAddScriptMessageHandler(content_controller_, name.get(),
                                                   &script_message, this);
  }

  preferences_ = WKPreferencesCreate();
  WKPreferencesSetFullScreenEnabled(preferences_, fullscreen_enabled);
  // Raster path (WKPagePaint) by default. With /data/prosperoweb/gpu the
  // WebProcess composites layers on the GPU (src/ps5/glslim) and its frames
  // still arrive here as ordinary bitmaps; if its GPU start fails it stays on
  // the raster path by itself (no shared display, patch 0013).
  {
    struct stat gpu_flag;
    const bool gpu = stat("/data/prosperoweb/gpu", &gpu_flag) == 0;
    WKPreferencesSetAcceleratedCompositingEnabled(preferences_, gpu);
    log_printf("view %s: GPU compositing %s", name_.c_str(), gpu ? "requested" : "off");
  }
  // PROSPEROWEB_CONSOLE_LOG=1: the WebProcess writes the page's console
  // messages to its stderr (stderr-<role>.txt with PROSPEROWEB_CHILD_STDERR).
  if (const char* console = std::getenv("PROSPEROWEB_CONSOLE_LOG"); console && !std::strcmp(console, "1")) {
    WKPreferencesSetLogsPageMessagesToSystemConsoleEnabled(preferences_, true);
  }
  // The PlayStation port resolves form-control and default text through the
  // generic font-family preferences (SystemFontDatabasePlayStation returns
  // the standard family). Sony's own browser sets them; unset, buttons and
  // text fields get no font at all and render as empty boxes.
  {
    auto set = [&](void (*fn)(WKPreferencesRef, WKStringRef), const char* family) {
      auto f = make_wk_string(family);
      fn(preferences_, f.get());
    };
    set(WKPreferencesSetStandardFontFamily, "Noto Sans");
    set(WKPreferencesSetSansSerifFontFamily, "Noto Sans");
    set(WKPreferencesSetSerifFontFamily, "Noto Serif");
    set(WKPreferencesSetFixedFontFamily, "Liberation Mono");
    set(WKPreferencesSetCursiveFontFamily, "Noto Serif");
    set(WKPreferencesSetFantasyFontFamily, "Noto Sans");
    set(WKPreferencesSetPictographFontFamily, "Noto Color Emoji");
    WKPreferencesSetDefaultFontSize(preferences_, 16);
    WKPreferencesSetDefaultFixedFontSize(preferences_, 13);
  }
  WKPreferencesSetJavaScriptCanOpenWindowsAutomatically(preferences_, true);

  WKPageConfigurationRef config = WKPageConfigurationCreate();
  WKPageConfigurationSetContext(config, context);
  WKPageConfigurationSetPreferences(config, preferences_);
  WKPageConfigurationSetUserContentController(config, content_controller_);
  if (data_store) {
    WKPageConfigurationSetWebsiteDataStore(config, data_store);
  }

  view_ = WKViewCreate(config);
  WKRelease(config);
  page_ = WKViewGetPage(view_);
  log_printf("view %s: created (view %s, page %s)", name_.c_str(),
             view_ ? "ok" : "NULL", page_ ? "ok" : "NULL");

  WKViewClientV0 view_client{};
  view_client.base.version = 0;
  view_client.base.clientInfo = this;
  view_client.setViewNeedsDisplay = &WkWebView::s_needs_display;
  view_client.enterFullScreen = &WkWebView::s_enter_fullscreen;
  view_client.exitFullScreen = &WkWebView::s_exit_fullscreen;
  view_client.closeFullScreen = &WkWebView::s_exit_fullscreen;
  view_client.beganEnterFullScreen = &WkWebView::s_began_enter_fullscreen;
  view_client.beganExitFullScreen = &WkWebView::s_began_exit_fullscreen;
  view_client.setCursor = &WkWebView::s_set_cursor;
  WKViewSetViewClient(view_, &view_client.base);

  WKPageNavigationClientV0 nav_client{};
  nav_client.base.version = 0;
  nav_client.base.clientInfo = this;
  nav_client.didStartProvisionalNavigation = &nav_did_start;
  nav_client.didFinishNavigation = &nav_did_finish;
  nav_client.didFailNavigation = &nav_did_fail;
  nav_client.didFailProvisionalNavigation = &nav_did_fail;
  WKPageSetPageNavigationClient(page_, &nav_client.base);

  WKPageStateClientV0 state_client{};
  state_client.base.version = 0;
  state_client.base.clientInfo = this;
  state_client.didChangeTitle = &state_did_change_title;
  state_client.didChangeActiveURL = &state_did_change_url;
  state_client.didChangeCanGoBack = &state_did_change_nav;
  state_client.didChangeCanGoForward = &state_did_change_nav;
  WKPageSetPageStateClient(page_, &state_client.base);

  WKPageUIClientV19 ui_client{};
  ui_client.base.version = 19;
  ui_client.base.clientInfo = this;
  ui_client.createNewPage = &ui_create_new_page;
  WKPageSetPageUIClient(page_, &ui_client.base);

  all_views().push_back(this);
}

WkWebView::~WkWebView() {
  auto& views = all_views();
  views.erase(std::remove(views.begin(), views.end(), this), views.end());
  // The view owns the page; release the pieces we created.
  if (view_) {
    WKRelease(view_);
  }
  if (preferences_) {
    WKRelease(preferences_);
  }
  if (content_controller_ && owns_controller_) {
    WKRelease(content_controller_);
  }
}

// --- add-ons: content filters and extra user scripts ------------------------

std::vector<std::string> WkWebView::base_scripts(const std::string& message_handler_name,
                                                 const std::vector<std::string>& user_scripts) {
  std::vector<std::string> scripts;
  scripts.reserve(user_scripts.size() + 1);
  scripts.push_back(bridge_script(message_handler_name));  // before any user of it
  scripts.insert(scripts.end(), user_scripts.begin(), user_scripts.end());
  return scripts;
}

WKUserContentControllerRef WkWebView::create_shared_controller(
    const std::string& message_handler_name, const std::vector<std::string>& user_scripts) {
  WKUserContentControllerRef controller = WKUserContentControllerCreate();
  for (const auto& src : base_scripts(message_handler_name, user_scripts)) {
    auto s = make_wk_string(src);
    WKUserScriptRef script = WKUserScriptCreateWithSource(
        s.get(), kWKInjectAtDocumentStart, /*forMainFrameOnly=*/false);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
  }
  // Page-routed handler (clientInfo == nullptr -> route by the message's page).
  auto name = make_wk_string(message_handler_name);
  WKUserContentControllerAddScriptMessageHandler(controller, name.get(), &script_message, nullptr);
  return controller;
}

void WkWebView::set_controller_filters(WKUserContentControllerRef controller,
                                       const std::vector<WKUserContentFilterRef>& filters) {
  if (!controller) {
    return;
  }
  WKUserContentControllerRemoveAllUserContentFilters(controller);
  for (WKUserContentFilterRef f : filters) {
    if (f) {
      WKUserContentControllerAddUserContentFilter(controller, f);
    }
  }
}

void WkWebView::set_controller_scripts(WKUserContentControllerRef controller,
                                       const std::vector<std::string>& base,
                                       const std::vector<std::string>& extra) {
  if (!controller) {
    return;
  }
  WKUserContentControllerRemoveAllUserScripts(controller);
  auto add = [&](const std::string& src) {
    auto s = make_wk_string(src);
    WKUserScriptRef script = WKUserScriptCreateWithSource(
        s.get(), kWKInjectAtDocumentStart, /*forMainFrameOnly=*/false);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
  };
  for (const auto& src : base) {
    add(src);
  }
  for (const auto& src : extra) {
    add(src);
  }
}

// --- navigation / geometry --------------------------------------------------

void WkWebView::load_url(const std::string& url) {
  navigated_ = true;
  WKURLRef u = WKURLCreateWithUTF8CString(url.c_str());
  WKPageLoadURL(page_, u);
  WKRelease(u);
}

void WkWebView::go_back() { WKPageGoBack(page_); }
void WkWebView::go_forward() { WKPageGoForward(page_); }
void WkWebView::reload() { WKPageReload(page_); }
void WkWebView::stop() { WKPageStopLoading(page_); }

void WkWebView::set_size(int width, int height) {
  width_ = width;
  height_ = height;
  WKViewSetSize(view_, WKSizeMake(width, height));
}

void WkWebView::set_visible(bool visible) {
  WKViewSetVisible(view_, visible);
  WKViewSetActive(view_, visible);
  WKViewSetFocus(view_, visible);
}

// --- input ------------------------------------------------------------------

void WkWebView::pointer_move(int x, int y) {
  WKMouseEvent e = WKMouseEventMake(kWKEventMouseMove,
                                    static_cast<WKEventMouseButton>(held_mouse_button_),
                                    WKPointMake(x, y), 0, 0);
  WKPageHandleMouseEvent(page_, e);
}

void WkWebView::pointer_button(int x, int y, int wk_mouse_button, bool pressed) {
  held_mouse_button_ = pressed ? wk_mouse_button : static_cast<int>(kWKEventMouseButtonNoButton);
  WKMouseEvent e = WKMouseEventMake(pressed ? kWKEventMouseDown : kWKEventMouseUp,
                                    static_cast<WKEventMouseButton>(wk_mouse_button),
                                    WKPointMake(x, y), 1, 0);
  WKPageHandleMouseEvent(page_, e);
}

void WkWebView::scroll(int x, int y, int dx, int dy) {
  if (dx == 0 && dy == 0) {
    return;
  }
  // dx/dy are pixels, positive = scroll the content right/down (EngineView).
  // WebKit's wheel delta is the opposite (positive scrolls up/left, as in the
  // WPE Wayland seat, which negates wl_pointer axis values), hence the sign
  // flip. Patch 0011 sends it as one precise-pixel 2D wheel event.
  WKWheelEvent e = WKWheelEventMake(kWKEventWheel, WKPointMake(x, y),
                                    WKSizeMake(-dx, -dy), WKSizeMake(0, 0), 0);
  WKPageHandleWheelEvent(page_, e);
}

void WkWebView::key(std::uint32_t keysym, std::uint32_t wk_modifiers,
                    bool pressed, const std::string& text) {
  WKKeyboardEvent e = WKKeyboardEventMake(
      pressed ? kWKEventKeyDown : kWKEventKeyUp, kWKInputTypeNormal, text.c_str(),
      static_cast<uint32_t>(text.size()), nullptr,
      static_cast<int32_t>(keysym), 0, 0, wk_modifiers);
  WKPageHandleKeyboardEvent(page_, e);
}

void WkWebView::run_javascript(const std::string& js) {
  auto s = make_wk_string(js);
  WKPageEvaluateJavaScriptInMainFrame(page_, s.get(), nullptr, nullptr);
}

void WkWebView::evaluate(const std::string& body, EvaluateDone done) {
  auto s = make_wk_string(body);
  auto* req = new EvaluateRequest{alive_, std::move(done)};
  // WebCore::ScriptController::callInWorld dereferences the argument
  // dictionary unconditionally: a null one crashes the WebProcess (ud2).
  WKDictionaryRef args = WKDictionaryCreate(nullptr, nullptr, 0);
  WKPageCallAsyncJavaScriptWithoutUserGesture(page_, s.get(), args, nullptr, req,
                                              &evaluate_finished);
  WKRelease(args);
}

// --- script-message bridge --------------------------------------------------

void WkWebView::poll_all() {
  const std::uint64_t now = now_ms();
  // Delivery happens in evaluate() completions, never inside this loop, so
  // no view can go away while it runs.
  for (WkWebView* view : all_views()) {
    view->poll_script_messages(now);
  }
}

void WkWebView::note_native_message(const char* why) {
  if (!native_messages_) {
    native_messages_ = true;
    log_printf("view %s: native script messages (%s), bridge polling stops", name_.c_str(),
               why);
  }
}

void WkWebView::poll_script_messages(std::uint64_t now) {
  if (native_messages_ || !navigated_ || now - last_poll_ms_ < kBridgePollMs) {
    return;
  }
  if (poll_in_flight_ && now - poll_started_ms_ < kBridgePollTimeoutMs) {
    return;
  }
  last_poll_ms_ = now;
  poll_started_ms_ = now;
  poll_in_flight_ = true;
  // Without the drain function the shim stood down because WebKit provides
  // window.webkit.messageHandlers itself (ENABLE_USER_MESSAGE_HANDLERS):
  // kBridgeNative says so, and polling stops for this view.
  evaluate(std::string("var d = window.") + kBridgeDrain +
               "; if (typeof d === 'function') return d();"
               " var h = window.webkit && window.webkit.messageHandlers;"
               " return h && h['" + name_ + "'] ? '" + kBridgeNative + "' : '';",
           [this](std::optional<std::string> batch) {
             poll_in_flight_ = false;
             if (batch && *batch == kBridgeNative) {
               note_native_message("handler present");
             } else if (batch && !batch->empty()) {
               deliver_bridged(*batch);
             }
           });
}

void WkWebView::deliver_bridged(const std::string& batch) {
  const auto items = nlohmann::json::parse(batch, nullptr, /*allow_exceptions=*/false);
  if (!items.is_array()) {
    return;
  }
  if (!bridge_logged_) {
    bridge_logged_ = true;
    log_printf("view %s: script messages arrive through the bridge (%zu queued)",
               name_.c_str(), items.size());
  }
  const std::weak_ptr<char> alive = alive_;
  for (const auto& item : items) {
    if (alive.expired()) {
      return;  // a message closed this view
    }
    if (item.is_string() && cb_.script_message) {
      cb_.script_message(item.get<std::string>());
    }
  }
}

void WkWebView::set_user_agent(const std::string& user_agent) {
  auto s = make_wk_string(user_agent);
  WKPageSetCustomUserAgent(page_, s.get());
  if (user_agent != logged_user_agent_) {
    logged_user_agent_ = user_agent;
    log_printf("ua: %s applied to view %s", user_agent.c_str(), name_.c_str());
  }
}

void WkWebView::request_exit_fullscreen() { WKViewRequestExitFullScreen(view_); }

// --- painting ---------------------------------------------------------------

void WkWebView::paint(unsigned char* argb, int width, int height,
                      const display::Rect& rect) {
  if (!argb || rect.empty()) {
    return;
  }
  const WKRect r = to_wk_rect(rect);
  WKPagePaint(page_, argb, WKSizeMake(width, height), r);
  if (!paint_logged_) {
    paint_logged_ = true;
    log_printf("view %s: first WKPagePaint done (%dx%d, rect %d,%d %dx%d)", name_.c_str(),
               width, height, static_cast<int>(r.origin.x), static_cast<int>(r.origin.y),
               static_cast<int>(r.size.width), static_cast<int>(r.size.height));
  }
}

void WkWebView::set_page_zoom(double factor) {
  WKPageSetPageZoomFactor(page_, factor);
  log_printf("view %s: page zoom %.2f", name_.c_str(), factor);
}

display::Region WkWebView::take_dirty() {
  display::Region d = dirty_;
  dirty_.clear();
  return d;
}

display::AreaStat WkWebView::take_needs_display_stats() {
  display::AreaStat s = needs_display_stats_;
  needs_display_stats_ = display::AreaStat{};
  return s;
}

}  // namespace prosperoweb::frontend
