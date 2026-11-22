// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// Linux backend: a GTK window hosting WebKitGTK. Built for one of
//   GTK 4 + webkitgtk-6.0       (UNIAPI_GTK4 defined; CMake's default when found)
//   GTK 3 + webkit2gtk-4.1      (otherwise)
// The two differ only in window setup, dialogs and a few renamed WebKit calls; see #if UNIAPI_GTK4.
// Further windows (App::openWindow) are LinuxWindow: their own GtkWindow and web view in the main
// window's web context.
//
//   page -> app messages    window.__uniapiPost(text), bridged to a script message handler in
//                           its own script world (top frame only; see AddBridge)
//   app -> page messages    webkit_web_view_evaluate_javascript("window.__uniapi.receive(...)")
//   page files and fetches  the custom URI scheme uniapi://<host>/
//   shared memory           memfd regions, mapped into the page by uniapi's web process
//                           extension (extension/extension.cpp), when every check passes:
//                             - the extension library is found (see ExtensionDirectory)
//                             - the extension answers a handshake within 2 seconds (once
//                               the web process has started, which may take up to 10), and
//                               confirms that Gigacage is off in the web process (below)
//                           otherwise streams use the fetch fallback.
//
// Set G_MESSAGES_DEBUG=uniapi to see which check decided. WebKitGTK must be 2.40 or newer
// in any case (evaluate_javascript, request bodies); that also covers what the extension needs
// (ArrayBuffers over outside memory, 2.38).
//
// Gigacage: JavaScriptCore keeps ArrayBuffer memory inside a reserved address range, a
// hardening layer against JavaScript engine exploits. An ArrayBuffer over outside memory
// (the zero-copy part) needs it switched off, and WebKitGTK's web process aborts rather than
// do that at run time. So when shared memory is on, the backend sets GIGACAGE_ENABLED=no
// before WebKit starts its processes; WebKit prints "Warning: disabling gigacage" for each.
// uniapi pages are the app's own, which keeps the risk low. AppOptions::sharedMemory = false
// leaves Gigacage alone.

#include "core/backend.hpp"

#define G_LOG_DOMAIN "uniapi"

#include <gtk/gtk.h>
#include <libsoup/soup.h>
#ifdef UNIAPI_GTK4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif

#include <gio/gunixfdlist.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <vector>

namespace uniapi {

std::string executablePath() {
    std::vector<char> buffer(4096);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    return length > 0 ? std::string(buffer.data(), size_t(length)) : std::string();
}

std::vector<std::string> arguments() {
    // Each argument ends with a NUL; the first is the program itself.
    std::ifstream file("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> result;
    size_t start = 0;
    bool first = true;
    while (start < all.size()) {
        size_t end = all.find('\0', start);
        if (end == std::string::npos) end = all.size();
        if (!first) result.push_back(all.substr(start, end - start));
        first = false;
        start = end + 1;
    }
    return result;
}

namespace {

// Whether GTK has been started (by run(), or by a dialog or alert() before it). From then on
// GTK's helper threads may read the environment, so setenv() is no longer safe.
bool gtkStarted = false;

#ifdef UNIAPI_GTK4

bool InitGtk() {
    // GTK's OpenGL renderer instead of its default Vulkan one (GTK 4.16+). With Vulkan, putting
    // each web frame on screen took about one display refresh on NVIDIA + Wayland, and the page
    // waited for it: page updates dropped to 30 per second and streaming at 1080p went from
    // ~315 to ~160 fps. With it GTK 4 is as fast as GTK 3 or faster. Its name
    // is "gl" since GTK 4.18 (which removed the old GL renderer and warns about "ngl" on every
    // start), "ngl" before. Set before GTK picks its renderer. A value you set yourself wins.
    const bool renamed = gtk_get_major_version() > 4 || gtk_get_minor_version() >= 18;
    setenv("GSK_RENDERER", renamed ? "gl" : "ngl", 0);  // no change once set: safe again later
    // The window's app id on Wayland (WM_CLASS on X11), which the desktop matches to the app's
    // .desktop file for its icon and taskbar entry: GTK 4 takes GLib's program name, which
    // nothing sets without argv (GTK 3's gtk_init does), so every app was "GTK Application".
    // The executable's name, as GTK 3; one the app set itself wins.
    if (!g_get_prgname()) {
        const std::string exe = executablePath();
        if (!exe.empty()) g_set_prgname(exe.substr(exe.find_last_of('/') + 1).c_str());
    }
    gtkStarted = true;
    return gtk_init_check();
}

// GTK 4 dialogs are asynchronous; uniapi's are modal calls that return the answer, so wait in
// a nested main loop (as GTK 3's gtk_dialog_run did). The page keeps running meanwhile.
struct DialogWait {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    GFile* file = nullptr;
    GListModel* files = nullptr;
    ~DialogWait() { g_main_loop_unref(loop); }
    void Run() { g_main_loop_run(loop); }
};

}  // namespace

void alert(std::string_view title, std::string_view message) {
    if (!InitGtk()) return;
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", std::string(title).c_str());
    gtk_alert_dialog_set_detail(dialog, std::string(message).c_str());
    gtk_alert_dialog_set_modal(dialog, TRUE);
    DialogWait wait;
    gtk_alert_dialog_choose(dialog, nullptr, nullptr,
                            +[](GObject* source, GAsyncResult* result, gpointer data) {
                                gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
                                g_main_loop_quit(static_cast<DialogWait*>(data)->loop);
                            },
                            &wait);
    wait.Run();
    g_object_unref(dialog);
}

#else  // GTK 3

// Whether GTK is going to open an X11 display, decided before it starts (as GTK does): the
// first entry of GDK_BACKEND if set, else Wayland when there is a Wayland display, else X11.
bool WillUseX11() {
    if (const char* backends = getenv("GDK_BACKEND"); backends && *backends) {
        const std::string first(backends, std::strcspn(backends, ","));
        return first == "x11";
    }
    const char* wayland = getenv("WAYLAND_DISPLAY");
    return !(wayland && *wayland) && getenv("DISPLAY") != nullptr;
}

bool InitGtk() {
    // GTK 3 draws the window through EGL when the web view renders with the GPU, but some
    // frames in software. NVIDIA's EGL turns on Wayland explicit sync for the window, and a
    // software frame on it is then a protocol error that kills the process ("Error 71").
    // Turning explicit sync off for this process avoids it; it only affects NVIDIA's driver.
    // Set before GTK loads EGL, and inherited by WebKit's child processes. A value you set
    // yourself wins.
    setenv("__NV_DISABLE_EXPLICIT_SYNC", "1", 0);  // no change once set: safe again later
    // File dialogs: GtkFileChooserNative uses the desktop's own dialog (through the portal, as
    // GTK 4 does by itself) only when told to; otherwise GTK's built-in one, which looks foreign
    // on KDE and makes GLib log CRITICALs there. Without a portal, GTK falls back to its own.
    setenv("GTK_USE_PORTAL", "1", 0);
    // Under X11, WebKit's DMA-BUF renderer can fail to create its buffers (seen on NVIDIA:
    // "Failed to create GBM buffer") and the page stays blank. Its other path showed the page at
    // the same frame rates (measured with the demo, 2026-11-04). Wayland is fine either way.
    if (WillUseX11()) setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    gtkStarted = true;
    return gtk_init_check(nullptr, nullptr);
}

}  // namespace

void alert(std::string_view title, std::string_view message) {
    if (!InitGtk()) return;
    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK, "%s", std::string(message).c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), std::string(title).c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

#endif

namespace detail {
namespace {

constexpr char kScheme[] = "uniapi";
constexpr char kSchemeRegisteredKey[] = "uniapi-scheme-registered";  // on a WebKitWebContext
constexpr char kBackendKey[] = "uniapi-backend";                     // on our WebKitWebView
// The message handler lives in its own script world, out of reach of page scripts and iframes
// (WebKitGTK doesn't say which frame sent a message, and any frame can see a handler in the
// default world: a third-party iframe in an app page could call C++). See AddBridge().
constexpr char kWorld[] = "uniapi";
constexpr char kExtensionFile[] = "libuniapi_webext.so";
// The extension's answer, counted from the handshake (the blank page has loaded, so the web
// process is running). Starting that process is counted apart, with more room: on a slow start
// (about a second each with GTK 3's WebKit and its DMA-BUF renderer on Intel Haswell graphics;
// several views start one after another)
// one count for both took shared memory away from every window opened before run().
constexpr guint kHandshakeTimeoutMs = 2000;
constexpr guint kWebProcessStartTimeoutMs = 10000;

// Where the web process extension is, or "" if it isn't found. WebKitGTK loads every library
// in that directory, so it holds only ours. Looked for in order:
//   $UNIAPI_WEBEXT_DIR
//   uniapi-webext/ next to the executable                 (a self-contained folder)
//   ../lib/<executable name>/uniapi-webext/ from it       (installed with uniapi_install())
//   the build tree
std::string ExtensionDirectory() {
    auto has = [](const std::string& dir) {
        return !dir.empty() && access((dir + "/" + kExtensionFile).c_str(), R_OK) == 0;
    };
    if (const char* dir = getenv("UNIAPI_WEBEXT_DIR")) return has(dir) ? dir : "";
    const std::string exe = executablePath();
    const std::string exeDir = exe.substr(0, exe.find_last_of('/') + 1);
    const std::string exeName = exe.substr(exeDir.size());
    for (const std::string& dir : {exeDir + "uniapi-webext", exeDir + "../lib/" + exeName + "/uniapi-webext"}) {
        if (has(dir)) return dir;
    }
#ifdef UNIAPI_WEBEXT_BUILD_DIR
    if (has(UNIAPI_WEBEXT_BUILD_DIR)) return UNIAPI_WEBEXT_BUILD_DIR;
#endif
    return "";
}

// A memfd mapped into the app; the extension maps the same memory into the web process.
class LinuxRegion final : public SharedRegion {
public:
    int fd = -1;
    ~LinuxRegion() override {
        if (data) munmap(data, size);
        if (fd >= 0) close(fd);
    }
};

// A JavaScript string literal holding `text`. JSON strings are valid JavaScript; invalid
// UTF-8 is replaced rather than thrown on.
std::string JsString(const std::string& text) {
    return json(text).dump(-1, ' ', false, json::error_handler_t::replace);
}

// What a web view's requests go to: the main window (LinuxBackend) or a further window
// (LinuxWindow). Kept on the view under kBackendKey, so it dies with the view.
class ViewHost {
public:
    virtual ~ViewHost() = default;
    virtual void OnSchemeRequest(WebKitURISchemeRequest* request) = 0;
};

// A message to the page: window.__uniapi.receive(text). Calls run in order in the page. Before
// uniapi.js has loaded there is nobody to receive them, same as on the other platforms: the
// page starts with "hello".
void PostScript(WebKitWebView* view, const std::string& text) {
    if (!view) return;
    std::string script = "window.__uniapi&&window.__uniapi.receive(" + JsString(text) + ")";
    webkit_web_view_evaluate_javascript(view, script.data(), gssize(script.size()), nullptr, nullptr, nullptr,
                                        nullptr, nullptr);
}

// A shared region to the view's page, through uniapi's web process extension (which maps it).
// Each view gets its own copy of the file descriptor: one region can go to several pages.
// `writable` (a sink's slot): the extension maps it shared, so the page's writes reach the app;
// otherwise (a stream's) copy-on-write, so they stay the page's own.
void SendRegion(WebKitWebView* view, SharedRegion& region, const std::string& infoJson, bool writable) {
    auto& linuxRegion = static_cast<LinuxRegion&>(region);
    if (!view || linuxRegion.fd < 0) return;
    GUnixFDList* fds = g_unix_fd_list_new();
    if (g_unix_fd_list_append(fds, linuxRegion.fd, nullptr) >= 0) {  // duplicates it
        WebKitUserMessage* message = webkit_user_message_new_with_fd_list(
            "uniapi.region",
            g_variant_new("(stb)", infoJson.c_str(), guint64(linuxRegion.size), gboolean(writable)), fds);
        webkit_web_view_send_message_to_page(view, message, nullptr, nullptr, nullptr);
    }
    g_object_unref(fds);
}

// How the page reaches the handler: the top frame gets window.__uniapiPost (in the page's
// world), which dispatches a DOM event on its own document; a script in the handler's world,
// also in the top frame only, forwards it. An iframe from another origin can't touch the top
// document, so it has no way in. uniapi.js uses __uniapiPost.
void AddBridge(WebKitUserContentManager* manager) {
    static const char kPage[] =
        "window.__uniapiPost = (text) => "
        "document.dispatchEvent(new CustomEvent('uniapi:post', { detail: String(text) }));";
    static const char kForward[] =
        "document.addEventListener('uniapi:post', (event) => {"
        "  if (typeof event.detail === 'string') window.webkit.messageHandlers.uniapi.postMessage(event.detail);"
        "});";
    WebKitUserScript* page = webkit_user_script_new(kPage, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    WebKitUserScript* forward =
        webkit_user_script_new_for_world(kForward, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                         WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kWorld, nullptr, nullptr);
    webkit_user_content_manager_add_script(manager, page);
    webkit_user_content_manager_add_script(manager, forward);
    webkit_user_script_unref(page);
    webkit_user_script_unref(forward);
}

// A message from the view's page, for the core. Only pages the app serves itself may talk to it.
void DeliverScriptMessage(WebKitWebView* view, const std::string& origin, const BackendEvents& events, JSCValue* value) {
    const char* uri = view ? webkit_web_view_get_uri(view) : nullptr;
    const std::string prefix = origin + "/";
    if (!uri || std::strncmp(uri, prefix.c_str(), prefix.size()) != 0) return;
    if (!jsc_value_is_string(value)) return;
    char* text = jsc_value_to_string(value);
    std::string message(text ? text : "");
    g_free(text);
    if (events.onMessage) events.onMessage(message);
}

// The POST body. WebKit hands the request over with its body complete, so reading it here
// doesn't wait on the page.
void ReadBody(WebKitURISchemeRequest* request, std::vector<uint8_t>& body) {
    GInputStream* stream = webkit_uri_scheme_request_get_http_body(request);
    if (!stream) return;
    constexpr size_t kChunk = 1 << 20;
    for (;;) {
        const size_t used = body.size();
        body.resize(used + kChunk);
        gsize read = 0;
        const bool ok = g_input_stream_read_all(stream, body.data() + used, kChunk, &read, nullptr, nullptr);
        body.resize(used + read);
        if (!ok || read < kChunk) break;
    }
    g_object_unref(stream);
}

// Answers a request for uniapi://<host>/...: the core's response, or 404.
void AnswerSchemeRequest(WebKitURISchemeRequest* request, const std::string& origin, const BackendEvents& events) {
    const std::string uri = webkit_uri_scheme_request_get_uri(request);
    Response response;
    if (uri.size() > origin.size() && uri.compare(0, origin.size(), origin) == 0 &&
        uri[origin.size()] == '/' && events.onRequest) {
        Request info;
        info.path = uri.substr(origin.size());
        if (const char* method = webkit_uri_scheme_request_get_http_method(request)) info.method = method;
        if (info.method == "POST") ReadBody(request, info.body);
        events.onRequest(info, response);
    }

    GBytes* bytes;
    if (response.status == 200) {
        // No copy: the bytes stay owned by keepAlive until WebKit is done reading them.
        auto* keepAlive = new std::shared_ptr<const void>(std::move(response.keepAlive));
        bytes = g_bytes_new_with_free_func(response.data, response.size,
                                           +[](gpointer p) { delete static_cast<std::shared_ptr<const void>*>(p); },
                                           keepAlive);
    } else {
        static const char kNotFound[] = "Not Found";
        bytes = g_bytes_new_static(kNotFound, sizeof(kNotFound) - 1);
        response.mimeType = "text/plain";
    }
    const gsize size = g_bytes_get_size(bytes);
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);

    WebKitURISchemeResponse* reply = webkit_uri_scheme_response_new(stream, gint64(size));
    g_object_unref(stream);
    webkit_uri_scheme_response_set_status(reply, response.status == 200 ? 200 : 404, nullptr);
    webkit_uri_scheme_response_set_content_type(reply, response.mimeType.c_str());
    SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Cache-Control", "no-store");
    soup_message_headers_append(headers, "Content-Type", response.mimeType.c_str());
    webkit_uri_scheme_response_set_http_headers(reply, headers);  // takes ownership
    webkit_uri_scheme_request_finish_with_response(request, reply);
    g_object_unref(reply);
}

class LinuxWindow;

// AppOptions::devTools on a new web view: "Inspect Element" in its menu, and the Web Inspector
// always in a window of its own. WebKit opens it docked in the page's window (taking the page's
// lower part) and has no setting for that, so each time it docks (when it opens, and when its own
// dock buttons ask) it is told to detach a moment later, which opens its window. (Answering the
// request "done" instead, so that it never docks, leaves it with no window at all.)
void AllowDevTools(WebKitWebView* view) {
    webkit_settings_set_enable_developer_extras(webkit_web_view_get_settings(view), TRUE);
    g_signal_connect(webkit_web_view_get_inspector(view), "attach",
                     G_CALLBACK(+[](WebKitWebInspector* inspector, gpointer) -> gboolean {
                         g_idle_add(
                             +[](gpointer data) -> gboolean {
                                 auto* inspector = static_cast<WebKitWebInspector*>(data);
                                 webkit_web_inspector_detach(inspector);
                                 g_object_unref(inspector);
                                 return G_SOURCE_REMOVE;
                             },
                             g_object_ref(inspector));
                         return FALSE;
                     }),
                     nullptr);
}

// AppOptions::contextMenu off: the view's right-click menu keeps its text editing items only, with
// separators only between them; no menu at all when none is left (returns TRUE then).
gboolean KeepEditingItems(WebKitContextMenu* menu) {
    static const WebKitContextMenuAction kEditing[] = {
        WEBKIT_CONTEXT_MENU_ACTION_CUT,    WEBKIT_CONTEXT_MENU_ACTION_COPY,       WEBKIT_CONTEXT_MENU_ACTION_PASTE,
        WEBKIT_CONTEXT_MENU_ACTION_DELETE, WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL, WEBKIT_CONTEXT_MENU_ACTION_PASTE_AS_PLAIN_TEXT};
    std::vector<WebKitContextMenuItem*> remove;
    WebKitContextMenuItem* lastKept = nullptr;
    bool afterSeparator = true;  // (or at the start): no separator may follow
    int kept = 0;
    for (GList* l = webkit_context_menu_get_items(menu); l; l = l->next) {
        auto* item = static_cast<WebKitContextMenuItem*>(l->data);
        const bool separator = webkit_context_menu_item_is_separator(item);
        bool keep = false;
        if (separator) {
            keep = !afterSeparator;
        } else {
            const WebKitContextMenuAction action = webkit_context_menu_item_get_stock_action(item);
            for (WebKitContextMenuAction editing : kEditing) keep = keep || action == editing;
        }
        if (!keep) {
            remove.push_back(item);
            continue;
        }
        afterSeparator = separator;
        lastKept = item;
        if (!separator) ++kept;
    }
    if (lastKept && afterSeparator) remove.push_back(lastKept);  // a separator at the end
    for (WebKitContextMenuItem* item : remove) webkit_context_menu_remove(menu, item);
    return kept == 0 ? TRUE : FALSE;
}

void LimitContextMenu(WebKitWebView* view) {
#ifdef UNIAPI_GTK4
    g_signal_connect(view, "context-menu",
                     G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, WebKitHitTestResult*, gpointer) -> gboolean {
                         return KeepEditingItems(menu);
                     }),
                     nullptr);
#else
    g_signal_connect(view, "context-menu",
                     G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, GdkEvent*, WebKitHitTestResult*,
                                    gpointer) -> gboolean { return KeepEditingItems(menu); }),
                     nullptr);
#endif
}

// Opens the Web Inspector on a view's page (detached: AllowDevTools); nothing without devTools.
void OpenDevToolsOf(WebKitWebView* view, bool devTools) {
    if (!view || !devTools) return;
    webkit_web_inspector_show(webkit_web_view_get_inspector(view));
}

// A window's geometry hints (GTK 3; GTK 4 has none, so there this does nothing):
// - keepAspectRatio: an aspect hint, which X11 window managers apply while resizing. Wayland has
//   no such request.
// - the maximum size (maxWidth, maxHeight; a side left at 0 has none), which the window manager
//   and compositor apply, on X11 and Wayland.
template <typename Options>
void ApplyGeometryHints(GtkWindow* window, const Options& options) {
#ifndef UNIAPI_GTK4
    if (!window) return;
    GdkGeometry geometry = {};
    int hints = 0;
    if (options.keepAspectRatio && options.width > 0 && options.height > 0) {
        geometry.min_aspect = geometry.max_aspect = double(options.width) / options.height;
        hints |= GDK_HINT_ASPECT;
    }
    if (options.maxWidth > 0 || options.maxHeight > 0) {
        geometry.max_width = options.maxWidth > 0 ? options.maxWidth : G_MAXINT;
        geometry.max_height = options.maxHeight > 0 ? options.maxHeight : G_MAXINT;
        hints |= GDK_HINT_MAX_SIZE;
    }
    if (hints) gtk_window_set_geometry_hints(window, nullptr, &geometry, GdkWindowHints(hints));
#else
    (void)window;
    (void)options;
#endif
}

// backgroundColor (0xRRGGBB, or -1: the theme's): the window's own background (what shows until
// the page has drawn, see HidePage) and the web view's (through a page with no background, and
// at its edges while resizing). The window's is a style class of its own, in a provider for the
// whole display.
void ApplyBackground(GtkWidget* window, WebKitWebView* view, int rgb) {
    if (rgb < 0 || !window || !view) return;
    const GdkRGBA color = {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f};
    webkit_web_view_set_background_color(view, &color);
    char name[32];
    std::snprintf(name, sizeof name, "uniapi-background-%06x", rgb);
    char css[96];
    std::snprintf(css, sizeof css, "window.%s { background-color: #%06x; }", name, rgb);
    GtkCssProvider* provider = gtk_css_provider_new();
#ifdef UNIAPI_GTK4
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS  // load_from_data: load_from_string only exists from GTK 4.12
    gtk_css_provider_load_from_data(provider, css, -1);
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(window), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_add_css_class(window, name);
#else
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(window), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), name);
#endif
    g_object_unref(provider);
}

// The page is hidden (HidePage) and appears once it has loaded and drawn
// a frame (kRevealAfterDrawMs after that frame, for it to reach the screen), or after the
// timeout; longer when the page shows it (showWhenReady). The timeouts count from the page's
// load, not from the window's creation: WebKit's own start (its view and web process) takes
// ~0.35 s here, but more than a second on the first start after a reboot, and counted from
// the window it used up the 3 s, so a slow first start showed the page half built. Until a
// page has loaded, kRevealStartTimeoutMs (WebKit didn't start, or the app never navigates).
constexpr guint kRevealAfterDrawMs = 20;
constexpr guint kRevealTimeoutMs = 3000;
constexpr guint kRevealTimeoutReadyMs = 5000;
constexpr guint kRevealStartTimeoutMs = 15000;

// Never a white window, or a page half built: until its page has drawn, the window hides it,
// while the page loads and runs as if shown.
// - GTK 3 on X11 with a compositing manager: the whole window at opacity 0, which the window
//   manager applies, frame and all; the window still draws as usual.
// - Elsewhere (GTK 4; GTK 3 on Wayland, or on X11 without a compositor): the window opens as
//   usual, with the web view nearly transparent, so the window's plain background shows. Not
//   the whole window at 0: GTK 4 doesn't draw a widget whose opacity rounds to 0 in 8 bits,
//   and WebKit, waiting for its last frame to be taken, then renders nothing new. The page got
//   no animation frames, and when the window appeared it showed WebKit's last frame, from
//   before the page's scripts had built it (seen at every start of a page built by scripts). So
//   1/255, the least that counts. GTK 3 on Wayland can't make a window transparent at all, and
//   showed everything at once: the window, the web view's white, the page half built.
constexpr double kHiddenViewOpacity = 1.0 / 255;

// Whether the whole window is hidden (GTK 3 on X11 with a compositor), not just its web view.
bool HidesWholeWindow() {
#ifdef UNIAPI_GTK4
    return false;
#else
    GdkDisplay* display = gdk_display_get_default();
    return display && g_strcmp0(G_OBJECT_TYPE_NAME(display), "GdkX11Display") == 0 &&
           gdk_screen_is_composited(gdk_screen_get_default());
#endif
}

void HidePage(GtkWidget* window, GtkWidget* view) {
    if (HidesWholeWindow()) {
        gtk_widget_realize(window);  // GTK 3 drops a top-level's opacity set before it has its X window
        gtk_widget_set_opacity(window, 0.0);
    } else {
        gtk_widget_set_opacity(view, kHiddenViewOpacity);
    }
}

void ShowPage(GtkWindow* window, WebKitWebView* view) {
    if (HidesWholeWindow()) {
        if (window) gtk_widget_set_opacity(GTK_WIDGET(window), 1.0);
    } else if (view) {
        gtk_widget_set_opacity(GTK_WIDGET(view), 1.0);
    }
}

// The close button, Alt+F4, the window manager's Close: host->AskClose() decides (false keeps
// the window open). gtk_window_close from Close/Quit goes through the same signal; the host
// lets it through without asking.
template <typename Host>
void ConnectCloseRequest(GtkWidget* window, Host* host) {
#ifdef UNIAPI_GTK4
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer self) -> gboolean {
                         return static_cast<Host*>(self)->AskClose() ? FALSE : TRUE;
                     }), host);
#else
    g_signal_connect(window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer self) -> gboolean {
                         return static_cast<Host*>(self)->AskClose() ? FALSE : TRUE;
                     }), host);
#endif
}

// Windows can be placed (and found) only on X11; Wayland doesn't let apps.
bool IsX11() {
    GdkDisplay* display = gdk_display_get_default();
    return display && g_strcmp0(G_OBJECT_TYPE_NAME(display), "GdkX11Display") == 0;
}

// --- Placement ---
// Size, minimised, maximised: everywhere. The position (and centring, and always on top): GTK 3
// on X11 only. Wayland doesn't let apps place their windows or know where they are, and GTK 4
// has no API for either, on X11 too. One per window, the main one and further ones: what it
// asked for before the window existed, and what the window tells as it goes.
class GtkPlacement {
public:
    std::function<void()> changed;  // the window moved, was resized, minimised, maximised...
    const bool* hidden = nullptr;   // the window's: hide(), startHidden

    ~GtkPlacement() { Detach(); }

    // The window made, before it is shown: where it opens, on top, and its placement events.
    void Attach(GtkWindow* window, WebKitWebView* view) {
        window_ = window;
        view_ = view;
#ifndef UNIAPI_GTK4
        // A position from before the window existed (setPosition, a remembered placement) on a
        // screen that is still there; else centred.
        if (hasPosition_ && IsX11() && !OnSomeScreen(x_, y_)) {
            hasPosition_ = false;
            center_ = true;
        }
        if (hasPosition_ && IsX11()) {
            gtk_window_move(window, x_, y_);
        } else if (center_) {
            gtk_window_set_position(window, GTK_WIN_POS_CENTER);
        }
        if (onTop_) gtk_window_set_keep_above(window, TRUE);
        g_signal_connect(window, "configure-event", G_CALLBACK(+[](GtkWidget*, GdkEventConfigure*, gpointer self) -> gboolean {
                             static_cast<GtkPlacement*>(self)->Changed();
                             return FALSE;
                         }), this);
        g_signal_connect(window, "window-state-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventWindowState* event, gpointer self) -> gboolean {
                             static_cast<GtkPlacement*>(self)->StateEvent(event);
                             return FALSE;
                         }), this);
#else
        auto notify = +[](GObject*, GParamSpec*, gpointer self) { static_cast<GtkPlacement*>(self)->Tell(); };
        g_signal_connect(window, "notify::default-width", G_CALLBACK(notify), this);
        g_signal_connect(window, "notify::default-height", G_CALLBACK(notify), this);
        g_signal_connect(window, "notify::maximized", G_CALLBACK(notify), this);
        g_signal_connect(window, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer self) {
                             GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
                             if (!surface) return;
                             g_signal_connect(surface, "notify::state", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                                                  static_cast<GtkPlacement*>(data)->SurfaceState();
                                              }), self);
                         }), this);
#endif
    }

    // The window is going: no more events from it, nothing waiting.
    void Detach() {
        if (remapIdle_) g_source_remove(remapIdle_);
        remapIdle_ = 0;
        remapping_ = false;
        if (window_) {
            g_signal_handlers_disconnect_by_data(window_, this);
#ifdef UNIAPI_GTK4
            if (GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_))) {
                g_signal_handlers_disconnect_by_data(surface, this);
            }
#endif
        }
        window_ = nullptr;
        view_ = nullptr;
    }

    // Shows a window made hidden (startHidden, hide()): unminimised, in front with focus if
    // `activate`, else shown where it was; a state asked for while hidden, taken.
    void Show(bool activate) {
        if (!window_) return;
#ifdef UNIAPI_GTK4
        Unminimize();
#else
        gtk_window_deiconify(window_);
#endif
        if (activate) {
            gtk_window_present(window_);
        } else {
            gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
        }
        if (statePending_) {  // minimize(), maximize() while it was hidden
            statePending_ = false;
            ApplyState();
        }
    }

    void Hide() {
        if (!window_) return;
#ifdef UNIAPI_GTK4
        if (remapping_) {  // hidden before its new surface was laid out: nothing to wait for
            if (GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_))) {
                g_signal_handlers_disconnect_by_func(surface, reinterpret_cast<gpointer>(&GtkPlacement::OnRemappedLayout), this);
            }
            remapping_ = false;
        }
#endif
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }

    Size CurrentSize(int width, int height) const {
        Size size{width, height};
        if (!view_) return size;
#ifdef UNIAPI_GTK4
        const int w = gtk_widget_get_width(GTK_WIDGET(view_)), h = gtk_widget_get_height(GTK_WIDGET(view_));
#else
        const int w = gtk_widget_get_allocated_width(GTK_WIDGET(view_)), h = gtk_widget_get_allocated_height(GTK_WIDGET(view_));
#endif
        if (w > 1 && h > 1) size = {w, h};  // allocated (shown)
        return size;
    }

    std::optional<Point> Position() const {
#ifndef UNIAPI_GTK4
        if (window_ && IsX11() && gtk_widget_get_realized(GTK_WIDGET(window_))) {
            Point point;
            gtk_window_get_position(window_, &point.x, &point.y);
            return point;
        }
        if (!window_ && hasPosition_) return Point{x_, y_};
#endif
        return std::nullopt;
    }

    bool SetPosition(int x, int y) {
#ifndef UNIAPI_GTK4
        hasPosition_ = true;
        center_ = false;
        x_ = x;
        y_ = y;
        if (!window_) return true;  // where it opens, on X11
        if (!IsX11()) return false;
        gtk_window_move(window_, x, y);
        return true;
#else
        (void)x;
        (void)y;
        return false;
#endif
    }

    void Center() {
#ifndef UNIAPI_GTK4
        hasPosition_ = false;
        center_ = true;
        if (!window_ || !IsX11()) return;
        GdkWindow* surface = gtk_widget_get_window(GTK_WIDGET(window_));
        if (!surface) return;
        GdkMonitor* monitor = gdk_display_get_monitor_at_window(gdk_window_get_display(surface), surface);
        if (!monitor) return;
        GdkRectangle area;
        gdk_monitor_get_workarea(monitor, &area);
        int width = 0, height = 0;
        gtk_window_get_size(window_, &width, &height);
        gtk_window_move(window_, area.x + (area.width - width) / 2, area.y + (area.height - height) / 2);
#endif
    }

    // The desktop's screens (GDK's monitors), the one the window is on marked current. GTK 4:
    // no primary monitor; GTK 3: whole scales only. None before GTK has started (run()).
    std::vector<Screen> Screens() const {
        std::vector<Screen> screens;
        GdkDisplay* display = gtkStarted ? gdk_display_get_default() : nullptr;
        if (!display) return screens;
        GdkMonitor* current = nullptr;
#ifdef UNIAPI_GTK4
        if (window_ && gtk_widget_get_realized(GTK_WIDGET(window_))) {
            if (GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_))) {
                current = gdk_display_get_monitor_at_surface(display, surface);
            }
        }
        GListModel* monitors = gdk_display_get_monitors(display);
        const guint total = g_list_model_get_n_items(monitors);
        for (guint i = 0; i < total; ++i) {
            auto* monitor = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
            if (!monitor) continue;
            GdkRectangle area;
            gdk_monitor_get_geometry(monitor, &area);
            Screen screen;
            screen.x = area.x;
            screen.y = area.y;
            screen.width = area.width;
            screen.height = area.height;
#if GTK_CHECK_VERSION(4, 14, 0)
            screen.scale = gdk_monitor_get_scale(monitor);  // fractional scales too
#else
            screen.scale = gdk_monitor_get_scale_factor(monitor);
#endif
            screen.primary = false;  // GTK 4 has no primary monitor
            screen.current = monitor == current;
            screens.push_back(screen);
            g_object_unref(monitor);
        }
#else
        if (window_) {
            if (GdkWindow* surface = gtk_widget_get_window(GTK_WIDGET(window_))) {
                current = gdk_display_get_monitor_at_window(display, surface);
            }
        }
        const int total = gdk_display_get_n_monitors(display);
        for (int i = 0; i < total; ++i) {
            GdkMonitor* monitor = gdk_display_get_monitor(display, i);
            if (!monitor) continue;
            GdkRectangle area;
            gdk_monitor_get_geometry(monitor, &area);
            Screen screen;
            screen.x = area.x;
            screen.y = area.y;
            screen.width = area.width;
            screen.height = area.height;
            screen.scale = gdk_monitor_get_scale_factor(monitor);
            screen.primary = gdk_monitor_is_primary(monitor);
            screen.current = monitor == current;
            screens.push_back(screen);
        }
#endif
        return screens;
    }

    // The monitors' work areas, where windows can be placed: GTK 3 on X11.
    static std::vector<Rect> ScreenAreas() {
        std::vector<Rect> areas;
#ifndef UNIAPI_GTK4
        GdkDisplay* display = gtkStarted ? gdk_display_get_default() : nullptr;
        if (!display || !IsX11()) return areas;
        const int monitors = gdk_display_get_n_monitors(display);
        for (int i = 0; i < monitors; ++i) {
            GdkMonitor* monitor = gdk_display_get_monitor(display, i);
            if (!monitor) continue;
            GdkRectangle area;
            gdk_monitor_get_workarea(monitor, &area);
            if (area.width > 0 && area.height > 0) areas.push_back({area.x, area.y, area.width, area.height});
        }
#endif
        return areas;
    }

    WindowState State() const {
        if (!window_ || (hidden && *hidden)) return statePending_ ? state_ : WindowState::Normal;
#ifdef UNIAPI_GTK4
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
        if (surface && GDK_IS_TOPLEVEL(surface) && (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_MINIMIZED)) {
            return WindowState::Minimized;
        }
        if (minAsked_ && minSeen_) return WindowState::Minimized;  // Wayland
        return gtk_window_is_maximized(window_) ? WindowState::Maximized : WindowState::Normal;
#else
        GdkWindow* surface = gtk_widget_get_window(GTK_WIDGET(window_));
        if (!surface) return WindowState::Normal;
        const GdkWindowState state = gdk_window_get_state(surface);
        if (state & GDK_WINDOW_STATE_ICONIFIED) return WindowState::Minimized;
        return (state & GDK_WINDOW_STATE_MAXIMIZED) ? WindowState::Maximized : WindowState::Normal;
#endif
    }

    // On a window shown; a hidden one (or one not yet made) takes it when shown.
    void SetState(WindowState state) {
        state_ = state;
        if (!window_ || (hidden && *hidden) || remapping_) {
            statePending_ = true;
            return;
        }
        statePending_ = false;
        ApplyState();
    }

    // A state asked for before the window was shown, for a window shown now (made, not hidden).
    void ApplyPending() {
        if (!window_ || (hidden && *hidden) || !statePending_) return;
        statePending_ = false;
        ApplyState();
    }

    Bounds NormalBounds(int width, int height) const {
        Bounds bounds;
#ifdef UNIAPI_GTK4
        // GTK 4 keeps the size it had before being maximised as the default size.
        if (window_) {
            gtk_window_get_default_size(window_, &bounds.width, &bounds.height);
            if (bounds.width > 0 && bounds.height > 0) return bounds;
        }
        const Size size = CurrentSize(width, height);
        bounds.width = size.width;
        bounds.height = size.height;
#else
        if (hasNormal_) {
            bounds = {normal_.x, normal_.y, normal_.width, normal_.height, IsX11()};
            return bounds;
        }
        const Size size = CurrentSize(width, height);
        bounds.width = size.width;
        bounds.height = size.height;
        if (const std::optional<Point> point = Position()) {
            bounds.x = point->x;
            bounds.y = point->y;
            bounds.hasPosition = true;
        }
#endif
        return bounds;
    }

    bool SetAlwaysOnTop(bool on) {
#ifndef UNIAPI_GTK4
        onTop_ = on;
        if (!window_) return true;
        gtk_window_set_keep_above(window_, on ? TRUE : FALSE);
        return IsX11();  // Wayland has no such thing
#else
        (void)on;
        return false;
#endif
    }

    bool IsAlwaysOnTop() const {
#ifndef UNIAPI_GTK4
        return onTop_ && (!window_ || IsX11());
#else
        return false;
#endif
    }

private:
    void Tell() {
        if (window_ && changed) changed();
    }

#ifndef UNIAPI_GTK4
    static bool OnSomeScreen(int x, int y) {
        constexpr int kGrab = 48;  // as the core's FitPlacement: the title bar's top-left part
        for (const Rect& s : ScreenAreas()) {
            if (x + kGrab > s.x && x < s.x + s.width - kGrab && y >= s.y && y < s.y + s.height - kGrab) return true;
        }
        return false;
    }

    // Moved or resized (configure), minimised or maximised (window-state). The size and place
    // while normal are kept for NormalBounds.
    void Changed() {
        GdkWindow* surface = window_ ? gtk_widget_get_window(GTK_WIDGET(window_)) : nullptr;
        if (surface && !(gdk_window_get_state(surface) &
                         (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN))) {
            Rect now{normal_.x, normal_.y, 0, 0};
            gtk_window_get_size(window_, &now.width, &now.height);
            if (IsX11()) gtk_window_get_position(window_, &now.x, &now.y);
            if (!hasNormal_ || now.width != normal_.width || now.height != normal_.height || now.x != normal_.x ||
                now.y != normal_.y) {
                previous_ = normal_;
                hasPrevious_ = hasNormal_;
                normal_ = now;
                hasNormal_ = true;
            }
        }
        Tell();
    }

    void StateEvent(GdkEventWindowState* event) {
        // Some window managers resize the window first and say it is maximised after: the size
        // kept as normal is then the maximised one. What was kept before it is the normal one.
        const int large = GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN;
        if ((event->changed_mask & large) && (event->new_window_state & event->changed_mask & large) && hasPrevious_ &&
            window_) {
            int width = 0, height = 0;
            gtk_window_get_size(window_, &width, &height);
            if (width == normal_.width && height == normal_.height) {
                normal_ = previous_;
                hasPrevious_ = false;
            }
        }
        Changed();
    }
#else
    // Wayland doesn't tell an app that its window is minimised, but a desktop that stops showing
    // one suspends its surface. Suspended after minimize(): minimised. Shown again after that:
    // the person brought it back (the taskbar), it no longer is.
    bool SurfaceSuspended() const {
#if GTK_CHECK_VERSION(4, 12, 0)
        GdkSurface* surface = window_ ? gtk_native_get_surface(GTK_NATIVE(window_)) : nullptr;
        return surface && GDK_IS_TOPLEVEL(surface) &&
               (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_SUSPENDED) != 0;
#else
        return false;
#endif
    }

    void SurfaceState() {
        if (minAsked_) {
            if (SurfaceSuspended()) {
                minSeen_ = true;
            } else if (minSeen_) {
                minAsked_ = minSeen_ = false;
            }
        }
        Tell();
    }

    // Not minimised any more. Wayland has no request for it: a window asked to minimise is taken
    // off the screen and put back, which gives it a new surface, one that isn't minimised. A
    // state asked for before the desktop has laid that surface out would lose the normal size:
    // it waits (remapping) until then.
    void Unminimize() {
        gtk_window_unminimize(window_);  // first: a window shown opens as it was last asked to be
        if (minAsked_ && !IsX11() && gtk_widget_get_visible(GTK_WIDGET(window_))) {
            gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
            gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
            GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
            if (surface && !remapping_) {
                remapping_ = true;
                g_signal_connect(surface, "layout", G_CALLBACK(&GtkPlacement::OnRemappedLayout), this);
            }
        }
        minAsked_ = minSeen_ = false;
    }

    static void OnRemappedLayout(GdkSurface* surface, int, int, gpointer data) {
        auto* self = static_cast<GtkPlacement*>(data);
        g_signal_handlers_disconnect_by_func(surface, reinterpret_cast<gpointer>(&GtkPlacement::OnRemappedLayout), data);
        if (!self->remapIdle_) {
            self->remapIdle_ = g_idle_add(+[](gpointer me) -> gboolean {
                auto* placement = static_cast<GtkPlacement*>(me);
                placement->remapIdle_ = 0;
                placement->remapping_ = false;
                placement->ApplyPending();
                return G_SOURCE_REMOVE;
            }, self);
        }
    }
#endif

    void ApplyState() {
        if (!window_) return;
        switch (state_) {
        case WindowState::Minimized:
#ifdef UNIAPI_GTK4
            gtk_window_minimize(window_);
            if (!IsX11()) {
                minAsked_ = true;
                minSeen_ = SurfaceSuspended();
            }
#else
            gtk_window_iconify(window_);
#endif
            break;
        case WindowState::Maximized:
#ifdef UNIAPI_GTK4
            Unminimize();
            if (remapping_) {
                statePending_ = true;
                break;
            }
#endif
            gtk_window_maximize(window_);
            break;
        case WindowState::Normal:
#ifdef UNIAPI_GTK4
            Unminimize();
            if (remapping_) {
                statePending_ = true;
                break;
            }
#else
            gtk_window_deiconify(window_);
#endif
            gtk_window_unmaximize(window_);
            break;
        }
    }

    GtkWindow* window_ = nullptr;
    WebKitWebView* view_ = nullptr;
    bool hasPosition_ = false;  // setPosition before the window existed: where it opens
    int x_ = 0, y_ = 0;
    bool center_ = false;
    WindowState state_ = WindowState::Normal;  // the state asked for last
    bool statePending_ = false;                // asked for while hidden: taken when shown
    bool onTop_ = false;
    bool remapping_ = false;  // GTK 4 on Wayland: put back on screen, until its surface is laid out
    guint remapIdle_ = 0;
    bool minAsked_ = false, minSeen_ = false;  // GTK 4 on Wayland: minimize() asked, then seen
    Rect normal_, previous_;                   // GTK 3: the size and place while normal, and before
    bool hasNormal_ = false, hasPrevious_ = false;
};

// --- Where the page may go, and files dropped on it ---
// The app's pages load; files never; another site opens outside the app (the core decides): a
// link or window.open to it (a new window: never one of the page's own), a form sent there, or a
// script going there because of the person (a click). WebKitGTK doesn't say which frame
// navigates, so what isn't the page's doing (an iframe loading another site) is left to load.
gboolean DecidePolicy(const std::string& origin, const BackendEvents& events, WebKitPolicyDecision* decision,
                      WebKitPolicyDecisionType type) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        return FALSE;  // responses: WebKit's way
    }
    WebKitNavigationAction* action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    const std::string target = uri ? uri : "";
    const Destination where = DestinationOf(origin, target);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        if (where == Destination::App) return FALSE;
        if (where == Destination::Outside) {
            const WebKitNavigationType kind = webkit_navigation_action_get_navigation_type(action);
            if (kind != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED && kind != WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED &&
                !webkit_navigation_action_is_user_gesture(action)) {
                return FALSE;
            }
        }
    }
    webkit_policy_decision_ignore(decision);
    if (where == Destination::Outside && events.onExternalNavigation) events.onExternalNavigation(target);
    return TRUE;
}

void ReportDrop(const BackendEvents& events, std::vector<std::string> paths, int x, int y) {
    if (!paths.empty() && events.onFileDrop) events.onFileDrop(paths, x, y);
}

// The navigation policy and file drops for a window's view. Host: Origin(), Events(), and (GTK 3)
// a `dropping` flag.
template <typename Host>
void ConnectDesktop(WebKitWebView* view, Host* host) {
    g_signal_connect(view, "decide-policy",
                     G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type,
                                    gpointer self) -> gboolean {
                         auto* h = static_cast<Host*>(self);
                         return DecidePolicy(h->Origin(), h->Events(), decision, type);
                     }),
                     host);
#ifdef UNIAPI_GTK4
    // A drop target for files of the view's own, before WebKit's (capture phase): it takes file
    // drops, so the page gets no drop event for them; never a navigation to the file.
    GtkDropTarget* drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
    g_signal_connect(drop, "drop",
                     G_CALLBACK(+[](GtkDropTarget*, const GValue* value, double x, double y, gpointer self) -> gboolean {
                         if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
                         std::vector<std::string> paths;
                         GSList* files = gdk_file_list_get_files(static_cast<GdkFileList*>(g_value_get_boxed(value)));
                         for (GSList* item = files; item; item = item->next) {
                             if (char* path = g_file_get_path(G_FILE(item->data))) {
                                 paths.emplace_back(path);
                                 g_free(path);
                             }
                         }
                         g_slist_free(files);
                         ReportDrop(static_cast<Host*>(self)->Events(), std::move(paths), int(x), int(y));
                         return TRUE;
                     }),
                     host);
    gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(drop));
#else
    // WebKit takes the drop as usual (the page's drop event); its data, when it is files, is read
    // on the way. drag-drop marks the drop: data asked for while dragging isn't one.
    g_signal_connect(view, "drag-drop",
                     G_CALLBACK(+[](GtkWidget*, GdkDragContext*, gint, gint, guint, gpointer self) -> gboolean {
                         static_cast<Host*>(self)->dropping = true;
                         return FALSE;  // WebKit's handler goes on
                     }),
                     host);
    g_signal_connect(view, "drag-data-received",
                     G_CALLBACK(+[](GtkWidget*, GdkDragContext*, gint x, gint y, GtkSelectionData* selection, guint,
                                    guint, gpointer self) {
                         auto* h = static_cast<Host*>(self);
                         if (!h->dropping) return;
                         gchar** uris = gtk_selection_data_get_uris(selection);
                         if (!uris) return;  // another of the drop's formats: the files may come next
                         h->dropping = false;
                         std::vector<std::string> paths;
                         for (gchar** uri = uris; *uri; ++uri) {
                             if (char* path = g_filename_from_uri(*uri, nullptr, nullptr)) {
                                 paths.emplace_back(path);
                                 g_free(path);
                             }
                         }
                         g_strfreev(uris);
                         ReportDrop(h->Events(), std::move(paths), x, y);
                     }),
                     host);
#endif
}

// Calls host->PageDrawn() once the page has drawn a frame after its load event. WebKit's
// WEBKIT_LOAD_FINISHED can come long before that event: a large page's module scripts ran
// for 0.7 s after it, so two frames counted from it ended just after the scripts, before the
// page they had built was drawn, and the window showed the page from before them. And the load
// event comes before its first full frame (styles, layout and paint follow). So, in uniapi's
// own script world: the page's load event, then two animation frames; the second runs once
// the frame after the load has been drawn. Cancelled with `cancellable` (the window gone); a
// page that navigated on meanwhile counts as drawn.
template <typename Host>
void AfterPageDrawn(WebKitWebView* view, GCancellable* cancellable, Host* host) {
    static const char kBody[] =
        "return new Promise(r => {"
        "  const frames = () => requestAnimationFrame(() => requestAnimationFrame(() => r(true)));"
        "  if (document.readyState === 'complete') frames(); else addEventListener('load', frames, { once: true });"
        "});";
    webkit_web_view_call_async_javascript_function(
        view, kBody, -1, nullptr, kWorld, nullptr, cancellable,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            GError* error = nullptr;
            JSCValue* value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(source), result, &error);
            if (value) g_object_unref(value);
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free(error);  // the window is gone; `data` must not be used
                return;
            }
            if (error) {
                g_debug("waiting for the page's first frame: %s", error->message);
                g_error_free(error);
            }
            static_cast<Host*>(data)->PageDrawn();
        },
        host);
}

// --- Notifications: the desktop's notification server over D-Bus (GDBus, part of GLib) ---
constexpr char kNotificationsName[] = "org.freedesktop.Notifications";
constexpr char kNotificationsPath[] = "/org/freedesktop/Notifications";

class LinuxBackend;
LinuxBackend* gNotifying = nullptr;  // the backend the server's signals go to (UI thread only)

class LinuxBackend final : public Backend, public ViewHost {
public:
    LinuxBackend(const AppOptions& options, BackendEvents events)
        : options_(options), events_(std::move(events)),
          origin_(std::string(kScheme) + "://" + options.host), uiThread_(std::this_thread::get_id()) {}

    ~LinuxBackend() override {
        if (gNotifying == this) gNotifying = nullptr;
        for (auto& entry : timers_) g_source_remove(entry.second.source);
        if (postSource_) g_source_remove(postSource_);
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        if (revealTimer_) g_source_remove(revealTimer_);
        // A handshake reply still on its way must not reach this object: cancelled, its
        // callback returns without touching it.
        g_cancellable_cancel(cancellable_);
        g_object_unref(cancellable_);
        if (portal_) {
            g_signal_handlers_disconnect_by_data(portal_, this);
            g_object_unref(portal_);
        }
#ifndef UNIAPI_GTK4
        if (icon_) g_object_unref(icon_);
#else
        if (session_) g_object_unref(session_);
#endif
        if (context_) g_object_unref(context_);
    }

    int Run() override {
        // Shared memory: find the extension, and set GIGACAGE_ENABLED before GTK starts its
        // helper threads (setenv isn't safe while another thread reads the environment). If a
        // dialog or alert() started GTK already, the environment is left alone and streams
        // use the fetch fallback for this run.
        std::string extensionDir;
        if (options_.sharedMemory) {
            extensionDir = ExtensionDirectory();
            const char* gigacage = getenv("GIGACAGE_ENABLED");
            const bool alreadyOff = gigacage && std::strcmp(gigacage, "no") == 0;
            if (extensionDir.empty()) {
                g_debug("shared memory off: %s not found", kExtensionFile);
            } else if (!alreadyOff && gtkStarted) {
                g_debug("shared memory off: GTK was started before run() (a dialog or alert), too late "
                        "to set GIGACAGE_ENABLED safely");
                extensionDir.clear();
            } else if (!alreadyOff) {
                setenv("GIGACAGE_ENABLED", "no", 1);  // inherited by the web process; see the top
            }
        }

        // AppOptions::dmabufRenderer = false: WebKit's DMA-BUF renderer off, read by WebKit when
        // GTK and the web processes start (setenv is safe only before GTK's threads exist, as above).
        if (!options_.dmabufRenderer) {
            if (gtkStarted) {
                g_debug("dmabufRenderer = false ignored: GTK was started before run() (a dialog or alert)");
            } else {
                setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
            }
        }
        if (!InitGtk()) {
            g_printerr("uniapi: cannot open a display (GTK failed to initialize)\n");
            return 1;
        }

#ifdef UNIAPI_GTK4
        GtkWidget* window = gtk_window_new();
#else
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (!options_.activate) gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
#endif
        window_ = GTK_WINDOW(window);
        ApplyTitleBar();
        gtk_window_set_title(window_, options_.title.c_str());
        gtk_window_set_default_size(window_, options_.width, options_.height);  // logical pixels
        gtk_window_set_resizable(window_, options_.resizable);
        ApplyHints();
        if (options_.fullscreen) {
            gtk_window_fullscreen(window_);
            fullscreen_ = true;
        }
#ifndef UNIAPI_GTK4
        if (icon_) gtk_window_set_icon(window_, icon_);
#endif
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer self) {
                             static_cast<LinuxBackend*>(self)->OnDestroyed();
                         }), this);
        ConnectCloseRequest(window, this);
        placement_.hidden = &hidden_;
        placement_.changed = [this] {
            if (events_.onPlacementChanged) events_.onPlacementChanged();
        };

        // Pages and stream data come from the app through a custom scheme. Secure: the page is
        // a secure context like on https. CORS-enabled: fetch() works on it.
        WebKitWebContext* context = webkit_web_context_get_default();
        // AppOptions::dataDirectory: the profile goes there instead of WebKit's XDG folders.
        const std::string dataDirectory = options_.dataDirectory;
        const std::string cacheDirectory = dataDirectory + "/cache";
#ifdef UNIAPI_GTK4
        WebKitNetworkSession* session =
            dataDirectory.empty() ? nullptr
                                  : webkit_network_session_new(dataDirectory.c_str(), cacheDirectory.c_str());
#else
        bool ownContext = false;
        if (!dataDirectory.empty()) {
            WebKitWebsiteDataManager* data = webkit_website_data_manager_new(
                "base-data-directory", dataDirectory.c_str(), "base-cache-directory", cacheDirectory.c_str(),
                nullptr);
            context = webkit_web_context_new_with_website_data_manager(data);
            g_object_unref(data);
            ownContext = true;
        }
#endif
        // The handler belongs to the context, and the default context lives as long as the
        // process and is shared by every App in it: register once per context, and find the
        // backend through the request's web view, so no context holds a pointer to a backend
        // that has gone.
        if (!g_object_get_data(G_OBJECT(context), kSchemeRegisteredKey)) {
            webkit_web_context_register_uri_scheme(context, kScheme, &LinuxBackend::OnSchemeRequestOfView,
                                                   nullptr, nullptr);
            g_object_set_data(G_OBJECT(context), kSchemeRegisteredKey, GINT_TO_POINTER(1));
        }
        WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
        webkit_security_manager_register_uri_scheme_as_secure(security, kScheme);
        webkit_security_manager_register_uri_scheme_as_cors_enabled(security, kScheme);

        // Shared memory: point WebKitGTK at the extension before the web process starts.
        const bool tryShared = !extensionDir.empty();
        tryShared_ = tryShared;
        if (tryShared) {
#ifdef UNIAPI_GTK4
            webkit_web_context_set_web_process_extensions_directory(context, extensionDir.c_str());
            // The web process runs in a sandbox, which only sees paths it is given.
            webkit_web_context_add_path_to_sandbox(context, extensionDir.c_str(), TRUE);
#else
            webkit_web_context_set_web_extensions_directory(context, extensionDir.c_str());
#endif
        }

        context_ = WEBKIT_WEB_CONTEXT(g_object_ref(context));  // for further windows' views
#ifdef UNIAPI_GTK4
        GtkWidget* view =
            session ? GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "network-session",
                                              session, nullptr))
                    : GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, nullptr));
        session_ = session;  // the view has its own reference; this one is for further windows' views
#else
        GtkWidget* view = webkit_web_view_new_with_context(context);
        if (ownContext) g_object_unref(context);  // the view keeps it
#endif
        webview_ = WEBKIT_WEB_VIEW(view);
        if (options_.devTools) AllowDevTools(webview_);
        if (!options_.contextMenu) LimitContextMenu(webview_);
        ApplyBackground(GTK_WIDGET(window_), webview_, ColorValue(options_.backgroundColor));
        g_object_set_data(G_OBJECT(view), kBackendKey, static_cast<ViewHost*>(this));  // for OnSchemeRequestOfView; dies with the view
        placement_.Attach(window_, webview_);

        WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(webview_);
#ifdef UNIAPI_GTK4
        g_signal_connect(manager, "script-message-received::uniapi",
                         G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer self) {
                             static_cast<LinuxBackend*>(self)->OnScriptMessage(value);
                         }),
                         this);
        webkit_user_content_manager_register_script_message_handler(manager, "uniapi", kWorld);
#else
        g_signal_connect(manager, "script-message-received::uniapi",
                         G_CALLBACK(+[](WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer self) {
                             static_cast<LinuxBackend*>(self)->OnScriptMessage(
                                 webkit_javascript_result_get_js_value(result));
                         }),
                         this);
        webkit_user_content_manager_register_script_message_handler_in_world(manager, "uniapi", kWorld);
#endif
        AddBridge(manager);

        g_signal_connect(view, "load-changed",
                         G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer self) {
                             auto* backend = static_cast<LinuxBackend*>(self);
                             if (!backend->readyReported_) {  // the handshake's blank page
                                 if (event == WEBKIT_LOAD_FINISHED) backend->SendHandshake();
                             } else if (event == WEBKIT_LOAD_STARTED && backend->events_.onNavigationStarting) {
                                 backend->events_.onNavigationStarting();
                             } else if (event == WEBKIT_LOAD_FINISHED) {
                                 backend->PageLoaded();
                             }
                         }),
                         this);
        ConnectDesktop(webview_, this);

        // Never an empty or white window, or a page half built: the page stays hidden until it
        // is drawn (Reveal), as Windows cloaks the window. See HidePage.
        HidePage(window, view);
        invisible_ = true;
        StartRevealTimer(kRevealStartTimeoutMs);  // until its page has loaded (PageLoaded)
        // Started hidden (or hidden before run()): made, its page loading, but shown only by
        // SetVisible.
        everShown_ = !hidden_;
#ifdef UNIAPI_GTK4
        gtk_window_set_child(window_, view);
        // present() asks for focus; just showing it leaves focus with the app you're in.
        if (hidden_) {
            g_debug("the main window opens hidden");
        } else if (options_.activate) {
            gtk_window_present(window_);
        } else {
            gtk_widget_set_visible(window, TRUE);
        }
#else
        gtk_container_add(GTK_CONTAINER(window), view);
        if (hidden_) {
            gtk_widget_show(view);  // the window shown later shows it
            g_debug("the main window opens hidden");
        } else {
            gtk_widget_show_all(window);
        }
#endif
        placement_.ApplyPending();  // minimize(), maximize() before it existed
        gtk_widget_grab_focus(view);  // keyboard shortcuts work without a click
        // Minimum size of the page's area (the view), like width/height.
        if (options_.minWidth > 0 || options_.minHeight > 0) {
            gtk_widget_set_size_request(view, options_.minWidth > 0 ? options_.minWidth : -1,
                                        options_.minHeight > 0 ? options_.minHeight : -1);
        }

        CreateWindows();  // the further windows opened before run()

        if (tryShared) {
            // The extension loads with the web process, which starts with the first page. A
            // blank page starts it; the handshake follows when that page has loaded.
            webkit_web_view_load_uri(webview_, "about:blank");
            StartHandshakeTimer(kWebProcessStartTimeoutMs);
        } else {
            ReportReady(false);
        }
        if (!window_) return 0;  // closed from onReady
        loop_ = g_main_loop_new(nullptr, FALSE);
        g_main_loop_run(loop_);
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        return 0;
    }

    void Quit() override {
        if (!window_) return;
        closeAsked_ = true;  // closes without asking (AskClose)
        if (!gtk_widget_get_realized(GTK_WIDGET(window_))) {
            // Never shown (startHidden): gtk_window_close does nothing to a window that isn't
            // realized, and run() would never end.
#ifdef UNIAPI_GTK4
            gtk_window_destroy(window_);  // "destroy": OnDestroyed
#else
            gtk_widget_destroy(GTK_WIDGET(window_));
#endif
            return;
        }
        gtk_window_close(window_);
    }

    void Navigate(const std::string& path) override {
        if (webview_) webkit_web_view_load_uri(webview_, (origin_ + path).c_str());
    }

    void PostToPage(const std::string& text) override { PostScript(webview_, text); }

    std::unique_ptr<SharedRegion> CreateSharedRegion(size_t bytes) override {
        if (!sharedMemory_ || bytes == 0) return nullptr;
        auto region = std::make_unique<LinuxRegion>();
        region->fd = memfd_create("uniapi-stream", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (region->fd < 0 || ftruncate(region->fd, off_t(bytes)) != 0) return nullptr;
        // The size can't change any more, so the web process can't be made to fault on it.
        fcntl(region->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
        void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd, 0);
        if (data == MAP_FAILED) return nullptr;
        region->data = static_cast<uint8_t*>(data);
        region->size = bytes;
        return region;
    }

    void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) override {
        SendRegion(webview_, region, infoJson, writable);
    }

    void Post(std::function<void()> fn) override {
        std::lock_guard<std::mutex> lock(postMutex_);
        posted_.push_back(std::move(fn));
        // One idle source drains the queue, so posted functions run in order.
        if (!postSource_) {
            postSource_ = g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer self) -> gboolean {
                static_cast<LinuxBackend*>(self)->RunPosted();
                return G_SOURCE_REMOVE;
            }, this, nullptr);
        }
    }

    bool OnUiThread() const override { return std::this_thread::get_id() == uiThread_; }

    void SetTitle(const std::string& title) override {
        options_.title = title;
        if (window_) gtk_window_set_title(window_, title.c_str());
    }

    void SetSize(int width, int height) override {
        options_.width = width;  // also the shape keepAspectRatio keeps
        options_.height = height;
        if (!window_ || fullscreen_) return;  // before Run(): the window opens at this size
        if (gtk_window_is_maximized(window_)) gtk_window_unmaximize(window_);
#ifdef UNIAPI_GTK4
        gtk_window_set_default_size(window_, width, height);  // GTK 4 resizes a shown window to it
#else
        ApplyHints();
        gtk_window_resize(window_, width, height);
#endif
    }

    // keepAspectRatio and the maximum size (ApplyGeometryHints).
    void ApplyHints() { ApplyGeometryHints(window_, options_); }

    void SetFullscreen(bool on) override {
        if (!window_) {
            options_.fullscreen = on;  // applied when the window opens
            return;
        }
        if (on) {
            gtk_window_fullscreen(window_);
        } else {
            gtk_window_unfullscreen(window_);
        }
        fullscreen_ = on;  // as requested; the window manager applies it a moment later
    }

    bool IsFullscreen() const override { return window_ ? fullscreen_ : options_.fullscreen; }

    void SetTitleBar(TitleBar mode) override {
        options_.titleBar = mode;
        ApplyTitleBar();  // before Run(): when the window opens
    }

    TitleBar GetTitleBar() const override { return options_.titleBar; }

    // The window kept invisible until its page is drawn (AppOptions::showWhenReady) appears.
    void Show() override { Reveal(); }

    void SetVisible(bool visible) override {
        hidden_ = !visible;
        if (!window_) return;  // before Run(): how it opens
        if (!visible) {
            placement_.Hide();
            return;
        }
        if (!everShown_) {
            everShown_ = true;
            // A page still hidden waits for its frame (or app.show()) as usual.
            if (!invisible_) HideUntilDrawn();
        }
        placement_.Show(options_.activate);
    }

    // Placement (GtkPlacement).
    Size CurrentSize() const override { return placement_.CurrentSize(options_.width, options_.height); }
    std::optional<Point> Position() const override { return placement_.Position(); }
    bool SetPosition(int x, int y) override { return placement_.SetPosition(x, y); }
    void Center() override { placement_.Center(); }
    WindowState State() const override { return placement_.State(); }
    void SetState(WindowState state) override { placement_.SetState(state); }
    Bounds NormalBounds() const override { return placement_.NormalBounds(options_.width, options_.height); }
    bool SetAlwaysOnTop(bool on) override { return placement_.SetAlwaysOnTop(on); }
    bool IsAlwaysOnTop() const override { return placement_.IsAlwaysOnTop(); }
    std::vector<Screen> Screens() const override { return placement_.Screens(); }
    std::vector<Rect> ScreenAreas() const override { return GtkPlacement::ScreenAreas(); }
    // The WebKitGTK loaded at run time, which may be newer than the one built against.
    WebEngine Engine() const override {
        WebEngine engine;
        engine.name = "WebKitGTK";
        engine.version = std::to_string(webkit_get_major_version()) + "." + std::to_string(webkit_get_minor_version())
                         + "." + std::to_string(webkit_get_micro_version());
#ifdef UNIAPI_GTK4
        engine.variant = "API 6.0, GTK 4";  // webkitgtk-6.0
#else
        engine.variant = "API 4.1, GTK 3";  // webkit2gtk-4.1
#endif
        return engine;
    }

    bool IsVisible() const override { return !hidden_; }

    // The close button, Alt+F4, the window manager's Close: the core decides. Quit doesn't ask.
    bool AskClose() {
        if (closeAsked_ || !window_) return true;
        return !events_.onCloseRequested || events_.onCloseRequested(CloseReason::Window);
    }

    // Hides the page until it has drawn again, as when the window opened: for a window started
    // hidden whose page was revealed while it was off screen (by the timeout, or app.show())
    // but hasn't been drawn there.
    void HideUntilDrawn() {
        HidePage(GTK_WIDGET(window_), GTK_WIDGET(webview_));
        invisible_ = true;
        StartRevealTimer(kRevealTimeoutMs);
        AfterPageDrawn(webview_, cancellable_, this);
    }

    // The page has loaded: once it has drawn, the window appears (unless the page shows it
    // itself: showWhenReady). The timeout counts from here.
    void PageLoaded() {
        if (!invisible_) return;
        StartRevealTimer(options_.showWhenReady ? kRevealTimeoutReadyMs : kRevealTimeoutMs);
        if (!options_.showWhenReady) AfterPageDrawn(webview_, cancellable_, this);
    }

    void PageDrawn() {
        if (invisible_) StartRevealTimer(kRevealAfterDrawMs);
    }

    void StartRevealTimer(guint ms) {
        if (revealTimer_) g_source_remove(revealTimer_);
        revealTimer_ = g_timeout_add(ms, +[](gpointer self) -> gboolean {
            auto* backend = static_cast<LinuxBackend*>(self);
            backend->revealTimer_ = 0;
            backend->Reveal();
            return G_SOURCE_REMOVE;
        }, this);
    }

    // Shows the page: once, after its first page has drawn (or the page said so), or the
    // timeout.
    void Reveal() {
        if (revealTimer_) {
            g_source_remove(revealTimer_);
            revealTimer_ = 0;
        }
        if (!invisible_) return;
        invisible_ = false;
        ShowPage(window_, webview_);
    }

    // The title bar where GTK draws it (client-side decorations: GTK 4, GNOME): Dark and Light
    // ask the GTK theme for its dark or light variant (gtk-application-prefer-dark-theme).
    // System takes the desktop's preference from the XDG desktop portal (GNOME 42+, KDE Plasma
    // 5.24+), following changes, else what the GTK settings said before. Where the window
    // manager draws the title bar (KWin on Plasma, for GTK 3), it follows the desktop's colours
    // itself, and Dark and Light don't reach it.
    void ApplyTitleBar() {
        if (!window_) return;
        GtkSettings* settings = gtk_settings_get_default();
        if (!settings) return;
        if (!systemPrefersDarkRead_) {
            gboolean prefers = FALSE;
            g_object_get(settings, "gtk-application-prefer-dark-theme", &prefers, nullptr);
            systemPrefersDark_ = prefers;
            systemPrefersDarkRead_ = true;
            WatchColorScheme();
        }
        const TitleBar mode = options_.titleBar;
        gboolean dark = mode == TitleBar::Dark;
        if (mode == TitleBar::System) {
            dark = colorScheme_ == 1 ? TRUE : colorScheme_ == 2 ? FALSE : systemPrefersDark_;
        }
        g_object_set(settings, "gtk-application-prefer-dark-theme", dark, nullptr);
    }

    // A portal value, unwrapped from the variants and one-element tuples around it (Read wraps
    // it once more than ReadOne), as a uint32, or `fallback`.
    static guint32 PortalUint(GVariant* value, guint32 fallback) {
        GVariant* current = g_variant_ref(value);
        for (int depth = 0; depth < 4; ++depth) {
            GVariant* inner = nullptr;
            if (g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT)) {
                inner = g_variant_get_variant(current);
            } else if (g_variant_is_container(current) && g_variant_n_children(current) == 1) {
                inner = g_variant_get_child_value(current, 0);
            }
            if (!inner) break;
            g_variant_unref(current);
            current = inner;
        }
        const guint32 result = g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(current) : fallback;
        g_variant_unref(current);
        return result;
    }

    // The desktop's light or dark preference (org.freedesktop.portal.Settings, namespace
    // "org.freedesktop.appearance", key "color-scheme": 0 none, 1 prefer dark, 2 prefer light),
    // read once and followed through SettingChanged. No portal (no D-Bus session, an old
    // desktop): colorScheme_ stays 0 and System keeps the GTK settings' preference.
    void WatchColorScheme() {
        GError* error = nullptr;
        portal_ = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                                "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                "org.freedesktop.portal.Settings", nullptr, &error);
        if (!portal_) {
            g_debug("no desktop portal for the colour scheme: %s", error ? error->message : "?");
            g_clear_error(&error);
            return;
        }
        GVariant* result = g_dbus_proxy_call_sync(portal_, "ReadOne",
                                                  g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
                                                  G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        if (!result) {  // portals before version 2 have only Read
            g_clear_error(&error);
            result = g_dbus_proxy_call_sync(portal_, "Read", g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
                                            G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        }
        if (result) {
            colorScheme_ = PortalUint(result, 0);
            g_variant_unref(result);
        } else {
            g_debug("the desktop portal has no colour scheme: %s", error ? error->message : "?");
            g_clear_error(&error);
        }
        g_signal_connect(portal_, "g-signal",
                         G_CALLBACK(+[](GDBusProxy*, gchar*, gchar* signal, GVariant* parameters, gpointer self) {
                             if (g_strcmp0(signal, "SettingChanged") != 0) return;
                             const gchar* space = nullptr;
                             const gchar* key = nullptr;
                             GVariant* value = nullptr;
                             g_variant_get(parameters, "(&s&sv)", &space, &key, &value);
                             if (g_strcmp0(space, "org.freedesktop.appearance") == 0 && g_strcmp0(key, "color-scheme") == 0) {
                                 auto* backend = static_cast<LinuxBackend*>(self);
                                 backend->colorScheme_ = PortalUint(value, 0);
                                 if (backend->options_.titleBar == TitleBar::System) backend->ApplyTitleBar();
                             }
                             if (value) g_variant_unref(value);
                         }),
                         this);
    }

    void OpenDevTools() override { OpenDevToolsOf(webview_, options_.devTools); }

#ifdef UNIAPI_GTK4
    // GTK 4 takes window icons only by name, from the icon theme (the app's .desktop file).
    bool SetIcon(const unsigned char*, size_t) override { return false; }
#else
    bool SetIcon(const unsigned char* png, size_t size) override {
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        if (!loader) return false;
        const bool wrote = gdk_pixbuf_loader_write(loader, png, size, nullptr);
        const bool closed = gdk_pixbuf_loader_close(loader, nullptr);  // always exactly once
        GdkPixbuf* pixbuf = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
        if (pixbuf) {
            if (icon_) g_object_unref(icon_);
            icon_ = GDK_PIXBUF(g_object_ref(pixbuf));
            // Shown on X11; on Wayland the compositor uses the app's .desktop file instead.
            if (window_) gtk_window_set_icon(window_, icon_);
        }
        g_object_unref(loader);
        return pixbuf != nullptr;
    }
#endif

    int AddTimer(std::chrono::milliseconds interval, std::function<void()> fn) override {
        int id = nextTimer_++;
        Timer& timer = timers_[id];
        timer.fn = std::make_shared<std::function<void()>>(std::move(fn));
        timer.interval = guint(std::max<int64_t>(1, interval.count()));
        // Like on Windows, the first tick comes right away, then one per interval.
        timer.source = AddTimerSource(id, 0);
        return id;
    }

    void RemoveTimer(int id) override {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        g_source_remove(it->second.source);  // fine from inside its own callback
        timers_.erase(it);
    }

    // A file filter from "*.png;*.jpg"; nullptr for an empty pattern.
    static GtkFileFilter* MakeFilter(const std::string& name, const std::string& patterns) {
        if (patterns.empty()) return nullptr;
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, (name.empty() ? patterns : name).c_str());
        size_t start = 0;
        while (start <= patterns.size()) {
            const size_t end = std::min(patterns.find(';', start), patterns.size());
            if (end > start) gtk_file_filter_add_pattern(filter, patterns.substr(start, end - start).c_str());
            start = end + 1;
        }
        return filter;
    }

    // A dialog's file types (DialogFilters), each owned by the caller; the first is the one selected.
    template <typename Options>
    static std::vector<GtkFileFilter*> MakeFilters(const Options& options) {
        std::vector<GtkFileFilter*> filters;
        for (const FileFilter& f : DialogFilters(options)) {
            if (GtkFileFilter* filter = MakeFilter(f.name, f.pattern)) filters.push_back(filter);
        }
        return filters;
    }

#ifdef UNIAPI_GTK4
    template <typename Options>
    static void SetFilters(GtkFileDialog* dialog, const Options& options) {
        const std::vector<GtkFileFilter*> list = MakeFilters(options);
        if (list.empty()) return;
        GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        for (GtkFileFilter* filter : list) g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dialog, list.front());
        g_object_unref(filters);
        for (GtkFileFilter* filter : list) g_object_unref(filter);
    }

    // The folder the dialog opens in, if it is there.
    static void SetFolder(GtkFileDialog* dialog, const std::string& directory) {
        if (!IsFolder(directory)) return;
        GFile* folder = g_file_new_for_path(directory.c_str());
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }

    static void AddPath(GFile* file, std::vector<std::string>& paths) {
        if (char* path = g_file_get_path(file)) {
            paths.push_back(path);
            g_free(path);
        }
    }

    std::vector<std::string> OpenFileDialog(const OpenDialogOptions& options) override {
        if (!InitGtk()) return {};  // also before run()
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, options.title.c_str());
        gtk_file_dialog_set_modal(dialog, TRUE);
        if (!options.folder) SetFilters(dialog, options);
        SetFolder(dialog, options.directory);
        DialogWait wait;  // each variant reports a GFile (one pick) or a GListModel of GFiles
        if (options.folder && options.multiple) {
            gtk_file_dialog_select_multiple_folders(
                dialog, DialogOwner(), nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto* pending = static_cast<DialogWait*>(data);
                    pending->files = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(source), result, nullptr);
                    g_main_loop_quit(pending->loop);
                },
                &wait);
        } else if (options.folder) {
            gtk_file_dialog_select_folder(
                dialog, DialogOwner(), nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto* pending = static_cast<DialogWait*>(data);
                    pending->file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
                    g_main_loop_quit(pending->loop);
                },
                &wait);
        } else if (options.multiple) {
            gtk_file_dialog_open_multiple(
                dialog, DialogOwner(), nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto* pending = static_cast<DialogWait*>(data);
                    pending->files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, nullptr);
                    g_main_loop_quit(pending->loop);
                },
                &wait);
        } else {
            gtk_file_dialog_open(
                dialog, DialogOwner(), nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto* pending = static_cast<DialogWait*>(data);
                    pending->file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
                    g_main_loop_quit(pending->loop);
                },
                &wait);
        }
        wait.Run();
        g_object_unref(dialog);
        std::vector<std::string> paths;
        if (wait.file) {
            AddPath(wait.file, paths);
            g_object_unref(wait.file);
        }
        if (wait.files) {
            const guint count = g_list_model_get_n_items(wait.files);
            for (guint i = 0; i < count; ++i) {
                auto* file = static_cast<GFile*>(g_list_model_get_item(wait.files, i));
                AddPath(file, paths);
                g_object_unref(file);
            }
            g_object_unref(wait.files);
        }
        return paths;
    }

    std::optional<std::string> SaveFileDialog(const SaveDialogOptions& options) override {
        if (!InitGtk()) return std::nullopt;  // also before run()
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, options.title.c_str());
        gtk_file_dialog_set_modal(dialog, TRUE);
        if (!options.defaultName.empty()) gtk_file_dialog_set_initial_name(dialog, options.defaultName.c_str());
        SetFilters(dialog, options);
        SetFolder(dialog, options.directory);
        DialogWait wait;  // the dialog asks before overwriting
        gtk_file_dialog_save(dialog, DialogOwner(), nullptr,
                             +[](GObject* source, GAsyncResult* result, gpointer data) {
                                 auto* pending = static_cast<DialogWait*>(data);
                                 pending->file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
                                 g_main_loop_quit(pending->loop);
                             },
                             &wait);
        wait.Run();
        g_object_unref(dialog);
        std::optional<std::string> result;
        if (wait.file) {
            if (char* path = g_file_get_path(wait.file)) {
                result = path;
                g_free(path);
            }
            g_object_unref(wait.file);
        }
        return result;
    }
#else
    // The chooser's file types (it takes them over); the first is the one selected. And the folder
    // it opens in, if that is there.
    template <typename Options>
    static void SetFiltersAndFolder(GtkFileChooser* chooser, const Options& options, bool filters) {
        if (filters) {
            const std::vector<GtkFileFilter*> list = MakeFilters(options);
            for (GtkFileFilter* filter : list) gtk_file_chooser_add_filter(chooser, filter);
            if (!list.empty()) gtk_file_chooser_set_filter(chooser, list.front());
        }
        if (IsFolder(options.directory)) gtk_file_chooser_set_current_folder(chooser, options.directory.c_str());
    }

    std::vector<std::string> OpenFileDialog(const OpenDialogOptions& options) override {
        if (!InitGtk()) return {};  // also before run()
        GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
            options.title.c_str(), DialogOwner(),
            options.folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN, "_Open",
            "_Cancel");
        GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
        gtk_file_chooser_set_select_multiple(chooser, options.multiple);
        SetFiltersAndFolder(chooser, options, !options.folder);
        std::vector<std::string> paths;
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            GSList* names = gtk_file_chooser_get_filenames(chooser);
            for (GSList* item = names; item; item = item->next) {
                paths.push_back(static_cast<char*>(item->data));
                g_free(item->data);
            }
            g_slist_free(names);
        }
        g_object_unref(dialog);
        return paths;
    }

    std::optional<std::string> SaveFileDialog(const SaveDialogOptions& options) override {
        if (!InitGtk()) return std::nullopt;  // also before run()
        GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
            options.title.c_str(), DialogOwner(), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
        GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
        gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
        if (!options.defaultName.empty()) gtk_file_chooser_set_current_name(chooser, options.defaultName.c_str());
        SetFiltersAndFolder(chooser, options, true);
        std::optional<std::string> result;
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            if (char* path = gtk_file_chooser_get_filename(chooser)) {
                result = path;
                g_free(path);
            }
        }
        g_object_unref(dialog);
        return result;
    }
#endif

    // A further window (LinuxWindow, below). Before run() it is made with the main window.
    std::unique_ptr<BackendWindow> OpenWindow(const WindowOptions& options, BackendEvents events) override;

    // The clipboard's text. Reading is asynchronous (GTK 4's always; GTK 3's when another app
    // owns the clipboard). Before run() there is no display: nothing, and set fails.
    void ClipboardText(std::function<void(std::optional<std::string>)> done) override {
        if (!gtkStarted || !gdk_display_get_default()) {
            done(std::nullopt);
            return;
        }
        auto* pending = new std::function<void(std::optional<std::string>)>(std::move(done));
#ifdef UNIAPI_GTK4
        gdk_clipboard_read_text_async(
            gdk_display_get_clipboard(gdk_display_get_default()), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::function<void(std::optional<std::string>)>> answer(
                    static_cast<std::function<void(std::optional<std::string>)>*>(data));
                GError* error = nullptr;
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                if (error) g_error_free(error);
                std::optional<std::string> value;
                if (text) value = text;
                g_free(text);
                (*answer)(std::move(value));
            },
            pending);
#else
        gtk_clipboard_request_text(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            +[](GtkClipboard*, const gchar* text, gpointer data) {
                std::unique_ptr<std::function<void(std::optional<std::string>)>> answer(
                    static_cast<std::function<void(std::optional<std::string>)>*>(data));
                (*answer)(text ? std::optional<std::string>(text) : std::nullopt);
            },
            pending);
#endif
    }

    // GTK keeps the text while the app runs (and a clipboard manager, where the desktop has one,
    // after).
    bool SetClipboardText(const std::string& text) override {
        if (!gtkStarted || !gdk_display_get_default()) return false;
#ifdef UNIAPI_GTK4
        gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text.c_str());
#else
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), int(text.size()));
#endif
        return true;
    }

    // A new notification each time. Its name and icon come from the app's .desktop file, named
    // after the executable as the window's app id is (uniapi_install()).
    void Notify(const Notification& n, std::function<void(const std::string&)> done) override {
        gNotifying = this;
        GError* error = nullptr;
        GDBusConnection* bus = NotificationBus(&error);
        if (!bus) {
            done(error ? error->message : "no D-Bus session bus");
            if (error) g_error_free(error);
            return;
        }
        const std::string exe = executablePath();
        const std::string entry = exe.substr(exe.find_last_of('/') + 1);
        // "default" is the notification's body: clicking it (servers that show a button label it).
        static const gchar* const actions[] = {"default", "Open", nullptr};
        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        if (!entry.empty()) g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(entry.c_str()));
        struct Pending {
            LinuxBackend* backend;
            std::string id;
            std::function<void(const std::string&)> done;
        };
        // Notify(app_name, replaces_id: 0, a new one, app_icon, summary, body, actions, hints,
        // expire_timeout: -1, the server's default) -> (id). The icon: the app's, by the name its
        // .desktop file has. Cancelled with the backend: the reply then finds nobody.
        g_dbus_connection_call(
            bus, kNotificationsName, kNotificationsPath, kNotificationsName, "Notify",
            g_variant_new("(susss^asa{sv}i)", options_.title.c_str(), guint32(0), entry.c_str(), n.title.c_str(),
                          n.body.c_str(), actions, &hints, -1),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, cancellable_,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
                GError* failure = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &failure);
                if (g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_error_free(failure);  // the backend is gone
                    return;
                }
                guint32 server = 0;
                if (reply) {
                    g_variant_get(reply, "(u)", &server);
                    g_variant_unref(reply);
                    pending->backend->notifications_[server] = pending->id;
                }
                const std::string why = failure ? failure->message : "";
                if (failure) g_error_free(failure);
                pending->done(why);
            },
            new Pending{this, n.id, std::move(done)});
    }

    // The server's ActionInvoked: "default" is a click on the notification's body.
    void NotificationAction(guint32 server, const std::string& action) {
        if (action != "default") return;
        auto it = notifications_.find(server);
        if (it != notifications_.end() && events_.onNotificationClicked) events_.onNotificationClicked(it->second);
    }
    void NotificationClosed(guint32 server) { notifications_.erase(server); }

    // ConnectDesktop's host.
    const BackendEvents& Events() const { return events_; }
    bool dropping = false;  // GTK 3: a drop is under way (drag-drop came, its data not yet)

    // For further windows.
    const std::string& Origin() const { return origin_; }
    GtkWindow* MainWindowHandle() const { return window_; }
    bool TryShared() const { return tryShared_; }
    void ExtensionWorks() { sharedMemory_ = true; }  // a further window's handshake answered
    // A web view in the main window's context (and profile): one set of web processes, the same
    // scheme handler.
    bool DevTools() const { return options_.devTools; }
    bool ContextMenu() const { return options_.contextMenu; }
    GtkWidget* NewView() const {
#ifdef UNIAPI_GTK4
        return session_ ? GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context_, "network-session",
                                                  session_, nullptr))
                        : GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context_, nullptr));
#else
        return webkit_web_view_new_with_context(context_);
#endif
    }
#ifndef UNIAPI_GTK4
    GdkPixbuf* Icon() const { return icon_; }
#endif
    void Forget(LinuxWindow* window) { windows_.erase(std::remove(windows_.begin(), windows_.end(), window), windows_.end()); }

private:
    // The session bus, once, with the server's signals subscribed (delivered on the UI thread:
    // the default main context). For every app's notifications: only this app's ids count.
    static GDBusConnection* NotificationBus(GError** error) {
        static GDBusConnection* bus = nullptr;
        if (bus) return bus;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, error);
        if (!bus) return nullptr;
        auto signal = +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name,
                          GVariant* parameters, gpointer) {
            LinuxBackend* backend = gNotifying;
            if (!backend) return;
            if (g_strcmp0(name, "ActionInvoked") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
                guint32 id = 0;
                const gchar* action = nullptr;
                g_variant_get(parameters, "(u&s)", &id, &action);
                backend->NotificationAction(id, action ? action : "");
            } else if (g_strcmp0(name, "NotificationClosed") == 0 &&
                       g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) {
                guint32 id = 0, reason = 0;
                g_variant_get(parameters, "(uu)", &id, &reason);
                backend->NotificationClosed(id);
            }
        };
        g_dbus_connection_signal_subscribe(bus, kNotificationsName, kNotificationsName, "ActionInvoked", kNotificationsPath,
                                           nullptr, G_DBUS_SIGNAL_FLAGS_NONE, signal, nullptr, nullptr);
        g_dbus_connection_signal_subscribe(bus, kNotificationsName, kNotificationsName, "NotificationClosed",
                                           kNotificationsPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, signal, nullptr, nullptr);
        return bus;
    }

    void CreateWindows();
    void CloseWindows();
    GtkWindow* DialogOwner() const;  // the active window, a further one or the main one

    struct Timer {
        guint source = 0;
        guint interval = 1;
        std::shared_ptr<std::function<void()>> fn;
        bool first = true;
    };

    struct TimerTag {
        LinuxBackend* backend;
        int id;
    };

    guint AddTimerSource(int id, guint delay) {
        return g_timeout_add_full(
            G_PRIORITY_DEFAULT, delay,
            +[](gpointer data) -> gboolean {
                const TimerTag tag = *static_cast<TimerTag*>(data);
                return tag.backend->OnTimer(tag.id);
            },
            new TimerTag{this, id}, +[](gpointer data) { delete static_cast<TimerTag*>(data); });
    }

    gboolean OnTimer(int id) {
        auto it = timers_.find(id);
        if (it == timers_.end()) return G_SOURCE_REMOVE;
        // Held by a shared_ptr copy: the callback may cancel its own timer, and its state
        // (a mutable lambda's captures) lasts from tick to tick.
        std::shared_ptr<std::function<void()>> fn = it->second.fn;
        (*fn)();
        it = timers_.find(id);
        if (it == timers_.end()) return G_SOURCE_REMOVE;  // removed by the callback
        if (it->second.first) {  // the immediate tick is done; switch to the interval
            it->second.first = false;
            it->second.source = AddTimerSource(id, it->second.interval);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    // Shared memory off if the blank page hasn't loaded in `ms` (the web process start), or,
    // restarted by SendHandshake, the extension hasn't answered.
    void StartHandshakeTimer(guint ms) {
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        handshakeTimeout_ = g_timeout_add(ms, +[](gpointer self) -> gboolean {
            auto* backend = static_cast<LinuxBackend*>(self);
            backend->handshakeTimeout_ = 0;
            if (backend->handshakeSent_) {
                g_debug("shared memory off: the extension didn't answer within %u ms", kHandshakeTimeoutMs);
            } else {
                g_debug("shared memory off: the web process didn't start within %u ms", kWebProcessStartTimeoutMs);
            }
            backend->ReportReady(false);
            return G_SOURCE_REMOVE;
        }, this);
    }

    void SendHandshake() {
        if (handshakeSent_) return;
        handshakeSent_ = true;
        StartHandshakeTimer(kHandshakeTimeoutMs);
        webkit_web_view_send_message_to_page(
            webview_, webkit_user_message_new("uniapi.hello", nullptr), cancellable_,
            +[](GObject* view, GAsyncResult* result, gpointer self) {
                GError* error = nullptr;
                WebKitUserMessage* reply =
                    webkit_web_view_send_message_to_page_finish(WEBKIT_WEB_VIEW(view), result, &error);
                if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_error_free(error);  // the backend is gone; `self` must not be used
                    return;
                }
                auto* backend = static_cast<LinuxBackend*>(self);
                if (reply) {
                    GVariant* parameters = webkit_user_message_get_parameters(reply);
                    const bool gigacageOff = parameters && g_variant_is_of_type(parameters, G_VARIANT_TYPE_BOOLEAN) &&
                                             g_variant_get_boolean(parameters);
                    g_object_unref(reply);
                    if (!gigacageOff) g_debug("shared memory off: Gigacage is still on in the web process");
                    backend->ReportReady(gigacageOff);
                } else {
                    g_debug("shared memory off: no handshake (%s)", error ? error->message : "?");
                    backend->ReportReady(false);
                }
                if (error) g_error_free(error);
            },
            this);
    }

    void ReportReady(bool sharedMemory) {
        if (readyReported_) return;  // the answer or the timeout, whichever comes first
        readyReported_ = true;
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        handshakeTimeout_ = 0;
        sharedMemory_ = sharedMemory;
        if (sharedMemory) g_debug("shared memory on");
        if (events_.onReady) events_.onReady(sharedMemory);
    }

    void RunPosted() {
        std::vector<std::function<void()>> posted;
        {
            std::lock_guard<std::mutex> lock(postMutex_);
            posted.swap(posted_);
            postSource_ = 0;
        }
        for (auto& fn : posted) fn();
    }

    void OnDestroyed() {
        CloseWindows();  // every further window closes with the main one (their onClosed first)
        if (events_.onClosed) events_.onClosed();  // its placement read while the window is there
        placement_.Detach();
        webview_ = nullptr;
        window_ = nullptr;
        if (loop_) g_main_loop_quit(loop_);
    }

    void OnScriptMessage(JSCValue* value) { DeliverScriptMessage(webview_, origin_, events_, value); }

    void OnSchemeRequest(WebKitURISchemeRequest* request) override { AnswerSchemeRequest(request, origin_, events_); }

    static void OnSchemeRequestOfView(WebKitURISchemeRequest* request, gpointer) {
        WebKitWebView* view = webkit_uri_scheme_request_get_web_view(request);
        auto* host = view ? static_cast<ViewHost*>(g_object_get_data(G_OBJECT(view), kBackendKey)) : nullptr;
        if (host) {
            host->OnSchemeRequest(request);
            return;
        }
        GError* error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no uniapi app for this page");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
    }

    AppOptions options_;
    BackendEvents events_;
    std::string origin_;  // "uniapi://<host>"
    std::thread::id uiThread_;
    GtkWindow* window_ = nullptr;
    GMainLoop* loop_ = nullptr;
    WebKitWebView* webview_ = nullptr;
    bool fullscreen_ = false;
    gboolean systemPrefersDark_ = FALSE;  // gtk-application-prefer-dark-theme before we changed it
    bool systemPrefersDarkRead_ = false;
    GDBusProxy* portal_ = nullptr;        // the desktop portal's settings, for the colour scheme
    bool invisible_ = false;              // its page hidden (HidePage), not yet revealed (Reveal)
    guint revealTimer_ = 0;
    bool hidden_ = options_.startHidden;  // hide(), startHidden: not shown until SetVisible
    bool everShown_ = false;              // it has been shown (started hidden: once shown)
    bool closeAsked_ = false;             // Quit: closing without asking (AskClose)
    GtkPlacement placement_;
    guint32 colorScheme_ = 0;             // the portal's: 0 none, 1 prefer dark, 2 prefer light
#ifndef UNIAPI_GTK4
    GdkPixbuf* icon_ = nullptr;
#endif
    std::mutex postMutex_;
    std::vector<std::function<void()>> posted_;
    guint postSource_ = 0;
    bool readyReported_ = false;
    bool handshakeSent_ = false;
    guint handshakeTimeout_ = 0;
    bool sharedMemory_ = false;
    GCancellable* cancellable_ = g_cancellable_new();  // cancels the handshake when destroyed
    std::map<int, Timer> timers_;
    int nextTimer_ = 1;
    WebKitWebContext* context_ = nullptr;  // the main view's, for further windows' views
#ifdef UNIAPI_GTK4
    WebKitNetworkSession* session_ = nullptr;  // AppOptions::dataDirectory's; nullptr: the default
#endif
    bool tryShared_ = false;                 // the extension was found: views do the handshake
    std::vector<LinuxWindow*> windows_;      // further windows, until they are destroyed
    std::map<guint32, std::string> notifications_;  // shown: the server's id -> the core's
};

// A further window (App::openWindow): its own GtkWindow and web view, in the main window's web
// context and profile, so one scheme handler serves every page and the web process extension
// loads in each. Its events come through the BackendEvents it was opened with, as the main
// window's do. Owned: kept above the main window (transient for it; GTK 3 centres it over it, a
// GTK 4 compositor places it), and every further window closes with the main one. Modal: GTK
// keeps input from the app's other windows until it closes.
class LinuxWindow final : public BackendWindow, public ViewHost {
public:
    LinuxWindow(LinuxBackend& backend, const WindowOptions& options, BackendEvents events)
        : backend_(backend), options_(options), events_(std::move(events)) {}

    ~LinuxWindow() override {
        backend_.Forget(this);
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        if (revealTimer_) g_source_remove(revealTimer_);
        // A handshake reply still on its way must not reach this object.
        g_cancellable_cancel(cancellable_);
        g_object_unref(cancellable_);
        placement_.Detach();
        if (window_) {
            // Closed without events (the App ending while it is open).
            g_signal_handlers_disconnect_by_data(window_, this);
            if (webview_) {
                g_signal_handlers_disconnect_by_data(webview_, this);
                g_signal_handlers_disconnect_by_data(webkit_web_view_get_user_content_manager(webview_), this);
                g_object_set_data(G_OBJECT(webview_), kBackendKey, nullptr);
            }
            GtkWindow* window = window_;
            window_ = nullptr;
            webview_ = nullptr;
#ifdef UNIAPI_GTK4
            gtk_window_destroy(window);
#else
            gtk_widget_destroy(GTK_WIDGET(window));
#endif
        }
#ifndef UNIAPI_GTK4
        if (icon_) g_object_unref(icon_);
#endif
    }

    // Makes the window and its view: at once, or (opened before run()) with the main window.
    void Create() {
        if (created_) return;
        created_ = true;
#ifdef UNIAPI_GTK4
        GtkWidget* window = gtk_window_new();
#else
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (!options_.activate) gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
#endif
        window_ = GTK_WINDOW(window);
        gtk_window_set_title(window_, options_.title.c_str());
        gtk_window_set_default_size(window_, options_.width, options_.height);  // logical pixels
        gtk_window_set_resizable(window_, options_.resizable);
        if (options_.owned) {
            if (GtkWindow* main = backend_.MainWindowHandle()) {
                gtk_window_set_transient_for(window_, main);
#ifndef UNIAPI_GTK4
                gtk_window_set_position(window_, GTK_WIN_POS_CENTER_ON_PARENT);
#endif
            }
        }
        gtk_window_set_modal(window_, options_.modal ? TRUE : FALSE);
        ApplyHints();
        if (fullscreen_) gtk_window_fullscreen(window_);
#ifndef UNIAPI_GTK4
        if (GdkPixbuf* icon = icon_ ? icon_ : backend_.Icon()) gtk_window_set_icon(window_, icon);  // the app's by default
#endif
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer self) {
                             static_cast<LinuxWindow*>(self)->OnDestroyed();
                         }), this);
        ConnectCloseRequest(window, this);
        placement_.hidden = &hidden_;
        placement_.changed = [this] {
            if (!closing_ && events_.onPlacementChanged) events_.onPlacementChanged();
        };

        GtkWidget* view = backend_.NewView();
        webview_ = WEBKIT_WEB_VIEW(view);
        if (backend_.DevTools()) AllowDevTools(webview_);
        if (!backend_.ContextMenu()) LimitContextMenu(webview_);
        ApplyBackground(GTK_WIDGET(window_), webview_, ColorValue(options_.backgroundColor));
        g_object_set_data(G_OBJECT(view), kBackendKey, static_cast<ViewHost*>(this));  // dies with the view
        placement_.Attach(window_, webview_);

        WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(webview_);
#ifdef UNIAPI_GTK4
        g_signal_connect(manager, "script-message-received::uniapi",
                         G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer self) {
                             auto* w = static_cast<LinuxWindow*>(self);
                             DeliverScriptMessage(w->webview_, w->backend_.Origin(), w->events_, value);
                         }),
                         this);
        webkit_user_content_manager_register_script_message_handler(manager, "uniapi", kWorld);
#else
        g_signal_connect(manager, "script-message-received::uniapi",
                         G_CALLBACK(+[](WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer self) {
                             auto* w = static_cast<LinuxWindow*>(self);
                             DeliverScriptMessage(w->webview_, w->backend_.Origin(), w->events_,
                                                  webkit_javascript_result_get_js_value(result));
                         }),
                         this);
        webkit_user_content_manager_register_script_message_handler_in_world(manager, "uniapi", kWorld);
#endif
        AddBridge(manager);

        g_signal_connect(view, "load-changed",
                         G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer self) {
                             auto* w = static_cast<LinuxWindow*>(self);
                             if (!w->readyReported_) {  // the handshake's blank page
                                 if (event == WEBKIT_LOAD_FINISHED) w->SendHandshake();
                             } else if (event == WEBKIT_LOAD_STARTED && w->events_.onNavigationStarting) {
                                 w->events_.onNavigationStarting();
                             } else if (event == WEBKIT_LOAD_FINISHED) {
                                 w->PageLoaded();
                             }
                         }),
                         this);
        ConnectDesktop(webview_, this);

        // As the main window: its page hidden until drawn; started hidden, shown by SetVisible.
        HidePage(window, view);
        invisible_ = true;
        StartRevealTimer(kRevealStartTimeoutMs);  // until its page has loaded (PageLoaded)
        everShown_ = !hidden_;
#ifdef UNIAPI_GTK4
        gtk_window_set_child(window_, view);
        if (hidden_) {
            g_debug("window \"%s\" opens hidden", options_.title.c_str());
        } else if (options_.activate) {
            gtk_window_present(window_);
        } else {
            gtk_widget_set_visible(window, TRUE);
        }
#else
        gtk_container_add(GTK_CONTAINER(window), view);
        if (hidden_) {
            gtk_widget_show(view);  // the window shown later shows it
            g_debug("window \"%s\" opens hidden", options_.title.c_str());
        } else {
            gtk_widget_show_all(window);
        }
#endif
        placement_.ApplyPending();
        gtk_widget_grab_focus(view);
        if (options_.minWidth > 0 || options_.minHeight > 0) {
            gtk_widget_set_size_request(view, options_.minWidth > 0 ? options_.minWidth : -1,
                                        options_.minHeight > 0 ? options_.minHeight : -1);
        }

        if (backend_.TryShared()) {
            // Its own web process (or a shared one) answers the extension's handshake for this view.
            webkit_web_view_load_uri(webview_, "about:blank");
            StartHandshakeTimer(kWebProcessStartTimeoutMs);
        } else {
            ReportReady(false);
        }
    }

    // The main window closed: this one closes too, with its events.
    void Destroy() {
        if (!window_) return;
#ifdef UNIAPI_GTK4
        gtk_window_destroy(window_);  // "destroy": OnDestroyed
#else
        gtk_widget_destroy(GTK_WIDGET(window_));
#endif
    }

    GtkWindow* Handle() const { return window_; }

    // BackendWindow
    void Navigate(const std::string& path) override {
        if (webview_) webkit_web_view_load_uri(webview_, (backend_.Origin() + path).c_str());
    }
    void PostToPage(const std::string& text) override { PostScript(webview_, text); }
    void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) override {
        SendRegion(webview_, region, infoJson, writable);
    }

    void SetTitle(const std::string& title) override {
        options_.title = title;
        if (window_) gtk_window_set_title(window_, title.c_str());
    }

    void SetSize(int width, int height) override {
        options_.width = width;
        options_.height = height;
        if (!window_ || fullscreen_) return;
        if (gtk_window_is_maximized(window_)) gtk_window_unmaximize(window_);
#ifdef UNIAPI_GTK4
        gtk_window_set_default_size(window_, width, height);
#else
        ApplyHints();
        gtk_window_resize(window_, width, height);
#endif
    }

    void SetFullscreen(bool on) override {
        fullscreen_ = on;
        if (!window_) return;  // applied when it opens
        if (on) {
            gtk_window_fullscreen(window_);
        } else {
            gtk_window_unfullscreen(window_);
        }
    }

    bool IsFullscreen() const override { return fullscreen_; }

    // GTK's dark or light preference is the app's, not a window's (see LinuxBackend::ApplyTitleBar):
    // a further window's title bar follows the main window's setting. Kept to answer GetTitleBar.
    void SetTitleBar(TitleBar mode) override { options_.titleBar = mode; }
    TitleBar GetTitleBar() const override { return options_.titleBar; }

    void Show() override { Reveal(); }

    // As the main window's (LinuxBackend::SetVisible).
    void SetVisible(bool visible) override {
        hidden_ = !visible;
        if (!window_ || closing_) return;  // before it is made: how it opens
        if (!visible) {
            placement_.Hide();
            return;
        }
        if (!everShown_) {
            everShown_ = true;
            if (!invisible_) HideUntilDrawn();
        }
        placement_.Show(options_.activate);
    }

    // Placement (GtkPlacement).
    Size CurrentSize() const override { return placement_.CurrentSize(options_.width, options_.height); }
    std::optional<Point> Position() const override { return placement_.Position(); }
    bool SetPosition(int x, int y) override { return placement_.SetPosition(x, y); }
    void Center() override { placement_.Center(); }
    WindowState State() const override { return placement_.State(); }
    void SetState(WindowState state) override { placement_.SetState(state); }
    Bounds NormalBounds() const override { return placement_.NormalBounds(options_.width, options_.height); }
    bool SetAlwaysOnTop(bool on) override { return placement_.SetAlwaysOnTop(on); }
    bool IsAlwaysOnTop() const override { return placement_.IsAlwaysOnTop(); }
    std::vector<Screen> Screens() const override { return placement_.Screens(); }

    bool IsVisible() const override { return !hidden_; }

    bool AskClose() {
        if (closeAsked_ || closing_ || !window_) return true;
        return !events_.onCloseRequested || events_.onCloseRequested(CloseReason::Window);
    }

#ifdef UNIAPI_GTK4
    bool SetIcon(const unsigned char*, size_t) override { return false; }  // by name only, as the main window
#else
    bool SetIcon(const unsigned char* png, size_t size) override {
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        if (!loader) return false;
        const bool wrote = gdk_pixbuf_loader_write(loader, png, size, nullptr);
        const bool closed = gdk_pixbuf_loader_close(loader, nullptr);
        GdkPixbuf* pixbuf = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
        if (pixbuf) {
            if (icon_) g_object_unref(icon_);
            icon_ = GDK_PIXBUF(g_object_ref(pixbuf));
            if (window_) gtk_window_set_icon(window_, icon_);
        }
        g_object_unref(loader);
        return pixbuf != nullptr;
    }
#endif

    void OpenDevTools() override { OpenDevToolsOf(webview_, backend_.DevTools()); }

    void Close() override {
        if (!window_ || closing_) return;
        closeAsked_ = true;  // without asking (AskClose)
        if (!gtk_widget_get_realized(GTK_WIDGET(window_))) {
            Destroy();  // never shown: gtk_window_close does nothing to it (see LinuxBackend::Quit)
            return;
        }
        gtk_window_close(window_);  // "destroy" follows
    }

    // ViewHost
    void OnSchemeRequest(WebKitURISchemeRequest* request) override {
        AnswerSchemeRequest(request, backend_.Origin(), events_);
    }

    // ConnectDesktop's host.
    const std::string& Origin() const { return backend_.Origin(); }
    const BackendEvents& Events() const { return events_; }
    bool dropping = false;

    // Its page has drawn after loading (AfterPageDrawn): the window appears.
    void PageDrawn() {
        if (invisible_) StartRevealTimer(kRevealAfterDrawMs);
    }

private:
    void ApplyHints() { ApplyGeometryHints(window_, options_); }  // and the maximum size

    // As the main window's (LinuxBackend::PageLoaded).
    void PageLoaded() {
        if (!invisible_) return;
        StartRevealTimer(options_.showWhenReady ? kRevealTimeoutReadyMs : kRevealTimeoutMs);
        if (!options_.showWhenReady) AfterPageDrawn(webview_, cancellable_, this);
    }

    void StartRevealTimer(guint ms) {
        if (revealTimer_) g_source_remove(revealTimer_);
        revealTimer_ = g_timeout_add(ms, +[](gpointer self) -> gboolean {
            auto* w = static_cast<LinuxWindow*>(self);
            w->revealTimer_ = 0;
            w->Reveal();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void Reveal() {
        if (revealTimer_) {
            g_source_remove(revealTimer_);
            revealTimer_ = 0;
        }
        if (!invisible_) return;
        invisible_ = false;
        ShowPage(window_, webview_);
    }

    // As the main window's (LinuxBackend::HideUntilDrawn).
    void HideUntilDrawn() {
        HidePage(GTK_WIDGET(window_), GTK_WIDGET(webview_));
        invisible_ = true;
        StartRevealTimer(kRevealTimeoutMs);
        AfterPageDrawn(webview_, cancellable_, this);
    }

    // As the main window's (LinuxBackend::StartHandshakeTimer).
    void StartHandshakeTimer(guint ms) {
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        handshakeTimeout_ = g_timeout_add(ms, +[](gpointer self) -> gboolean {
            auto* w = static_cast<LinuxWindow*>(self);
            w->handshakeTimeout_ = 0;
            if (w->handshakeSent_) {
                g_debug("a further window: shared memory off, the extension didn't answer within %u ms",
                        kHandshakeTimeoutMs);
            } else {
                g_debug("a further window: shared memory off, the web process didn't start within %u ms",
                        kWebProcessStartTimeoutMs);
            }
            w->ReportReady(false);
            return G_SOURCE_REMOVE;
        }, this);
    }

    void SendHandshake() {
        if (handshakeSent_) return;
        handshakeSent_ = true;
        StartHandshakeTimer(kHandshakeTimeoutMs);
        webkit_web_view_send_message_to_page(
            webview_, webkit_user_message_new("uniapi.hello", nullptr), cancellable_,
            +[](GObject* view, GAsyncResult* result, gpointer self) {
                GError* error = nullptr;
                WebKitUserMessage* reply =
                    webkit_web_view_send_message_to_page_finish(WEBKIT_WEB_VIEW(view), result, &error);
                if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_error_free(error);  // the window is gone; `self` must not be used
                    return;
                }
                auto* w = static_cast<LinuxWindow*>(self);
                bool gigacageOff = false;
                if (reply) {
                    GVariant* parameters = webkit_user_message_get_parameters(reply);
                    gigacageOff = parameters && g_variant_is_of_type(parameters, G_VARIANT_TYPE_BOOLEAN) &&
                                  g_variant_get_boolean(parameters);
                    g_object_unref(reply);
                }
                if (error) g_error_free(error);
                w->ReportReady(gigacageOff);
            },
            this);
    }

    void ReportReady(bool sharedMemory) {
        if (readyReported_) return;  // the answer or the timeout, whichever comes first
        readyReported_ = true;
        if (handshakeTimeout_) g_source_remove(handshakeTimeout_);
        handshakeTimeout_ = 0;
        if (sharedMemory) backend_.ExtensionWorks();  // regions can be made, also before the main window's answer
        if (events_.onReady) events_.onReady(sharedMemory);
    }

    void OnDestroyed() {
        closing_ = true;
        if (events_.onClosed) events_.onClosed();  // the core destroys this object later (posted)
        placement_.Detach();
        webview_ = nullptr;
        window_ = nullptr;
    }

    LinuxBackend& backend_;
    WindowOptions options_;
    BackendEvents events_;
    GtkWindow* window_ = nullptr;
    WebKitWebView* webview_ = nullptr;
    bool created_ = false;
    bool closing_ = false;
    bool fullscreen_ = false;
    bool invisible_ = false;
    guint revealTimer_ = 0;
    bool hidden_ = options_.startHidden;  // hide(), startHidden: not shown until SetVisible
    bool everShown_ = false;
    bool closeAsked_ = false;             // Close: closing without asking (AskClose)
    GtkPlacement placement_;
    bool readyReported_ = false;
    bool handshakeSent_ = false;
    guint handshakeTimeout_ = 0;
    GCancellable* cancellable_ = g_cancellable_new();
#ifndef UNIAPI_GTK4
    GdkPixbuf* icon_ = nullptr;
#endif
};

std::unique_ptr<BackendWindow> LinuxBackend::OpenWindow(const WindowOptions& options, BackendEvents events) {
    auto window = std::make_unique<LinuxWindow>(*this, options, std::move(events));
    windows_.push_back(window.get());
    if (window_) window->Create();  // otherwise run() makes it, with the main window
    return window;
}

void LinuxBackend::CreateWindows() {
    for (LinuxWindow* window : std::vector<LinuxWindow*>(windows_)) window->Create();
}

void LinuxBackend::CloseWindows() {
    for (LinuxWindow* window : std::vector<LinuxWindow*>(windows_)) window->Destroy();
}

GtkWindow* LinuxBackend::DialogOwner() const {
    for (LinuxWindow* window : windows_) {
        if (window->Handle() && gtk_window_is_active(window->Handle())) return window->Handle();
    }
    return window_;
}

}  // namespace

// xdg-open: the desktop's default app for it (freedesktop.org's xdg-utils), waited for.
void SystemOpen(const std::string& target) {
    const char* argv[] = {"xdg-open", target.c_str(), nullptr};
    GError* error = nullptr;
    gint status = 0;
    if (!g_spawn_sync(nullptr, const_cast<char**>(argv), nullptr,
                      GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr,
                      nullptr, nullptr, nullptr, &status, &error)) {
        const std::string why = error ? error->message : "xdg-open didn't start";
        if (error) g_error_free(error);
        throw std::runtime_error(why);
    }
#if GLIB_CHECK_VERSION(2, 70, 0)
    const bool ok = g_spawn_check_wait_status(status, nullptr);
#else
    const bool ok = g_spawn_check_exit_status(status, nullptr);
#endif
    if (!ok) throw std::runtime_error("xdg-open failed (exit status " + std::to_string(WEXITSTATUS(status)) + ")");
}

// The file manager's ShowItems (org.freedesktop.FileManager1: Nautilus, Dolphin, Nemo, Thunar...)
// selects it; without it, its folder opens.
void SystemReveal(const std::string& path) {
    GError* error = nullptr;
    char* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    GDBusConnection* bus = uri ? g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error) : nullptr;
    GVariant* reply = nullptr;
    if (bus) {
        const char* uris[] = {uri, nullptr};
        reply = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                            "org.freedesktop.FileManager1", "ShowItems",
                                            g_variant_new("(^ass)", uris, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
                                            nullptr, &error);
        g_object_unref(bus);
    }
    g_free(uri);
    if (reply) {
        g_variant_unref(reply);
        return;
    }
    if (error) {
        g_debug("revealPath: %s; opening its folder", error->message);
        g_error_free(error);
    }
    char* folder = g_path_get_dirname(path.c_str());
    const std::string parent = folder;
    g_free(folder);
    SystemOpen(parent);
}

std::unique_ptr<Backend> CreateBackend(const AppOptions& options, BackendEvents events) {
    return std::make_unique<LinuxBackend>(options, std::move(events));
}

}  // namespace detail
}  // namespace uniapi
