#pragma once

// The seam between the platform-independent core (app.cpp) and one platform's window and
// web view. Each platform implements Backend once: Windows in platform/windows/, Linux
// (WebKitGTK) in platform/linux/. Everything crossing this interface is UTF-8.

#include <uniapi/uniapi.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uniapi::detail {

// Memory mapped into both the app and the page's process.
class SharedRegion {
public:
    virtual ~SharedRegion() = default;
    uint8_t* data = nullptr;
    size_t size = 0;
};

// A request the page made on the app's origin.
struct Request {
    std::string method = "GET";
    std::string path;            // everything after the host, e.g. "/index.html?draw=2d"
    std::vector<uint8_t> body;   // POST data (binary data the page sends with a call or event)
};

// The answer to a request the page made for a file or for stream data.
struct Response {
    int status = 404;
    std::string mimeType = "text/plain";
    const uint8_t* data = nullptr;  // not copied: keepAlive owns it (or it is static, e.g. embedded files)
    size_t size = 0;
    std::shared_ptr<const void> keepAlive;
};

struct BackendEvents {
    std::function<void(bool sharedMemorySupported)> onReady;  // the web view exists
    std::function<void(const std::string& text)> onMessage;   // from the page
    // Raised on the UI thread; the core may take the request's body. The platform may read
    // the response data later from another thread.
    std::function<void(Request& request, Response& response)> onRequest;
    std::function<void()> onNavigationStarting;
    // The user asks the window to close: false keeps it open. Not for Close/Quit or the app
    // ending, nor a logout or shutdown.
    std::function<bool(CloseReason reason)> onCloseRequested;
    std::function<void()> onClosed;
    // The page tried to go somewhere that isn't the app's (a link or window.open to another site,
    // mailto:...): the platform cancelled it (and any new window); the core decides whether to
    // open it outside. Navigations to file: URLs are cancelled without this. Iframes load what
    // they like.
    std::function<void(const std::string& url)> onExternalNavigation;
    // Files dropped on the window: their paths, and where, in the page's CSS pixels.
    std::function<void(const std::vector<std::string>& paths, int x, int y)> onFileDrop;
    // The window moved, was resized, minimised, maximised or restored (often, while dragged).
    std::function<void()> onPlacementChanged;
    // A notification (Backend::Notify) was clicked: its id. The main window's events only.
    std::function<void(const std::string& id)> onNotificationClicked;
};

// A window's state: shown normally, minimised or maximised (full screen apart).
enum class WindowState { Normal, Minimized, Maximized };

// An area of the screen: logical pixels, from the primary screen's top-left corner.
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};

// A window's normal placement (what restoring gives): its frame's top-left corner and its page
// area's size. hasPosition is false where the platform doesn't tell (Wayland, GTK 4).
struct Bounds {
    int x = 0, y = 0, width = 0, height = 0;
    bool hasPosition = false;
};

// Where a navigation goes: the app's own pages (and about:, data:, blob:, javascript:), a file
// (never: a file dropped on a page that doesn't take it would replace the page), or outside.
enum class Destination { App, File, Outside };
Destination DestinationOf(const std::string& origin, const std::string& url);  // app.cpp

// A dialog's whole list of file types: filterName/filterPattern's first, then more; those
// without a pattern are left out. A name or pattern never holds a line break. (desktop.cpp)
std::vector<FileFilter> DialogFilters(const std::string& name, const std::string& pattern,
                                      const std::vector<FileFilter>& more);
template <typename Options>
std::vector<FileFilter> DialogFilters(const Options& options) {
    return DialogFilters(options.filterName, options.filterPattern, options.filters);
}
// The WebKit version in the macOS WebKit framework's bundle version, which puts the macOS
// generation before it: "20621.1.15.11.10" is "621.1.15.11.10" (WebKit's major version has three
// digits). Anything else comes back as it is. (desktop.cpp)
std::string WebKitVersion(const std::string& bundleVersion);
// The version in WebView2's browser version string, which names the channel after it for the
// preview channels: "133.0.3065.0 canary" is "133.0.3065.0". (desktop.cpp)
std::string RuntimeVersion(const std::string& browserVersion);

// A "#rrggbb" or "#rgb" colour (AppOptions::backgroundColor) as 0xRRGGBB, or -1 for none: the
// empty string, and anything else, which it reports on stderr. (desktop.cpp)
int ColorValue(const std::string& color);

// Whether path (UTF-8) is a folder that is there: where a dialog may open. (desktop.cpp)
bool IsFolder(const std::string& path);

// Opening things with the system (each platform's backend): an absolute path that exists, or a
// URL with a scheme. Throw std::runtime_error on failure.
void SystemOpen(const std::string& target);
void SystemReveal(const std::string& path);

// A further window with its own web view and page (App::openWindow). Its events come through
// the BackendEvents it was opened with, as the main window's do. Destroying it closes it
// without events (the App ending).
class BackendWindow {
public:
    virtual ~BackendWindow() = default;
    virtual void Navigate(const std::string& path) = 0;
    virtual void PostToPage(const std::string& text) = 0;
    virtual void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) = 0;
    virtual void SetTitle(const std::string& title) = 0;
    virtual void SetFullscreen(bool on) = 0;
    virtual void SetSize(int width, int height) = 0;
    virtual bool IsFullscreen() const = 0;
    virtual void SetTitleBar(TitleBar mode) = 0;
    virtual TitleBar GetTitleBar() const = 0;
    virtual void Show() = 0;  // the window kept hidden until its page is drawn appears
    // Shows the window again (in front, restored if minimised; with focus if its options say
    // activate) or hides it (hide(), startHidden). Before the window exists: how it opens. Its
    // page runs either way.
    virtual void SetVisible(bool visible) = 0;
    virtual bool IsVisible() const = 0;  // not hidden; a minimised window counts as visible
    virtual bool SetIcon(const unsigned char* png, size_t size) = 0;
    // Closes it, as its close button would but without onCloseRequested: onClosed follows.
    virtual void Close() = 0;
    // The engine's developer tools on its page, where the platform can and AppOptions::devTools
    // allows; nothing before the web view exists.
    virtual void OpenDevTools() = 0;

    // Placement. Positions are the frame's top-left corner, sizes the page area's, in logical
    // pixels from the primary screen's top-left corner. Before the window exists, setters say how
    // it opens.
    virtual Size CurrentSize() const = 0;
    virtual std::optional<Point> Position() const = 0;  // empty where the platform doesn't tell
    virtual bool SetPosition(int x, int y) = 0;          // false where the platform can't
    virtual void Center() = 0;
    virtual WindowState State() const = 0;
    virtual void SetState(WindowState state) = 0;  // a hidden window takes it when shown
    virtual Bounds NormalBounds() const = 0;       // what restoring gives (maximised: the size before)
    virtual bool SetAlwaysOnTop(bool on) = 0;      // false where the platform can't (GTK 4, Wayland)
    virtual bool IsAlwaysOnTop() const = 0;
    // The desktop's screens, the one this window is on marked current (none before it exists);
    // empty where the platform can't say yet (GTK before Run()).
    virtual std::vector<Screen> Screens() const = 0;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual int Run() = 0;  // creates the window and web view, then runs the event loop
    virtual void Quit() = 0;  // closes the main window without onCloseRequested
    virtual void Navigate(const std::string& path) = 0;
    virtual void PostToPage(const std::string& text) = 0;
    // nullptr when shared memory isn't available.
    virtual std::unique_ptr<SharedRegion> CreateSharedRegion(size_t bytes) = 0;
    // Gives the page access to `region`; it arrives with `infoJson`. `writable`: the page writes
    // into it and the app reads what it wrote (a Sink's slot); otherwise the page only reads (a
    // Stream's), and anything it writes stays its own. A region that doesn't arrive is handled
    // by the page, which falls back to fetching (a stream) or uploading (a sink).
    virtual void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) = 0;
    virtual void Post(std::function<void()> fn) = 0;  // thread-safe
    virtual bool OnUiThread() const = 0;
    virtual void SetTitle(const std::string& title) = 0;
    virtual void SetFullscreen(bool on) = 0;
    virtual void SetSize(int width, int height) = 0;  // the page area; may come before Run()
    virtual bool IsFullscreen() const = 0;
    virtual void SetTitleBar(TitleBar mode) = 0;  // may come before Run()
    virtual TitleBar GetTitleBar() const = 0;
    virtual void Show() = 0;  // the window kept hidden until its page is drawn appears
    virtual void SetVisible(bool visible) = 0;  // as BackendWindow's; may come before Run()
    virtual bool IsVisible() const = 0;
    virtual bool SetIcon(const unsigned char* png, size_t size) = 0;  // may come before Run()
    virtual void OpenDevTools() = 0;  // the main window's, as BackendWindow's
    // The main window's placement: as BackendWindow's.
    virtual Size CurrentSize() const = 0;
    virtual std::optional<Point> Position() const = 0;
    virtual bool SetPosition(int x, int y) = 0;
    virtual void Center() = 0;
    virtual WindowState State() const = 0;
    virtual void SetState(WindowState state) = 0;
    virtual Bounds NormalBounds() const = 0;
    virtual bool SetAlwaysOnTop(bool on) = 0;
    virtual bool IsAlwaysOnTop() const = 0;
    virtual std::vector<Screen> Screens() const = 0;  // the main window's, as BackendWindow's
    // The screens' work areas (without menu bar, Dock, taskbar); empty where windows can't be
    // placed (Wayland, GTK 4) and, on Linux, before Run() (no display yet).
    virtual std::vector<Rect> ScreenAreas() const = 0;
    // The web engine the pages run in and its version ("" while not known: on Windows until
    // Run() has started WebView2).
    virtual WebEngine Engine() const = 0;
    virtual int AddTimer(std::chrono::milliseconds interval, std::function<void()> fn) = 0;
    virtual void RemoveTimer(int id) = 0;
    // The clipboard's text: done(text), or done(nullopt) when it holds none; on the UI thread,
    // perhaps at once (the core posts it). Set: false if the system refused it.
    virtual void ClipboardText(std::function<void(std::optional<std::string>)> done) = 0;
    virtual bool SetClipboardText(const std::string& text) = 0;
    // Shows a new system notification, called on the UI thread once Run() has started;
    // notification.id is set. done (never empty) gets "" once the system has it, or why not, on
    // the UI thread, once. A click reports the main window's onNotificationClicked(id).
    virtual void Notify(const Notification& notification, std::function<void(const std::string& error)> done) = 0;
    virtual std::optional<std::string> SaveFileDialog(const SaveDialogOptions& options) = 0;
    virtual std::vector<std::string> OpenFileDialog(const OpenDialogOptions& options) = 0;
    // A further window (App::openWindow); before Run(), it opens with the main window. nullptr
    // where the platform can't open more windows yet. (Not "CreateWindow": a Windows macro.)
    virtual std::unique_ptr<BackendWindow> OpenWindow(const WindowOptions& options, BackendEvents events) {
        (void)options;
        (void)events;
        return nullptr;
    }
};

std::unique_ptr<Backend> CreateBackend(const AppOptions& options, BackendEvents events);

}  // namespace uniapi::detail
