// The lifecycle test: windows that are hidden and shown again, and closing that the app decides
// (onCloseRequested). The main window starts hidden (startHidden) and its page works while it
// is; hide() and show() take windows off the screen and back with their pages running; a page
// that says it is drawn (app.show()) doesn't show a window C++ keeps hidden; the user closing a
// window asks onCloseRequested, which can keep it open, hide it instead, or ask the page and
// close() once it answers; close() and quit() don't ask. On macOS also Quit (Cmd+Q) asking the
// main window. And the developer tools (devTools on here): openDevTools opening them in a window
// of their own, seen on Linux. Exits with 0 when every check passes.
//
//   uniapi_lifecycle_test          keeps the main window open with the results, to try by hand
//   uniapi_lifecycle_test --exit   closes when done, without taking focus (ctest)

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

UNIAPI_ASSETS(lifecycle_web);

// The platform's windows, found by title (lifecycle/linux.cpp, lifecycle/mac.mm; Windows below).
// OnScreen: 1 shown, 0 not, -1 no such window. CloseAsUser: as its close button (asking
// onCloseRequested); false if there is none. QuitAsUser: as Cmd+Q; false where there is no
// app-wide Quit. TakesInput: 1 the window takes input, 0 a modal window holds it, -1 can't tell.
int OnScreen(const std::string& title);
bool CloseAsUser(const std::string& title);
bool QuitAsUser();
int TakesInput(const std::string& title);
// DevToolsWindows: how many developer tools windows are shown, -1 if the test can't tell (they
// are another process's on Windows; macOS opens none); close: closes them.
int DevToolsWindows(bool close);

#ifdef _WIN32
namespace {
HWND Find(const std::string& title) {
    return FindWindowW(L"uniapi.window", std::wstring(title.begin(), title.end()).c_str());
}
}  // namespace
int OnScreen(const std::string& title) {
    HWND window = Find(title);
    return window ? (IsWindowVisible(window) ? 1 : 0) : -1;
}
bool CloseAsUser(const std::string& title) {
    HWND window = Find(title);
    return window && PostMessageW(window, WM_CLOSE, 0, 0);  // as the close button and Alt+F4
}
bool QuitAsUser() { return false; }
int DevToolsWindows(bool) { return -1; }  // WebView2's DevTools are its browser process's window
int TakesInput(const std::string& title) {
    HWND window = Find(title);
    return window ? (IsWindowEnabled(window) ? 1 : 0) : -1;
}
#endif

namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr char kTitle[] = "uniapi lifecycle test";

struct Checks {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> lines;

    void Check(const std::string& name, bool ok, const std::string& detail = {}) {
        ok ? ++passed : ++failed;
        std::string line = std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok || detail.empty() ? "" : ": " + detail);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        lines.push_back(std::move(line));
    }
    void NotApplicable(const std::string& name, const std::string& why) {
        std::string line = "  n/a   " + name + " (" + why + ")";
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        lines.push_back(std::move(line));
    }
};

// Waits (without blocking the window) until `done` is true, or the timeout: false then.
uniapi::Task<bool> WaitFor(uniapi::App& app, std::function<bool()> done, std::chrono::milliseconds timeout = 10s) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - start > timeout) co_return false;
        co_await app.sleep(10ms);
    }
    co_return true;
}

// Calls the page's echo: its page works.
uniapi::Task<bool> Echo(uniapi::Window& window, int value) {
    uniapi::Result result = co_await window.callAsync("echo", {value});
    co_return result.ok && result.value == json(value);
}

// visible() and, where the platform lets the test look, whether the window is on screen: both
// must agree with `want`.
bool Shown(const std::string& title, bool visible, bool want, std::string& detail) {
    const int on = OnScreen(title);
    detail = std::string("visible ") + (visible ? "true" : "false");
    if (on >= 0) detail += std::string(", on screen ") + (on ? "true" : "false");
    return visible == want && (on < 0 || (on == 1) == want);
}

const char* ReasonName(uniapi::CloseReason reason) { return reason == uniapi::CloseReason::Quit ? "Quit" : "Window"; }

// A further window and what the test saw of it.
struct Watched {
    uniapi::Window* window = nullptr;
    std::string title;
    int connects = 0;
    bool closed = false;
    std::vector<uniapi::CloseReason> asked;

    void Open(uniapi::App& app, const std::string& name, bool activate,
              const std::function<void(uniapi::WindowOptions&)>& change = {}) {
        uniapi::WindowOptions options;
        options.title = std::string(kTitle) + ": " + name;
        options.width = 420;
        options.height = 200;
        options.activate = activate;
        if (change) change(options);
        window = &app.openWindow(options);
        title = options.title;
        window->onConnect = [this](const std::string&) { ++connects; };
        window->onClosed = [this] {
            closed = true;
            window = nullptr;  // destroyed once this returns
        };
        window->navigate("/other.html?" + name);
    }
};

}  // namespace

int main() {
    bool keepOpen = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--exit") {
            keepOpen = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_lifecycle_test [--exit]\n", argument.c_str());
            return 2;
        }
    }

    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.activate = keepOpen;
    options.startHidden = true;
    options.devTools = true;
    uniapi::App app(options);
    app.serve(lifecycle_web);
    Checks checks;

    int mainAsked = 0;  // the main window's onCloseRequested, once the flow has set one
    int askedBeforeQuit = -1;
    bool byHand = false;
    bool started = false;

    // A window opened before run() and hidden before run(): it opens hidden.
    Watched early;
    early.Open(app, "early", keepOpen);
    early.window->hide();
    const bool hiddenBeforeRun = !early.window->visible();

    Watched ready, asks, confirms, modal;
    bool showed = false;
    bool hideOnAsk = false;
    std::vector<uniapi::CloseReason> reasons;

    auto flow = [&]() -> uniapi::Task<> {
        auto settle = [&] { return app.sleep(300ms); };  // a moment for the window system
        std::string detail;

        // 1. Started hidden: the main window's page connected (that started this) and works.
        bool ok = Shown(kTitle, app.visible(), false, detail);
        bool echoed = co_await Echo(app, 1);
        checks.Check("startHidden: the main window opens hidden, and its page connects and answers", ok && echoed, detail);
        bool connected = co_await WaitFor(app, [&] { return early.connects > 0; });
        ok = Shown(early.title, early.window && early.window->visible(), false, detail);
        checks.Check("hide() before run(): a further window opens hidden; its page connects",
                     hiddenBeforeRun && connected && ok, detail);

        // 2. show(), hide(), show(): on and off the screen, the page connected and working throughout.
        app.show();
        co_await settle();
        ok = Shown(kTitle, app.visible(), true, detail);
        echoed = co_await Echo(app, 2);
        checks.Check("show(): a window started hidden appears; its page still answers", ok && echoed, detail);
        app.hide();
        co_await settle();
        ok = Shown(kTitle, app.visible(), false, detail);
        echoed = co_await Echo(app, 3);
        checks.Check("hide(): the window leaves the screen; its page stays connected and answers",
                     ok && app.connected() && echoed, detail);
        app.show();
        co_await settle();
        ok = Shown(kTitle, app.visible(), true, detail);
        checks.Check("show() again: the window is back", ok, detail);

        // 3. The early window: shown, then closed by close(), which doesn't ask.
        early.window->onCloseRequested = [&](uniapi::CloseReason reason) {
            early.asked.push_back(reason);
            return false;
        };
        early.window->show();
        co_await settle();
        ok = Shown(early.title, early.window->visible(), true, detail);
        checks.Check("show(): a further window that opened hidden appears", ok, detail);
        early.window->close();
        bool closed = co_await WaitFor(app, [&] { return early.closed; });
        checks.Check("close() closes without asking onCloseRequested; onClosed", closed && early.asked.empty(),
                     "closed " + std::to_string(closed) + ", asked " + std::to_string(early.asked.size()));

        // 4. A showWhenReady page saying it is drawn (app.show()) doesn't show a hidden window.
        ready.Open(app, "ready", keepOpen, [](uniapi::WindowOptions& o) {
            o.startHidden = true;
            o.showWhenReady = true;
        });
        ready.window->on("showed", [&](const json&) { showed = true; });
        ok = co_await WaitFor(app, [&] { return showed; });
        co_await settle();
        bool stillHidden = Shown(ready.title, ready.window->visible(), false, detail);
        checks.Check("a hidden window's page calling app.show() leaves it hidden", ok && stillHidden, detail);
        ready.window->show();
        co_await settle();
        ok = Shown(ready.title, ready.window->visible(), true, detail);
        checks.Check("show() then shows it (its page drawn)", ok, detail);
        ready.window->close();
        co_await WaitFor(app, [&] { return ready.closed; });

        // 5. The user closes a window: onCloseRequested keeps it open, then hides it instead.
        asks.Open(app, "asks", keepOpen);
        asks.window->onCloseRequested = [&](uniapi::CloseReason reason) {
            asks.asked.push_back(reason);
            if (hideOnAsk) asks.window->hide();
            return false;
        };
        co_await WaitFor(app, [&] { return asks.connects > 0; });
        co_await settle();
        const bool clicked = CloseAsUser(asks.title);
        if (!clicked) {
            checks.Check("closed by the user: this platform's test can't close a window as the user does", false);
        } else {
            ok = co_await WaitFor(app, [&] { return asks.asked.size() == 1; });
            co_await settle();
            bool open = Shown(asks.title, asks.window && asks.window->visible(), true, detail);
            checks.Check("closed by the user: onCloseRequested is asked (CloseReason::Window); false keeps the window open",
                         ok && asks.asked[0] == uniapi::CloseReason::Window && !asks.closed && open, detail);
            hideOnAsk = true;
            CloseAsUser(asks.title);
            ok = co_await WaitFor(app, [&] { return asks.asked.size() == 2; });
            co_await settle();
            bool hidden = Shown(asks.title, asks.window && asks.window->visible(), false, detail);
            checks.Check("onCloseRequested hiding the window instead: off screen, not closed", ok && !asks.closed && hidden,
                         detail);
            echoed = false;
            if (asks.window) echoed = co_await Echo(*asks.window, 4);
            checks.Check("...and its page still answers", echoed);
        }
        if (asks.window) asks.window->close();
        closed = co_await WaitFor(app, [&] { return asks.closed; });
        const size_t wantAsked = clicked ? 2 : 0;
        checks.Check("close() then closes it without asking", closed && asks.asked.size() == wantAsked,
                     "asked " + std::to_string(asks.asked.size()));

        // 6. "Close this window?": onCloseRequested asks the page, and close() follows its answer.
        if (clicked) {
            confirms.Open(app, "confirms", keepOpen);
            confirms.window->onCloseRequested = [&](uniapi::CloseReason reason) {
                confirms.asked.push_back(reason);
                uniapi::Window* window = confirms.window;
                window->call("confirmClose", json::array(), [window](const uniapi::Result& r) {
                    if (r.ok && r.value == json(true)) window->close();
                });
                return false;
            };
            co_await WaitFor(app, [&] { return confirms.connects > 0; });
            CloseAsUser(confirms.title);
            closed = co_await WaitFor(app, [&] { return confirms.closed; });
            checks.Check("onCloseRequested asks the page; the window closes once it answers yes (asked once)",
                         closed && confirms.asked.size() == 1,
                         "closed " + std::to_string(closed) + ", asked " + std::to_string(confirms.asked.size()));
        }

        // 7. A modal window holds the main window only while it is shown (macOS and Windows).
        if (TakesInput(kTitle) >= 0) {
            modal.Open(app, "modal", keepOpen, [](uniapi::WindowOptions& o) {
                o.modal = true;
                o.startHidden = true;
            });
            co_await WaitFor(app, [&] { return modal.connects > 0; });
            const int whileHidden = TakesInput(kTitle);
            modal.window->show();
            co_await settle();
            const int whileShown = TakesInput(kTitle);
            modal.window->hide();
            co_await settle();
            const int hiddenAgain = TakesInput(kTitle);
            modal.window->close();
            co_await WaitFor(app, [&] { return modal.closed; });
            checks.Check("a modal window holds the main window only while shown (started hidden, shown, hidden)",
                         whileHidden == 1 && whileShown == 0 && hiddenAgain == 1,
                         "main takes input: " + std::to_string(whileHidden) + " " + std::to_string(whileShown) + " "
                             + std::to_string(hiddenAgain));
        }

        // 8. macOS: Quit (Cmd+Q) asks the main window (CloseReason::Quit); false keeps the app running.
        app.onCloseRequested = [&](uniapi::CloseReason reason) {
            ++mainAsked;
            reasons.push_back(reason);
            return false;
        };
        if (QuitAsUser()) {
            ok = co_await WaitFor(app, [&] { return reasons.size() == 1; });
            co_await settle();
            checks.Check("macOS Quit (Cmd+Q) asks the main window's onCloseRequested (CloseReason::Quit); false keeps "
                         "the app running",
                         ok && reasons[0] == uniapi::CloseReason::Quit && app.connected() && app.visible(),
                         reasons.empty() ? "not asked" : ReasonName(reasons[0]));
        }

        // 9. Developer tools (AppOptions::devTools on here): openDevTools opens them in a window
        // of their own (Linux: the Web Inspector, detached).
        if (DevToolsWindows(false) >= 0) {
            const int before = DevToolsWindows(false);
            app.openDevTools();
            ok = co_await WaitFor(app, [&] { return DevToolsWindows(false) > before; });
            checks.Check("openDevTools opens the developer tools in a window of their own (devTools on)", ok,
                         std::to_string(DevToolsWindows(false)) + " window(s)");
            DevToolsWindows(true);
            co_await settle();
        } else {
            checks.NotApplicable("openDevTools opens the developer tools", "the test can't see them here; try the checker");
        }

        std::printf("\nlifecycle: %d passed, %d failed (and one after the app ends)\n", checks.passed, checks.failed);
        std::fflush(stdout);
        if (!keepOpen) {
            askedBeforeQuit = mainAsked;
            app.quit();  // doesn't ask either: checked after run()
            co_return;
        }
        // By hand: on macOS closing the main window hides it, as Mac apps that keep running do
        // (click the Dock icon to bring it back; Cmd+Q quits). Elsewhere it closes.
        byHand = true;
#ifdef __APPLE__
        const bool mac = true;
        const char* note = "Closing the window hides it: click the Dock icon to bring it back; Cmd+Q quits.";
#else
        const bool mac = false;
        const char* note = "Closing the window ends the test.";
#endif
        app.onCloseRequested = [&app, mac](uniapi::CloseReason reason) {
            if (mac && reason == uniapi::CloseReason::Window) {
                app.hide();
                return false;
            }
            return true;
        };
        app.call("showResults", json::array({checks.lines, note}));
    };

    // By hand: hidden for 2 seconds, then back.
    auto hideBriefly = [&]() -> uniapi::Task<> {
        app.hide();
        co_await app.sleep(2s);
        app.show();
    };
    app.on("hideBriefly", [&](const json&) { hideBriefly(); });
    app.on("quit", [&](const json&) { app.quit(); });

    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();  // a Task runs at once, to its first co_await
    };
    // A page that never connects must not hang the test.
    int ticks = 0;
    app.every(60s, [&] {
        if (ticks++ == 0 || byHand) return;  // the first call comes at once
        checks.Check("finished within 60 s", false);
        app.quit();
    });
    app.navigate("/index.html");
    app.run();

    if (!byHand) {
        checks.Check("quit() ends the app without asking onCloseRequested",
                     askedBeforeQuit >= 0 && mainAsked == askedBeforeQuit,
                     "asked " + std::to_string(mainAsked - askedBeforeQuit) + " more time(s)");
    }
    std::printf("\nlifecycle: %d passed, %d failed\n", checks.passed, checks.failed);
    return checks.failed == 0 && started ? 0 : 1;
}
