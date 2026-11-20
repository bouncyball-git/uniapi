// The placement test: where a window is and how. Its size read back; moved (setPosition, read
// back with position) and centred; minimised and restored; maximised, its normal size kept, and
// restored to it; kept on top; onPlacementChanged told of each; a placement put back with
// setPlacement, and one off every screen landing centred instead; a window's placement
// remembered between openings (rememberPlacement). Where the platform can't place windows
// (Linux: Wayland, GTK 4), the checks that need a position say so and are left out. Exits with 0
// when every check passes.
//
//   uniapi_placement_test          keeps the window open with the results
//   uniapi_placement_test --exit   ends when done, without taking focus (ctest)

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

UNIAPI_ASSETS(placement_web);

namespace {

using uniapi::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr char kTitle[] = "uniapi placement test";
constexpr char kRememberAs[] = "uniapi placement test window";
constexpr char kMainRememberAs[] = "uniapi placement test main window";

struct Checks {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> lines;

    void Check(const std::string& name, bool ok, const std::string& detail = {}) {
        ok ? ++passed : ++failed;
        Print(std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok || detail.empty() ? "" : ": " + detail));
    }
    void NotApplicable(const std::string& name, const std::string& why) { Print("  n/a   " + name + " (" + why + ")"); }
    void Print(std::string line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        lines.push_back(std::move(line));
    }
};

uniapi::Task<bool> WaitFor(uniapi::App& app, std::function<bool()> done, std::chrono::milliseconds timeout = 10s) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - start > timeout) co_return false;
        co_await app.sleep(20ms);
    }
    co_return true;
}

bool Near(int a, int b) { return a - b <= 2 && b - a <= 2; }  // a pixel of rounding either way

std::string Text(uniapi::Size size) { return std::to_string(size.width) + "x" + std::to_string(size.height); }
std::string Text(const std::optional<uniapi::Point>& point) {
    return point ? std::to_string(point->x) + "," + std::to_string(point->y) : "no position";
}

// Where rememberPlacement keeps this program's placements (to remove after): as the library
// finds it.
fs::path PlacementFile() {
    fs::path config;
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA")) config = appData;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) config = fs::path(home) / "Library" / "Application Support";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg == '/') {
        config = xdg;
    } else if (const char* home = std::getenv("HOME")) {
        config = fs::path(home) / ".config";
    }
#endif
    const std::string exe = uniapi::executablePath();  // UTF-8
    fs::path program = fs::path(std::u8string(exe.begin(), exe.end())).filename();
    if (program.extension() == ".exe") program.replace_extension();
    return config / program / "uniapi-placement.json";
}

bool Wayland() {
#ifdef __linux__
    const char* display = std::getenv("WAYLAND_DISPLAY");
    const char* backend = std::getenv("GDK_BACKEND");
    return display && *display && !(backend && std::string(backend).rfind("x11", 0) == 0);
#else
    return false;
#endif
}

// Runs this program again with one argument and waits: its exit code, or -1.
int RunSelf(const std::string& argument) {
    const std::string exe = uniapi::executablePath();
#ifdef _WIN32
    auto wide = [](const std::string& text) {
        std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
        return out;
    };
    std::wstring line = L"\"" + wide(exe) + L"\" " + wide(argument);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return int(code);
#else
    char* argv[] = {const_cast<char*>(exe.c_str()), const_cast<char*>(argument.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv, environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

// The main window remembered between runs (--main-first, then --main-second): the first places
// its window and quits; the second, given the remembered placement before run(), checks that it
// opened there. Exit code 0: as left; 1: not; 2: the first run couldn't place it.
int MainWindowRun(bool first) {
    uniapi::AppOptions options;
    options.title = std::string(kTitle) + (first ? ": main, first run" : ": main, second run");
    options.width = 400;
    options.height = 280;
    options.activate = false;
    options.rememberPlacement = kMainRememberAs;
    uniapi::App app(options);
    app.serve(placement_web);
    int code = 1;
    bool started = false;
    auto flow = [&]() -> uniapi::Task<> {
        co_await app.sleep(300ms);
        const bool canPlace = app.position().has_value();
        if (first) {
            app.setSize(450, 300);
            if (canPlace) app.setPosition(200, 210);
            co_await WaitFor(app, [&] {
                const auto p = app.position();
                return Near(app.size().width, 450) && (!canPlace || (p && Near(p->x, 200)));
            });
            code = 0;
        } else {
            const auto p = app.position();
            code = Near(app.size().width, 450) && Near(app.size().height, 300) && (!canPlace || (p && Near(p->x, 200) && Near(p->y, 210)))
                       ? 0 : 1;
            std::printf("main window, second run: %s %s\n", Text(p).c_str(), Text(app.size()).c_str());
        }
        app.quit();
    };
    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();
    };
    app.navigate("/index.html?main-run");
    app.run();
    return code;
}

}  // namespace

int main() {
    bool keepOpen = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--main-first" || argument == "--main-second") return MainWindowRun(argument == "--main-first");
        if (argument == "--exit") {
            keepOpen = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_placement_test [--exit]\n", argument.c_str());
            return 2;
        }
    }
    std::error_code ignored;
    fs::remove(PlacementFile(), ignored);  // a clean start

    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.activate = keepOpen;
    uniapi::App app(options);
    app.serve(placement_web);
    Checks checks;
    int changes = 0;
    bool started = false;
    app.onPlacementChanged = [&] { ++changes; };

    // By hand (the page's button): minimised, then restored 3 s later; what minimized() says.
    auto minimizeByHand = [&]() -> uniapi::Task<> {
        const auto began = std::chrono::steady_clock::now();
        app.minimize();
        co_await app.sleep(1s);
        const bool during = app.minimized();
        co_await app.sleep(2s);
        app.restore();
        co_await app.sleep(1s);
        const bool after = app.minimized();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        char text[160];
        std::snprintf(text, sizeof text, "minimised: minimized() said %s; after restore(): %s (%.1f s)",
                      during ? "true" : "false", after ? "true" : "false", seconds);
        std::printf("minimizeByHand: %s\n", text);
        app.call("showByHand", json::array({text}));
    };
    app.expose("minimizeByHand", [&] { minimizeByHand(); });

    uniapi::Window* second = nullptr;
    bool secondClosed = false;
    auto openSecond = [&] {
        uniapi::WindowOptions o;
        o.title = std::string(kTitle) + ": remembered";
        o.width = 380;
        o.height = 240;
        o.activate = keepOpen;
        o.rememberPlacement = kRememberAs;
        second = &app.openWindow(o);
        secondClosed = false;
        second->onClosed = [&] {
            secondClosed = true;
            second = nullptr;
        };
        second->navigate("/index.html?second");
    };

    auto flow = [&]() -> uniapi::Task<> {
        co_await app.sleep(300ms);  // on screen
        checks.Check("size(): the page area as opened (640x420)", Near(app.size().width, 640) && Near(app.size().height, 420),
                     Text(app.size()));

        const std::vector<uniapi::Screen> screens = app.screens();
        int current = 0;
        bool sound = !screens.empty();
        std::string list;
        for (const uniapi::Screen& s : screens) {
            if (s.current) ++current;
            sound = sound && s.width > 0 && s.height > 0 && s.scale >= 1;
            list += (list.empty() ? "" : "; ") + std::to_string(s.x) + "," + std::to_string(s.y) + " " +
                    std::to_string(s.width) + "x" + std::to_string(s.height) + " scale " + std::to_string(s.scale) +
                    (s.primary ? " primary" : "") + (s.current ? " current" : "");
        }
        std::printf("screens: %s\n", list.c_str());
        checks.Check("screens(): each with a size and a scale, the window on one of them", sound && current == 1, list);

        // The web engine (here, where the window and its page exist: on Windows its version is
        // known once WebView2 has started).
        const uniapi::WebEngine engine = app.webEngine();
        const std::string about = engine.name + " " + engine.version + (engine.variant.empty() ? "" : " (" + engine.variant + ")");
        std::printf("webEngine: %s\n", about.c_str());
        const bool numbers = !engine.version.empty() && engine.version.find_first_not_of("0123456789.") == std::string::npos
                             && engine.version.find('.') != std::string::npos;
#if defined(__linux__)
        const bool variantRight = engine.variant == "API 6.0, GTK 4" || engine.variant == "API 4.1, GTK 3";
#else
        const bool variantRight = engine.variant.empty();
#endif
        checks.Check("webEngine(): a name and a version of numbers (on Linux, which WebKitGTK it is built for)",
                     !engine.name.empty() && numbers && variantRight, about);

        const bool canPlace = app.position().has_value();
        if (canPlace) {
            app.setPosition(100, 120);
            const bool moved = co_await WaitFor(app, [&] {
                const auto p = app.position();
                return p && Near(p->x, 100) && Near(p->y, 120);
            });
            checks.Check("setPosition, then position: the same place", moved, Text(app.position()));
        } else {
            checks.NotApplicable("setPosition, then position", "this platform doesn't let apps place windows");
        }

        app.setSize(700, 500);
        const bool resized = co_await WaitFor(app, [&] { return Near(app.size().width, 700) && Near(app.size().height, 500); });
        checks.Check("setSize, then size: the same size", resized, Text(app.size()));

        if (canPlace) {
            app.center();
            const bool centred = co_await WaitFor(app, [&] {
                const auto p = app.position();
                return p && !Near(p->x, 100);
            });
            checks.Check("center moves it (to the middle of its screen)", centred, Text(app.position()));
        }

        const int before = changes;
#if defined(__linux__) && defined(UNIAPI_TEST_GTK3)
        const bool gtk3 = true;
#else
        const bool gtk3 = false;
#endif
        if (gtk3 && Wayland()) {
            // GTK 3 on Wayland: an app can ask to be minimised, but isn't told that it is and can't
            // undo it. Left out: the window would stay minimised for the checks that follow.
            checks.NotApplicable("minimize, then restore", "GTK 3 on Wayland can't tell a minimised window, nor bring it back");
        } else {
            app.minimize();
            const bool minimized = co_await WaitFor(app, [&] { return app.minimized(); });
            app.restore();
            const bool restored = co_await WaitFor(app, [&] { return !app.minimized(); });
            checks.Check("minimize, then restore", minimized && restored,
                         std::string(minimized ? "minimised" : "not minimised") + ", " + (restored ? "restored" : "not restored"));
        }

        app.maximize();
        const bool maximized = co_await WaitFor(app, [&] { return app.maximized(); });
        const std::string during = app.placement();
        app.restore();
        const bool back = co_await WaitFor(app, [&] {
            return !app.maximized() && Near(app.size().width, 700) && Near(app.size().height, 500);
        });
        checks.Check("maximize; its placement keeps the normal size and says maximised; restore gives that size back",
                     maximized && during.find("\"width\":700") != std::string::npos
                         && during.find("\"maximized\":true") != std::string::npos && back,
                     during + " then " + Text(app.size()));
        checks.Check("onPlacementChanged told of the changes", changes > before, std::to_string(changes - before) + " calls");

        const bool onTop = app.setAlwaysOnTop(true) && app.alwaysOnTop();
        app.setAlwaysOnTop(false);
        const bool offAgain = !app.alwaysOnTop();
        if (onTop) {
            checks.Check("setAlwaysOnTop: on, then off", offAgain);
        } else {
            checks.NotApplicable("setAlwaysOnTop", "this platform can't keep a window on top");
        }

        // A placement put back.
        const std::string kept = app.placement();
        app.setSize(500, 360);
        if (canPlace) app.setPosition(40, 60);
        co_await WaitFor(app, [&] { return Near(app.size().width, 500); });
        const bool parsed = app.setPlacement(kept);
        const bool putBack = co_await WaitFor(app, [&] { return app.placement() == kept; });
        checks.Check("setPlacement puts back what placement kept", parsed && putBack, kept + " -> " + app.placement());
        checks.Check("setPlacement refuses text placement didn't make", !app.setPlacement("{\"x\":1}") && !app.setPlacement("not json"));

        if (canPlace) {
            const bool taken = app.setPlacement("{\"x\":-100000,\"y\":-100000,\"width\":600,\"height\":400,\"hasPosition\":true}");
            const bool landed = co_await WaitFor(app, [&] {
                const auto p = app.position();
                return p && p->x > -2000 && p->y > -2000;
            });
            checks.Check("a placement off every screen lands on one (centred)", taken && landed, Text(app.position()));
        }

        // Remembered between openings.
        openSecond();
        co_await WaitFor(app, [&] { return second && second->connected(); });
        if (second) {
            second->setSize(420, 260);
            if (canPlace) second->setPosition(150, 160);
        }
        co_await WaitFor(app, [&] {
            if (!second) return true;
            const auto p = second->position();
            return Near(second->size().width, 420) && (!canPlace || (p && Near(p->x, 150)));
        });
        if (second) second->close();
        co_await WaitFor(app, [&] { return secondClosed; });
        const bool saved = fs::exists(PlacementFile());
        openSecond();
        co_await WaitFor(app, [&] { return second && second->connected(); });
        const uniapi::Size size = second ? second->size() : uniapi::Size{};
        const std::optional<uniapi::Point> where = second ? second->position() : std::nullopt;
        checks.Check("rememberPlacement: a window reopens where and as large as it closed",
                     saved && Near(size.width, 420) && Near(size.height, 260)
                         && (!canPlace || (where && Near(where->x, 150) && Near(where->y, 160))),
                     Text(where) + " " + Text(size) + (saved ? "" : ", nothing saved"));

        // maxWidth/maxHeight: a window doesn't grow past them, also when the program asks (the
        // system applies the limit to setSize too: Windows, GTK 3). macOS applies it to the user's
        // resizing only, GTK 4 has none.
        if (second) second->close();
        co_await WaitFor(app, [&] { return secondClosed; });
        {
#if defined(__APPLE__)
            checks.NotApplicable("maxWidth, maxHeight", "macOS limits the user's resizing only; try the checker");
#else
            uniapi::WindowOptions o;
            o.title = std::string(kTitle) + ": limited";
            o.width = 360;
            o.height = 240;
            o.maxWidth = 400;
            o.maxHeight = 300;
            o.backgroundColor = "#1b2636";
            o.activate = keepOpen;
            uniapi::Window& limited = app.openWindow(o);
            bool limitedClosed = false;
            limited.onClosed = [&] { limitedClosed = true; };
            limited.navigate("/index.html?limited");
            co_await WaitFor(app, [&] { return limited.connected(); });
            co_await app.sleep(300ms);
            if (app.webEngine().variant == "API 6.0, GTK 4") {
                checks.NotApplicable("maxWidth, maxHeight", "GTK 4 can't limit a window's size");
            } else {
                limited.setSize(600, 450);
                co_await app.sleep(700ms);
                const uniapi::Size got = limited.size();
                checks.Check("maxWidth, maxHeight: setSize past them stops there (400x300)",
                             got.width > 0 && got.width <= 402 && got.height <= 302, Text(got));
            }
            limited.close();
            co_await WaitFor(app, [&] { return limitedClosed; });
#endif
        }

        // The main window remembered: given before run(), where the screens may not be known yet.
        co_await uniapi::switchToBackground();
        const int firstRun = RunSelf("--main-first");
        const int secondRun = firstRun == 0 ? RunSelf("--main-second") : -1;
        co_await app.switchToUi();
        checks.Check("rememberPlacement on the main window: the next run opens where and as large as it closed",
                     firstRun == 0 && secondRun == 0,
                     "first run " + std::to_string(firstRun) + ", second run " + std::to_string(secondRun));

        std::printf("\nplacement: %d passed, %d failed\n", checks.passed, checks.failed);
        std::fflush(stdout);
        if (!keepOpen) {
            app.quit();
            co_return;
        }
        app.call("showResults", json::array({checks.lines}));
    };

    app.onConnect = [&](const std::string& page) {
        if (started || page != "main") return;
        started = true;
        flow();
    };
    int ticks = 0;
    app.every(90s, [&] {
        if (ticks++ == 0 || keepOpen) return;
        checks.Check("finished within 90 s", false);
        app.quit();
    });
    app.navigate("/index.html");
    app.run();
    fs::remove(PlacementFile(), ignored);  // the test's own, not the user's
    fs::remove(PlacementFile().parent_path(), ignored);
    return checks.failed == 0 && started ? 0 : 1;
}
