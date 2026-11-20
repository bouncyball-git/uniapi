#pragma once

// uniapi: a desktop app whose UI is a web page and whose work is done in C++.
//
//   uniapi::AppOptions options;
//   options.title = "Demo";
//   uniapi::App app(options);
//   app.serve(demo_web);                                   // pages compiled into the program
//   app.expose("add", [](int a, int b) { return a + b; }); // JS: await app.native.add(1, 2)
//   app.onConnect = [&](const std::string& page) {
//       app.call("greet", uniapi::json::array({"from C++"}));  // runs a JS function
//   };
//   app.navigate("/index.html");
//   return app.run();
//
// Calls go both ways and are asynchronous. Small data travels as JSON messages; large data
// goes through streams, which use zero-copy shared memory where the platform supports it
// (Windows and Linux; not macOS) and otherwise let the page fetch the bytes from the app (one or more
// copies, but no base64). Pages load /uniapi/uniapi.js, which App serves itself.
//
// Threading: App, Window and Stream belong to the thread that created the App (the UI thread),
// where every handler, timer and posted function runs. Methods that only do something (emit,
// call, setTitle, show, hide, close, navigate, expose, on, quit, cancel, serve, setPosition...)
// work from any thread: called elsewhere they post themselves to the UI thread and return at
// once, happening a moment later in order with other posted work. Methods that answer something
// or must act at once (connected, page, visible, size, stream, sendDom, openWindow, every, the
// dialogs, Stream::acquire and publish...) throw std::logic_error when called elsewhere, naming
// themselves. post(), notify(), Reply and onUiThread() work from anywhere.
//
// More windows: app.openWindow({...}) opens one with its own page, connected to C++ the same way
// (uniapi::Window: expose, on, call, emit, streams); App is the main window.

#include <uniapi/assets.hpp>
#include <uniapi/dom.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace uniapi {

using json = nlohmann::json;

// The window's title bar and frame: following the system's light or dark setting (and changes
// to it while the app runs), or always dark, or always light.
enum class TitleBar { System, Dark, Light };

// A window's position (its frame's top-left corner) and size (its page area's), in logical
// pixels from the primary screen's top-left corner (Window::position, Window::size).
struct Point {
    int x = 0;
    int y = 0;
};
struct Size {
    int width = 0;
    int height = 0;
};

// One of the desktop's screens (monitors): Window::screens().
struct Screen {
    // The whole screen (work area and bars), in logical pixels from the primary screen's
    // top-left corner.
    int x = 0, y = 0, width = 0, height = 0;
    // Physical pixels per logical pixel: 1, 1.25, 2... GTK 3 knows whole factors only (on
    // Wayland a fractional scale comes rounded up: 2 for 1.2).
    double scale = 1;
    // The desktop's main screen (the menu bar's, the taskbar's). False for every screen where
    // the platform doesn't say (Linux: GTK 4, and Wayland, which has none).
    bool primary = false;
    // The window asked is on it (most of it, where it spans several).
    bool current = false;
};

// The web engine the app's pages run in: App::webEngine().
struct WebEngine {
    std::string name;     // "WebKitGTK", "WebView2", "WebKit"
    std::string version;  // "2.50.1", "131.0.2903.86", "621.1.15"; "" while not known
    // Which build of the engine, where there is more than one: on Linux WebKitGTK's API version
    // and the GTK it is for, "API 6.0, GTK 4" or "API 4.1, GTK 3" (the same engine version comes
    // in both). "" on Windows and macOS.
    std::string variant;
};

// A system notification (App::notify): a banner, kept in the system's list of notifications
// where it has one.
struct Notification {
    std::string title;
    std::string body;
    // Names it for App::onNotificationClicked. Every notification is a new one: several may have
    // one id. Empty: notify() makes one, unique across runs (macOS keeps notifications after the
    // program ends, and a click on one comes to the next run).
    std::string id;
};

// Why the user asks a window to close (Window::onCloseRequested).
enum class CloseReason {
    Window,  // the window's close button, Cmd+W, Alt+F4, the taskbar's or the window manager's Close
    Quit,    // macOS: the app's Quit (Cmd+Q, the app menu, the Dock); asked of the main window
};

struct AppOptions {
    std::string title = "uniapi";
    // The page's area in logical pixels (scaled for the screen), without title bar and borders.
    int width = 1000;
    int height = 700;
    int minWidth = 0;        // the smallest the user can make it; 0 = no minimum
    int minHeight = 0;
    // The largest the user can make it (the page's area, as width and height); 0 = no maximum.
    // Maximising and full screen stop at it too, where the system applies it to them. Done on
    // Windows, macOS, and on Linux with GTK 3; GTK 4 has no way to limit a window's size, so
    // there it does nothing.
    int maxWidth = 0;
    int maxHeight = 0;
    // The colour behind the page: what the window shows before the page has drawn, at its edges
    // while it is resized, and through a page that sets no background of its own. "#rrggbb" or
    // "#rgb"; empty: the system's (the window's plain background). A dark app names its page's
    // colour here, so nothing lighter flashes. Anything else is reported on stderr and ignored.
    std::string backgroundColor;
    bool resizable = true;   // false: fixed size, no maximize
    // Resizing keeps the page area's width:height (that of width and height above). Done on
    // Windows, macOS, and on Linux with GTK 3 under X11. Wayland and GTK 4 have no way to ask for it:
    // there the window resizes freely, so a page that must keep its shape scales itself.
    bool keepAspectRatio = false;
    bool fullscreen = false; // start in full screen (see App::setFullscreen)
    // The title bar and frame: the system's light or dark (default), or forced (see
    // App::setTitleBar). Windows 10 (20H1 or later) and 11; macOS (the window's appearance); on
    // Linux GTK's dark preference.
    TitleBar titleBar = TitleBar::System;
    // The window appears only with its page drawn, never empty or white: by default once the
    // first page has loaded. A page that builds itself after loading (asks C++ first, resizes
    // the window) sets this and shows the window when it is ready: app.show() in the page, or
    // App::show(); after 5 seconds it appears anyway (on Linux, 5 seconds after its page has
    // loaded, so WebKit's slow first start doesn't count). On Linux the window itself shows
    // early with its page hidden, except GTK 3 on X11 with a compositor (wholly transparent).
    bool showWhenReady = false;
    // Use zero-copy shared memory for streams where the platform supports it. When false,
    // or when it isn't supported, streams fall back to the page fetching the bytes.
    bool sharedMemory = true;
    // Give the window keyboard focus when it opens (and when show() brings it back). False
    // shows it without taking focus from the app you're in, e.g. for automated tests.
    bool activate = true;
    // Open hidden: the window and its page are made and the page loads and runs (it connects,
    // calls and is called), but nothing is on screen until show(). For an app that starts in
    // the background (in the tray, at login).
    bool startHidden = false;
    // Only one instance of the app runs: a second launch hands its arguments and working folder
    // to the running one (App::onSecondInstance; without it, its main window comes forward) and
    // ends, in the App constructor (std::exit(0)), before any window opens. An id unique to the
    // app, such as "org.example.mixer"; empty: as many as are started. Per user: a Unix-domain
    // socket in the user's own runtime or temporary folder (Linux, macOS), a named pipe on
    // Windows.
    std::string singleInstance;
    // The window opens where and as it was left (position, size, maximised): saved under this
    // name when it closes, in the user's config folder (<config>/<program>/uniapi-placement.json:
    // ~/.config on Linux, ~/Library/Application Support on macOS, %AppData% on Windows); empty:
    // as the options say. See Window::placement.
    std::string rememberPlacement;
    // Calls from the page to functions exposed with expose() are checked strictly: more arguments
    // than the function takes, or an object field its parameter's type hasn't (a misspelled
    // name), reject the call instead of being ignored. Fields are checked for types that convert
    // both ways (to_json and from_json, as NLOHMANN_DEFINE_TYPE_* gives), by converting the
    // argument back: a key that doesn't come back is unknown. Handlers that take the JSON
    // themselves ((const json& args, Reply)) and the C API's check what they like.
    bool strictCalls = false;
    // The pages can be inspected with the web engine's developer tools (console, elements,
    // sources, network): every window's. Off by default, for a program in people's hands; on
    // while developing. Linux: WebKit's Web Inspector, "Inspect Element" in the page's
    // right-click menu, always in a window of its own. Windows: WebView2's DevTools, F12 and
    // "Inspect" in the page's menu. macOS: Safari's Develop menu lists the page.
    // Window::openDevTools opens them from the program.
    bool devTools = false;
    // The web engine's own right-click menu in full (Back, Reload, Save as, Print, and Inspect
    // with devTools on). Off: it keeps only its text editing items (Undo, Redo, Cut, Copy, Paste,
    // Paste as plain text, Delete, Select All) where they apply, in text fields and on selected
    // text, and shows nothing elsewhere; for a program in people's hands. The page's own
    // contextmenu handlers work either way (a page can show a menu of its own), and devTools
    // still decides about the developer tools (F12 and the like).
    bool contextMenu = true;
    // Made-up host the pages are served from. Keep a reserved name: on Windows, ".local"
    // names trigger multicast DNS lookups that stall every page load by ~2 seconds.
    std::string host = "uniapi.example";
    // Where the web engine keeps its profile (cache, storage), UTF-8. Empty uses a per-user
    // folder named after the executable: %LOCALAPPDATA%\<exe name> on Windows, WebKitGTK's
    // XDG folders (~/.local/share/<exe name>, ~/.cache/<exe name>) on Linux, WebKit's
    // (~/Library/WebKit/<bundle id or exe name>) on macOS. On macOS the path names a profile of
    // its own (the same path, the same profile) whose files WebKit keeps under its own folder.
    std::string dataDirectory;
    // Windows only: extra command-line switches for the web engine (WebView2 is Chromium),
    // space-separated, e.g. "--proxy-server=host:8080" or "--disable-component-update".
    // Empty: none. WebView2 ignores switches it reserves for itself. They apply when the
    // engine starts for this profile: another app already running with the same
    // dataDirectory keeps the switches it started with. Ignored on Linux and macOS (WebKit has no
    // such switches).
    std::string browserArguments;
    // Windows only: Chromium's way after a click or key, which holds the page's other tasks
    // (C++'s messages among them) until the next frame is drawn, so the input's own response
    // paints sooner. Off by default: C++'s answer to a click (a lit key, a value) reaches the
    // page at once, a frame sooner (measured: a reply drawn 13 ms after the press, not 26).
    // True keeps Chromium's default. Ignored on Linux and macOS (WebKit schedules differently).
    bool deferTasksAfterInput = false;
    // Linux only: WebKit's DMA-BUF renderer, which hands the page's frames from the web process
    // to the window as GPU buffers. False turns it off: the window shows about 0.2 s sooner on
    // some GPUs (NVIDIA: hello at 0.38 s instead of 0.57 s), but every frame is then copied
    // through memory: WebGL runs at about 40 % of its speed and animation costs CPU (a page of
    // animated canvases: 0.67 of a core instead of 0.33; hello, idle: 0.05 instead of 0.01).
    // Worth it only for a page that hardly moves and must show at once. For the whole process,
    // so the first App's choice counts, made before GTK starts (a dialog before run() is too
    // late). It is WebKit's WEBKIT_DISABLE_DMABUF_RENDERER: a value set in the environment wins,
    // and GTK 3 under X11 has the renderer off anyway. Ignored on Windows and macOS.
    bool dmabufRenderer = true;
};

// The outcome of calling a JavaScript function from C++.
struct Result {
    bool ok = false;
    json value;         // the function's return value, when ok
    std::string error;  // the error message otherwise
};

// Binary data, in either direction, outside the JSON (no base64). A shared, read-only buffer:
// cheap to copy, safe to hand to another thread, valid as long as you keep it.
//
// From the page: an ArrayBuffer, typed array, DataView or Blob/File passed to app.native.fn(...),
// app.emit(...) or returned by a JavaScript function, anywhere in the value. Take it as a
// parameter:
//   app.expose("thumbnail", [](uniapi::Bytes image, int width) { ... });
// or from JSON with args[0].get<uniapi::Bytes>(), which works until the call is answered (in an
// event handler or a call's done callback: until it returns).
//
// To the page: put a Bytes in any JSON you send (a return value, Reply::resolve, App::call
// arguments, App::emit data); the page receives a Uint8Array there. Each json(bytes) makes a
// one-time placeholder: send it once, to the current page (a new page can't fetch it).
//   app.expose("thumbnail", [](uniapi::Bytes image, int w) { return uniapi::Bytes(Shrink(image, w)); });
class Bytes {
public:
    Bytes() = default;
    explicit Bytes(std::shared_ptr<const std::vector<uint8_t>> data) : data_(std::move(data)) {}
    explicit Bytes(std::vector<uint8_t> data);  // takes the vector
    Bytes(const void* data, size_t size);       // copies

    const uint8_t* data() const { return data_ ? data_->data() : nullptr; }
    size_t size() const { return data_ ? data_->size() : 0; }
    bool empty() const { return size() == 0; }
    const uint8_t* begin() const { return data(); }
    const uint8_t* end() const { return data() + size(); }
    const std::vector<uint8_t>& vector() const;  // the buffer itself (empty if none)

private:
    friend void to_json(json& value, const Bytes& bytes);
    std::shared_ptr<const std::vector<uint8_t>> data_;
};

// Reads the placeholder the page library puts in place of binary data. Throws if the data is
// no longer available (the call was answered) or the value isn't binary data from the page.
void from_json(const json& value, Bytes& bytes);
// Writes a placeholder the page library turns into a Uint8Array (fetched once, outside the JSON).
void to_json(json& value, const Bytes& bytes);

// Answers one call from JavaScript, now or later, from any thread. The first answer wins;
// answers to a page that has since closed are dropped.
class Reply {
public:
    void resolve(json value = nullptr) const;
    void reject(std::string message) const;
    bool strictCalls() const;  // internal: AppOptions::strictCalls of the app it answers for

    struct State;  // internal
    explicit Reply(std::shared_ptr<State> state) : state_(std::move(state)) {}

private:
    std::shared_ptr<State> state_;
};

// One of the file types a file dialog offers.
struct FileFilter {
    std::string name;     // e.g. "Images"; empty: the pattern is shown
    std::string pattern;  // e.g. "*.png;*.jpg" (several separated by ';'); "*" for every file
};

struct SaveDialogOptions {
    std::string title = "Save";
    std::string defaultName;    // suggested file name
    std::string filterName;     // e.g. "CSV file"
    std::string filterPattern;  // e.g. "*.csv"; several separated by ';'
    // More file types to choose from, listed after filterName/filterPattern's (which comes first
    // and is the one selected), or the whole list when those are empty.
    std::vector<FileFilter> filters;
    // The folder the dialog opens in; empty, or a folder that doesn't exist: the system's choice
    // (usually where it was last).
    std::string directory;
};

struct OpenDialogOptions {
    std::string title = "Open";
    std::string filterName;     // e.g. "Images"
    std::string filterPattern;  // e.g. "*.png;*.jpg"; ignored when picking folders
    // More file types, as SaveDialogOptions'; ignored when picking folders.
    std::vector<FileFilter> filters;
    bool multiple = false;      // let the user pick several
    bool folder = false;        // pick folders instead of files
    std::string directory;      // the folder the dialog opens in, as SaveDialogOptions'
};

class Window;

// A pool of equally sized slots that C++ fills and the page receives in order.
// Create one with App::stream() (or a further window's stream()); addWindow() sends it to more
// windows' pages at once.
class Stream {
public:
    struct Slot {
        int index = -1;
        uint8_t* data = nullptr;
        size_t capacity = 0;
    };

    // Reserves a free slot. Returns false while every slot is with the page (or no page is
    // connected); onSlotFreed then fires when one comes back.
    bool acquire(Slot& slot);
    // Hands the first `size` bytes of an acquired slot to the page, together with `meta`.
    // Returns false (and frees the slot) if no page is connected or `size` is more than the
    // slot holds; false without freeing anything for a slot that isn't acquired.
    bool publish(const Slot& slot, size_t size, const json& meta = nullptr);

    // Fires when the page hands back a slot after acquire() had failed.
    std::function<void()> onSlotFreed;

    // --- More windows ---
    // Sends the stream to another window's page too, under the same name: one set of slots,
    // written once by C++ and read by every page it reaches (with shared memory, the same memory
    // in each page). A slot comes back once every page it went to has handed it back, so the
    // slowest page sets the pace; a page that isn't connected doesn't hold anything up. Throws
    // std::invalid_argument if that window has a stream of this name already (or is another
    // App's). Replacing the stream (stream() with its name again) keeps the windows it reaches.
    void addWindow(Window& window);
    // That window's page gets no more frames (the frames it has, it hands back as usual). Any
    // window, the one that created it too; the stream still belongs to that one.
    void removeWindow(Window& window);
    std::vector<Window*> windows() const;  // the windows it reaches

    const std::string& name() const;
    size_t slotSize() const;
    int slotCount() const;
    bool shared() const;         // true: zero-copy shared memory (for every page it reaches now)
    uint64_t delivered() const;  // slots handed back so far (by every page they went to)

    struct Impl;  // internal
    explicit Stream(std::unique_ptr<Impl> impl);
    ~Stream();
    Impl* internal() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

class App;
namespace detail {
int AddEndingStep(App& app, std::function<void()> step);  // see src/core/ending.hpp
void RemoveEndingStep(App& app, int id);

template <typename T>
struct callable_traits : callable_traits<decltype(&T::operator())> {};
template <typename C, typename R, typename... A>
struct callable_traits<R (C::*)(A...) const> {
    using result = R;
    using args = std::tuple<std::decay_t<A>...>;
    static constexpr bool byValue = (!std::is_reference_v<A> && ...);
};
template <typename C, typename R, typename... A>
struct callable_traits<R (C::*)(A...)> : callable_traits<R (C::*)(A...) const> {};
template <typename R, typename... A>
struct callable_traits<R (*)(A...)> {
    using result = R;
    using args = std::tuple<std::decay_t<A>...>;
    static constexpr bool byValue = (!std::is_reference_v<A> && ...);
};

// A coroutine's return type (uniapi::Task in uniapi/coro.hpp, C++20) specializes this: an
// exposed function returning one answers the call when the coroutine finishes.
template <typename R>
struct task_traits : std::false_type {};

// (const json& args, Reply reply): the form that answers later.
template <typename Args>
struct is_reply_handler : std::false_type {};
template <typename A, typename B>
struct is_reply_handler<std::tuple<A, B>>
    : std::bool_constant<std::is_same_v<A, json> && std::is_same_v<B, Reply>> {};

// keepAlive owns fn; a coroutine (Task) holds on to it until it finishes.
// For AppOptions::strictCalls (app.cpp): whether a value holds binary data from the page (not
// converted back: that would make a download), and the check that every key the page sent came
// back when the decoded argument was converted to JSON again (else std::invalid_argument).
bool containsBytes(const json& value);
void checkKnownFields(const json& given, const json& decoded, size_t index);

// One argument as the parameter's type; its errors say which argument.
template <typename T>
T decodeArgument(const json& args, size_t index, bool strict) {
    try {
        T value = args[index].template get<T>();
        if constexpr (!std::is_same_v<T, json> && !std::is_same_v<T, Bytes> &&
                      nlohmann::detail::has_to_json<json, T>::value) {
            if (strict && args[index].is_structured() && !containsBytes(args[index])) {
                checkKnownFields(args[index], json(value), index);
            }
        }
        return value;
    } catch (const json::exception& e) {
        throw std::invalid_argument("argument " + std::to_string(index + 1) + ": " + e.what());
    }
}

template <typename Args, typename R, typename F, size_t... I>
void invokeWithJson(F& fn, const std::shared_ptr<const void>& keepAlive, const json& args, const Reply& reply,
                    std::index_sequence<I...>) {
    const bool strict = reply.strictCalls();
    const size_t given = args.is_array() ? args.size() : 0;
    if (!args.is_array() || given < sizeof...(I) || (strict && given > sizeof...(I))) {
        throw std::invalid_argument("expected " + std::to_string(sizeof...(I)) + " argument(s), got " +
                                    std::to_string(given));
    }
    // Decoded in order (a braced list is), so the first bad argument is the one reported.
    Args values{decodeArgument<std::tuple_element_t<I, Args>>(args, I, strict)...};
    (void)values;  // a function without parameters
    if constexpr (std::is_void_v<R>) {
        fn(std::get<I>(std::move(values))...);
        reply.resolve();
    } else if constexpr (task_traits<R>::value) {
        task_traits<R>::answer(fn(std::get<I>(std::move(values))...), reply, keepAlive);
    } else {
        reply.resolve(json(fn(std::get<I>(std::move(values))...)));
    }
}
}  // namespace detail

// A further window: its own title, size and web view, and its own page, connected to C++ as the
// main window's is (App::openWindow).
struct WindowOptions {
    std::string title = "uniapi";
    int width = 800;   // the page's area in logical pixels, as AppOptions::width
    int height = 600;
    int minWidth = 0;
    int minHeight = 0;
    int maxWidth = 0;             // as AppOptions::maxWidth, maxHeight
    int maxHeight = 0;
    std::string backgroundColor;  // as AppOptions::backgroundColor
    bool resizable = true;
    bool keepAspectRatio = false;
    TitleBar titleBar = TitleBar::System;
    bool showWhenReady = false;  // as AppOptions::showWhenReady
    bool activate = true;        // takes keyboard focus when it opens
    bool startHidden = false;    // as AppOptions::startHidden
    std::string rememberPlacement;  // as AppOptions::rememberPlacement: a name of its own
    // Owned by the main window: kept above it, minimised with it, closed with it, and opened
    // centred over it. False: a window of its own in the taskbar (still closed with the app).
    bool owned = true;
    // The main window takes no input until this one closes (implies owned), as a dialog.
    bool modal = false;
};

class App;

// A window and its page: what the page and C++ say to each other (calls, events, streams, DOM
// lists) and the window around it. App is the main window; App::openWindow() opens more. Use it
// from the UI thread, like App.
class Window {
public:
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    virtual ~Window();

    // --- Pages ---
    void navigate(std::string_view path);    // e.g. "/mixer.html"; served as the app's files

    // --- JavaScript calls C++ ---
    // Each JS argument becomes one parameter, converted from JSON; the return value goes
    // back as JSON. An exception becomes a rejected promise in JS.
    //   app.expose("add", [](int a, int b) { return a + b; });
    // For work that finishes later (another thread, a dialog), take the arguments as JSON
    // and answer through the Reply:
    //   app.expose("load", [](const uniapi::json& args, uniapi::Reply reply) { ... });
    // A further window's page calls its own window's functions, and the main window's (app's)
    // where it has none of that name; the same for events.
    template <typename F>
    void expose(std::string name, F fn);

    // Takes back a function made callable with expose(): the page's call to it is then rejected as
    // a call to a name never exposed is ("there is no C++ function named ..."), or, from a further
    // window's page, reaches the main window's function of that name again. A call being answered
    // (also later, through its Reply) is still answered.
    void unexpose(std::string_view name);

    // Events the page sends with app.emit(name, data).
    void on(std::string event, std::function<void(const json& data)> handler);
    // Stops handling an event: the page's app.emit(name, data) is then ignored again (a further
    // window's page reaches the main window's handler for it, if there is one).
    void off(std::string_view event);

    // --- C++ calls JavaScript ---
    // Runs a function the page registered with app.expose(); `done` gets its result.
    void call(std::string_view function, json args = json::array(),
              std::function<void(const Result&)> done = {});
    // Sends an event to the page's app.on(name) handlers.
    void emit(std::string_view event, json data = nullptr);
    // call() in a coroutine (C++20; include <uniapi/coro.hpp>):
    //   uniapi::Result r = co_await app.callAsync("confirm", {"Delete?"});
    class CallAwaiter;
    CallAwaiter callAsync(std::string function, json args = json::array());

    // --- Large data ---
    // Creates or replaces a stream. The page receives its slots with app.stream(name, handler).
    // Replacing destroys the old Stream: references to it, and slots acquired from it, are no
    // longer valid; use the returned one. The slots are allocated once the web view exists: if
    // that memory can't be had, stream() throws std::bad_alloc (or, for a stream created before
    // the web view, the failure is printed to stderr and its acquire() returns false).
    Stream& stream(const std::string& name, size_t slotSize, int slotCount);
    // Sends one DOM command list (see dom.hpp). Returns false if no page is connected or the
    // previous lists haven't been applied yet.
    bool sendDom(const std::function<void(DomCommandWriter&)>& write);

    // --- Connection ---
    // A page called uniapi.connect(). It fires before any call or event from that page reaches
    // C++ (the page holds them until then), so handlers can always call and emit back.
    std::function<void(const std::string& page)> onConnect;
    std::function<void(const std::string& page)> onDisconnect;  // that page went away
    bool connected() const;
    const std::string& page() const;
    // Whether streams use shared memory with the current page (a page that didn't receive its
    // shared regions falls back to fetching; each new page tries again).
    bool sharedMemory() const;

    // --- The window ---
    void setTitle(std::string_view title);
    // Full screen on or off, e.g. from a page button or F11; fullscreen() is the current state.
    void setFullscreen(bool on);
    // Resizes the page area to width × height logical pixels (like AppOptions::width/height),
    // keeping the window's position; with keepAspectRatio, resizing keeps the new shape. Not
    // while in full screen. Before the window exists, it sets the size it opens with.
    void setSize(int width, int height);
    bool fullscreen() const;
    // The title bar and frame dark, light, or following the system (e.g. to match a page's
    // theme); before the window exists, how it opens.
    void setTitleBar(TitleBar mode);
    TitleBar titleBar() const;
    // Brings the window to the front: shown again if hidden (hide(), startHidden), restored if
    // minimised, with keyboard focus unless its options turned activate off. A window kept
    // hidden for its page (showWhenReady) appears. Before run(): the window opens shown.
    void show();
    // Takes the window off the screen (and the taskbar or Window menu) without closing it: its
    // page stays connected and keeps running; show() brings it back. Before run(): the window
    // opens hidden, as with startHidden. Only this window hides: further windows stay as they
    // are. On macOS, clicking the app in the Dock shows a hidden main window again when no
    // window is visible.
    void hide();
    // Whether the window is shown: false once hidden (hide(), startHidden) or closed. A
    // minimised window, or one covered by others, counts as visible.
    bool visible() const;
    // The window icon from PNG bytes, e.g. from your embedded files:
    //   if (const uniapi::Asset* icon = my_web.find("icon.png")) app.setIcon(icon->data, icon->size);
    // Returns false if the bytes aren't a PNG the platform can read, or the platform takes
    // icons only from elsewhere (GTK 4 and Wayland use the app's .desktop file). macOS: the
    // main window's sets the app's icon in the Dock (Mac windows have none); further windows'
    // return false.
    bool setIcon(const unsigned char* png, size_t size);
    // Closes it, as its close button does but without asking onCloseRequested: for the main
    // window, the app (as App::quit()).
    void close();
    // Opens the web engine's developer tools on the window's page, in a window of their own:
    // WebKit's Web Inspector on Linux, WebView2's DevTools on Windows. It needs
    // AppOptions::devTools; without it nothing happens. On macOS nothing opens either way: with
    // the option the page is inspected from Safari's Develop menu (Safari > Settings > Advanced >
    // Show features for web developers). Before the window has its page nothing happens: call it
    // in a handler or from onConnect on.
    void openDevTools();
    // The user asks the window to close (reason: CloseReason::Window, or on macOS
    // CloseReason::Quit for the main window). Return true to let it close, false to keep it
    // open: to hide() it instead (an app that keeps running, e.g. in the tray), or to ask first
    // (unsaved changes) and close() once answered. Not asked for close(), App::quit(), the App
    // ending, or a logout or shutdown; empty, or an exception from it, lets the window close.
    std::function<bool(CloseReason reason)> onCloseRequested;
    // The window closed (its close button, close(), or the app ending), after its page went away.
    // A further window is destroyed once this has returned: drop every reference to it here.
    std::function<void()> onClosed;
    // --- Placement ---
    // Positions are the window frame's top-left corner, sizes the page area's, in logical pixels
    // from the primary screen's top-left corner. Linux: Wayland doesn't let apps place their
    // windows or know where they are, and GTK 4 can't on X11 either: there position() is empty,
    // setPosition() and center() do nothing, always on top isn't available, and a placement keeps
    // size and maximised only (everything works with GTK 3 on X11).
    Size size() const;                      // the page area now
    std::optional<Point> position() const;  // empty where the platform doesn't say
    // The desktop's screens, the one this window is on marked current (none before the window
    // exists). Every platform, Wayland and GTK 4 too; on Linux none before run().
    std::vector<Screen> screens() const;
    // Moves the frame's top-left corner there; before the window opens, where it opens.
    void setPosition(int x, int y);
    void center();  // on its screen (macOS: a little above the middle, as Mac apps open)
    // Minimised (to the Dock, the taskbar), as large as its screen allows (macOS: zoomed), or
    // back to its normal size and place; full screen is setFullscreen's. A hidden window (or one
    // not yet open) takes the state when shown.
    void minimize();
    void maximize();
    void restore();
    bool minimized() const;
    bool maximized() const;
    // Above other apps' windows (a tool panel, a meter), or not. False where the platform can't
    // (Linux: GTK 4, Wayland).
    bool setAlwaysOnTop(bool on);
    bool alwaysOnTop() const;
    // Where the window is and how (its normal position and size, maximised or not), as text to
    // keep (in the app's settings) and give setPlacement() later; empty if it isn't known yet.
    // rememberPlacement does that by itself.
    std::string placement() const;
    // Puts the window back as placement() found it: on a screen that is still there (a position
    // off every screen now, a laptop undocked, gives a centred window instead), no larger than
    // that screen, maximised if it was. Before the window opens: how it opens. False only for text
    // placement() didn't make.
    bool setPlacement(std::string_view placement);
    // The window moved, was resized, minimised, maximised or restored (by the person or the app;
    // while dragged, often).
    std::function<void()> onPlacementChanged;

    // Files dropped on the window (from the Finder, Explorer, a file manager): their paths (UTF-8),
    // and where, in the page's CSS pixels. The page gets its own drop event too (with File
    // objects, which have no paths), except on GTK 4, which gives the files to one of the two. A
    // dropped file never replaces the page.
    std::function<void(const std::vector<std::string>& paths, int x, int y)> onFileDrop;

    App& app() const;

    using Handler = std::function<void(const json& args, const Reply& reply)>;
    void exposeHandler(std::string name, Handler handler);  // what expose() builds on

    struct Impl;  // internal
    Impl* internal() const { return impl_.get(); }

protected:
    explicit Window(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class App;
};

// The program and its main window: the files its pages are served from, timers, posting from
// other threads, dialogs, the event loop, and further windows.
class App : public Window {
public:
    explicit App(AppOptions options = {});
    ~App() override;

    // --- Pages (for every window) ---
    void serve(const AssetTable& assets);    // files compiled in with uniapi_embed()
    void serveDirectory(std::string path);   // or files read from disk, handy while editing

    // --- Further windows ---
    // Opens a window with its own page (navigate() it), connected to C++ as the main window's:
    // expose, on, call, emit, streams and its own onConnect. Before run(), it opens with the
    // main window. Throws std::runtime_error where the platform can't open more windows yet.
    // The App owns it: it lives until it has closed (see Window::onClosed), or the App ends.
    Window& openWindow(WindowOptions options = {});
    // The further windows open now (not the main window).
    std::vector<Window*> windows() const;
    // The window whose page sent the call or event being handled now: inside an exposed
    // function or an on() handler while it runs (a further window's page reaching the app's
    // handlers is that window, not the App); nullptr anywhere else. A Reply answered later
    // doesn't need it: it goes back to the page that called.
    Window* sender() const;

    // --- Coroutines (C++20; include <uniapi/coro.hpp> for uniapi::Task) ---
    // Inside a coroutine on the UI thread:
    //   co_await app.sleep(500ms);       // waits without blocking the window
    //   co_await app.switchToUi();       // continues on the UI thread (e.g. after background work)
    class SleepAwaiter;
    class UiAwaiter;
    SleepAwaiter sleep(std::chrono::milliseconds duration);
    UiAwaiter switchToUi();

    // --- Event loop ---
    // Calls `fn` on the UI thread once right away, then every `interval`, with high-resolution
    // timing (an interval under 1 ms counts as 1 ms). Returns the timer's id for cancel(), or
    // 0 if no timer could be created (Windows allows 63 at once).
    int every(std::chrono::milliseconds interval, std::function<void()> fn);
    void cancel(int timer);
    // Runs `fn` soon on the UI thread, outside any WebView event. Safe from any thread.
    // An exception escaping a timer or posted function is printed to stderr; the timer keeps
    // running.
    void post(std::function<void()> fn);
    // Whether the caller is on the UI thread (the App's), where every method works. Any thread.
    bool onUiThread() const;
    // The web engine the app's pages run in and its version, for an About box or a bug report:
    // "WebKitGTK" 2.50.1 on Linux (the library loaded at run time, which may be newer than the
    // one built against), "WebView2" 131.0.2903.86 on Windows (the installed runtime's, so the
    // Chromium version), "WebKit" 621.1.15 on macOS (the system's, which WKWebView is). On
    // Windows the version is known once run() has started WebView2 (in any handler, and from
    // onConnect on); before that it is "". UI thread only.
    WebEngine webEngine() const;
    // Modal; returns the chosen path (UTF-8), or nothing if cancelled.
    std::optional<std::string> saveFileDialog(const SaveDialogOptions& options);
    // Modal; returns the chosen paths (UTF-8), or none if cancelled.
    std::vector<std::string> openFileDialog(const OpenDialogOptions& options = {});
    int run();  // shows the window (unless startHidden) and returns when it is closed
    void quit();

    // --- The desktop ---
    // The page tried to leave the app's pages for `url` (a link, a form or window.open to another
    // site, mailto:...). The navigation is cancelled, and no popup window ever opens; return true
    // to open url with the system (openURL: the default browser, the mail app...). Empty: it
    // opens. A file: URL (a file dropped on a page that doesn't take it) is refused without
    // asking. Iframes load what they like.
    std::function<bool(const std::string& url)> onExternalURL;
    // The app was started again while it runs (AppOptions::singleInstance): that launch's
    // arguments (without the program) and working folder. Empty: the main window is shown.
    std::function<void(const std::vector<std::string>& arguments, const std::string& workingDirectory)>
        onSecondInstance;
    // The clipboard's text: `done` gets it, or nothing when the clipboard holds no text. On the
    // UI thread, always after clipboardText() has returned (GTK 4 reads the clipboard
    // asynchronously). Not before run() on Linux (no display yet: nothing).
    void clipboardText(std::function<void(std::optional<std::string> text)> done);
    // Puts text on the system clipboard in place of what was there, for other apps to paste.
    // False if the system refused it (on Windows and Linux, also before run()). On Wayland the
    // compositor may ignore an app the person hasn't just used (a click or key in it).
    bool setClipboardText(std::string_view text);
    // Shows a system notification and returns its id. Safe from any thread. `done` (may be empty)
    // gets ok once the system has taken it, or why it couldn't (Result::error), and its id
    // (Result::value) either way, on the UI thread, always after notify() has returned; without
    // it a failure is printed to stderr.
    // Before run(), it waits for run(). A click on it calls onNotificationClicked; the person may
    // also never see it (do not disturb, notifications turned off for the app).
    //   - Linux: the desktop's notification server (org.freedesktop.Notifications over D-Bus); its
    //     name and icon come from the app's .desktop file (uniapi_install()).
    //   - macOS: Notification Center, from an .app bundle only (uniapi_install()): a plain
    //     executable gets an error. The first notification asks the person to allow them; those
    //     sent meanwhile wait for the answer.
    //   - Windows: a notification-area balloon, which Windows 10 and 11 show as a notification;
    //     the program's icon is in the notification area while it is shown.
    std::string notify(Notification notification, std::function<void(const Result& result)> done = {});
    // notify() in a coroutine: co_await app.notifyAsync({"Export done", "mix.wav is ready."}).
    class NotifyAwaiter;
    NotifyAwaiter notifyAsync(Notification notification);
    // The person clicked a notification (notify), named by its id; it may be one an earlier run
    // sent (macOS keeps them). Empty: the main window is shown.
    std::function<void(const std::string& id)> onNotificationClicked;
    // clipboardText() in a coroutine (C++20; include <uniapi/coro.hpp>):
    //   std::optional<std::string> text = co_await app.clipboardTextAsync();
    class ClipboardAwaiter;
    ClipboardAwaiter clipboardTextAsync();

    struct Impl;  // internal

private:
    friend int detail::AddEndingStep(App& app, std::function<void()> step);
    friend void detail::RemoveEndingStep(App& app, int id);
    std::shared_ptr<Impl> app_;
};

// The awaitables behind callAsync(), sleep() and switchToUi(). await_suspend is a template so
// this header stays C++17; only code that co_awaits them needs C++20.
class Window::CallAwaiter {
public:
    CallAwaiter(Window& window, std::string function, json args)
        : window_(&window), function_(std::move(function)), args_(std::move(args)) {}
    bool await_ready() const noexcept { return false; }
    template <typename Handle>
    void await_suspend(Handle handle) {
        // done may run (and resume the coroutine, ending this awaiter) inside call() itself,
        // so call() gets its own copies.
        std::string function = std::move(function_);
        window_->call(function, std::move(args_), [this, handle](const Result& result) mutable {
            result_ = result;
            handle.resume();
        });
    }
    Result await_resume() { return std::move(result_); }

private:
    Window* window_;
    std::string function_;
    json args_;
    Result result_;
};

class App::ClipboardAwaiter {
public:
    explicit ClipboardAwaiter(App& app) : app_(&app) {}
    bool await_ready() const noexcept { return false; }
    template <typename Handle>
    void await_suspend(Handle handle) {
        app_->clipboardText([this, handle](std::optional<std::string> text) mutable {
            text_ = std::move(text);
            handle.resume();
        });
    }
    std::optional<std::string> await_resume() { return std::move(text_); }

private:
    App* app_;
    std::optional<std::string> text_;
};

class App::NotifyAwaiter {
public:
    NotifyAwaiter(App& app, Notification notification) : app_(&app), notification_(std::move(notification)) {}
    bool await_ready() const noexcept { return false; }
    template <typename Handle>
    void await_suspend(Handle handle) {
        app_->notify(std::move(notification_), [this, handle](const Result& result) mutable {
            result_ = result;
            handle.resume();
        });
    }
    Result await_resume() { return std::move(result_); }

private:
    App* app_;
    Notification notification_;
    Result result_;
};

class App::SleepAwaiter {
public:
    SleepAwaiter(App& app, std::chrono::milliseconds duration) : app_(&app), duration_(duration) {}
    bool await_ready() const noexcept { return false; }
    template <typename Handle>
    void await_suspend(Handle handle) {
        App* app = app_;
        if (duration_.count() <= 0) {
            app->post([handle]() mutable { handle.resume(); });
            return;
        }
        // every() ticks once at once, then each interval: resume on the second tick.
        auto timer = std::make_shared<int>(0);
        auto ticks = std::make_shared<int>(0);
        *timer = app->every(duration_, [app, timer, ticks, handle]() mutable {
            if (++*ticks < 2) return;
            app->cancel(*timer);
            handle.resume();
        });
        // No timer to be had (Windows allows 63 at once): fail the co_await instead of hanging.
        // The coroutine resumes with this exception.
        if (*timer == 0) throw std::runtime_error("uniapi: no timer available for sleep()");
    }
    void await_resume() const noexcept {}

private:
    App* app_;
    std::chrono::milliseconds duration_;
};

class App::UiAwaiter {
public:
    explicit UiAwaiter(App& app) : app_(&app) {}
    bool await_ready() const noexcept { return false; }
    template <typename Handle>
    void await_suspend(Handle handle) {
        app_->post([handle]() mutable { handle.resume(); });  // post() works from any thread
    }
    void await_resume() const noexcept {}

private:
    App* app_;
};

inline Window::CallAwaiter Window::callAsync(std::string function, json args) {
    return CallAwaiter(*this, std::move(function), std::move(args));
}
inline App::SleepAwaiter App::sleep(std::chrono::milliseconds duration) { return SleepAwaiter(*this, duration); }
inline App::UiAwaiter App::switchToUi() { return UiAwaiter(*this); }
inline App::ClipboardAwaiter App::clipboardTextAsync() { return ClipboardAwaiter(*this); }
inline App::NotifyAwaiter App::notifyAsync(Notification notification) {
    return NotifyAwaiter(*this, std::move(notification));
}

template <typename F>
void Window::expose(std::string name, F fn) {
    using Traits = detail::callable_traits<std::decay_t<F>>;
    using Args = typename Traits::args;
    using R = typename Traits::result;
    static_assert(!detail::task_traits<R>::value || Traits::byValue,
                  "a coroutine exposed with expose() must take its parameters by value: a reference "
                  "would point at a temporary that is gone once the coroutine suspends");
    if constexpr (detail::is_reply_handler<Args>::value) {
        exposeHandler(std::move(name), Handler(std::move(fn)));
    } else {
        // The function object lives in a shared_ptr: each call holds it while it runs, and a
        // coroutine until it finishes, even if the name is exposed again meanwhile.
        auto shared = std::make_shared<std::decay_t<F>>(std::move(fn));
        exposeHandler(std::move(name), [shared](const json& args, const Reply& reply) {
            detail::invokeWithJson<Args, R>(*shared, shared, args, reply,
                                            std::make_index_sequence<std::tuple_size_v<Args>>{});
        });
    }
}

// --- Small platform helpers ---
std::string executablePath();  // UTF-8
// The program's arguments (without the program itself), UTF-8 on every platform. Prefer this
// to main()'s argv on Windows, where argv can't hold every character a path may contain.
std::vector<std::string> arguments();
std::string commandLine();     // arguments() joined with spaces
void alert(std::string_view title, std::string_view message);  // modal message box
// Open things with the system, from any thread; each throws std::runtime_error saying why when
// it can't. openURL: a URL with a scheme, as the system opens it (a web page in the default
// browser, mailto: in the mail app...). openPath: a file with its default app, a folder in the
// file manager. revealPath: the file manager showing a file or folder, selected where it can
// (Finder, Explorer; on Linux through org.freedesktop.FileManager1, else its folder opens).
// Paths are UTF-8, relative ones to the working folder, and must exist.
void openURL(std::string_view url);
void openPath(std::string_view path);
void revealPath(std::string_view path);

}  // namespace uniapi
