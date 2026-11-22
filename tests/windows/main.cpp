// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The windows test: App::openWindow against real web views. C++ opens further windows, each with
// its own page (web/second.html), and checks calls and events both ways (a window's own
// functions, falling back to the app's), binary data, a stream, navigation, a modal window, a
// window the user closes with a call pending, and a window that isn't owned closing with the app;
// and one stream reaching two windows' pages (Stream::addWindow).
// Exits with 0 when every check passes.
//
//   uniapi_windows_test                     keeps the main window open with the results
//   uniapi_windows_test --exit              closes when done, without taking focus (ctest)
//   uniapi_windows_test --no-shared-memory  streams fetched instead of shared

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

UNIAPI_ASSETS(windows_web);


#ifdef __APPLE__
int MacWindowTakesInput(const std::string& title);  // windows/mac.mm: 1 yes, 0 no, -1 not found
bool MacCloseAsUser(const std::string& title);      // windows/mac.mm: as its close button
#endif
namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr char kTitle[] = "uniapi windows test";

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

#ifdef _WIN32
HWND Find(const std::string& title) {
    return FindWindowW(L"uniapi.window", std::wstring(title.begin(), title.end()).c_str());
}
#endif

// A further window and what the test saw of it.
struct Watched {
    uniapi::Window* window = nullptr;
    std::string title;
    std::vector<std::string> connects;
    std::vector<std::string> disconnects;
    json report;
    bool closed = false;

    void Watch(uniapi::Window& w, std::string name) {
        window = &w;
        title = std::move(name);
        w.onConnect = [this](const std::string& page) { connects.push_back(page); };
        w.onDisconnect = [this](const std::string& page) { disconnects.push_back(page); };
        w.on("report", [this](const json& data) { report = data; });
        w.onClosed = [this] {
            closed = true;
            window = nullptr;  // destroyed once this returns
        };
    }
};

uniapi::WindowOptions Options(const std::string& title, bool activate) {
    uniapi::WindowOptions options;
    options.title = title;
    options.width = 420;
    options.height = 240;
    options.activate = activate;
    return options;
}

}  // namespace

int main() {
    bool keepOpen = true;
    bool shared = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--exit") {
            keepOpen = false;
        } else if (argument == "--no-shared-memory") {
            shared = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_windows_test [--exit] [--no-shared-memory]\n", argument.c_str());
            return 2;
        }
    }

    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.sharedMemory = shared;
    options.activate = keepOpen;
    uniapi::App app(options);
    app.serve(windows_web);
    Checks checks;

    // What every window's page can call: the app's functions, unless its window has its own.
    app.expose("who", [] { return "app"; });
    app.expose("shared", [] { return "from the app"; });
    app.expose("bytesLen", [](uniapi::Bytes bytes) { return bytes.size(); });
    app.expose("makeBytes", [](size_t size) { return uniapi::Bytes(std::vector<uint8_t>(size, 7)); });
    std::vector<std::string> toApp;  // the event only the app handles, by the page that sent it
    app.on("toApp", [&](const json& data) { toApp.push_back(data.get<std::string>()); });
    int mainPongs = 0;
    app.on("mainPong", [&](const json&) { ++mainPongs; });

    // A window opened before run(): it opens with the main window.
    Watched early;
    early.Watch(app.openWindow(Options(std::string(kTitle) + ": early", keepOpen)), std::string(kTitle) + ": early");
    early.window->navigate("/second.html?early");

    Watched second, modal, loose;
    int pong = 0;
    json frame;
    bool started = false;
    bool loosePresent = false;

    auto flow = [&]() -> uniapi::Task<> {
        // 1. The window opened before run().
        bool ok = co_await WaitFor(app, [&] { return !early.report.is_null(); });
        checks.Check("a window opened before run() opens with the main window; its page connects and reports",
                     ok && early.connects == std::vector<std::string>{"early"} && early.report.value("page", "") == "early",
                     early.report.dump());

        // 2. A further window: its own function wins, the app's are its fallback (functions and
        // events), binary data both ways.
        second.Watch(app.openWindow(Options(std::string(kTitle) + ": second", keepOpen)), std::string(kTitle) + ": second");
        second.window->expose("who", [] { return "window"; });
        int frames = 0;
        second.window->on("frameSeen", [&](const json& data) {
            frame = data;
            ++frames;
        });
        second.window->on("pong", [&](const json& data) { pong = data.get<int>(); });
        second.window->navigate("/second.html");
        ok = co_await WaitFor(app, [&] { return !second.report.is_null(); });
        const json& report = second.report;
        checks.Check("a further window's page connects (its own onConnect); the app counts it",
                     ok && second.connects == std::vector<std::string>{"second"} && second.window && second.window->connected()
                         && second.window->page() == "second" && app.windows().size() == 2 && app.connected(),
                     std::to_string(second.connects.size()) + " " + std::to_string(app.windows().size()));
#ifdef _WIN32
        {
            // uniapi_icon() gave this program the uniapi logo; with no setIcon its windows show it.
            const HWND mainWindow = Find(kTitle), further = Find(std::string(kTitle) + ": second");
            const LPCWSTR groupIcon = MAKEINTRESOURCEW(14);  // RT_GROUP_ICON (the narrow one here: no UNICODE)
            const bool hasIcon = FindResourceW(nullptr, MAKEINTRESOURCEW(1), groupIcon) != nullptr;
            const auto bigIcon = [](HWND w) { return w ? GetClassLongPtrW(w, GCLP_HICON) : 0; };
            const auto smallIcon = [](HWND w) { return w ? GetClassLongPtrW(w, GCLP_HICONSM) : 0; };
            checks.Check("Windows: the program's icon (uniapi_icon) is every window's title-bar icon",
                         hasIcon && bigIcon(mainWindow) && smallIcon(mainWindow) && bigIcon(further) && smallIcon(further),
                         std::string(hasIcon ? "" : "no icon resource; ") + (mainWindow ? "" : "no main window; ")
                             + (further ? "" : "no further window"));
        }
#endif
        checks.Check("its calls: its window's function first, the app's where it has none; binary data both ways",
                     report.value("who", "") == "window" && report.value("shared", "") == "from the app"
                         && report.value("bytesLen", 0) == 1000 && report.value("madeBytes", 0) == 500 && report.value("madeFirst", 0) == 7,
                     report.dump());
        ok = co_await WaitFor(app, [&] { return toApp.size() >= 2; });
        checks.Check("its events: the app's handler where its window has none", ok && toApp.back() == "second",
                     std::to_string(toApp.size()));

        // 3. C++ to it: calls and events reach its page only.
        uniapi::Result doubled = co_await second.window->callAsync("double", {21});
        uniapi::Result notThere = co_await second.window->callAsync("mainOnly");
        uniapi::Result onMain = co_await app.callAsync("mainOnly");
        second.window->emit("ping", 7);
        ok = co_await WaitFor(app, [&] { return pong == 8; });
        co_await app.sleep(100ms);
        checks.Check("C++ calls and events reach that window's page only (and the main page its own)",
                     doubled.ok && doubled.value == json(42) && !notThere.ok && onMain.ok && onMain.value == json("main page") && ok
                         && mainPongs == 0,
                     doubled.value.dump() + " " + notThere.error + " " + onMain.value.dump() + " " + std::to_string(pong));

        // 4. A stream into it: shared memory where the app has it.
        uniapi::Stream& stream = second.window->stream("frames", 4096, 2);
        uniapi::Stream::Slot slot;
        uint64_t sum = 0;
        if (stream.acquire(slot)) {
            for (size_t i = 0; i < 4096; ++i) {
                slot.data[i] = uint8_t(i % 251);
                sum += uint8_t(i % 251);
            }
            stream.publish(slot, 4096, {{"seq", 1}});
        }
        ok = co_await WaitFor(app, [&] { return frames > 0; });
        checks.Check("a stream into a further window: its page gets the frame intact; shared memory as in the main window",
                     ok && frame.value("sum", uint64_t(0)) == sum && frame.value("size", 0) == 4096 && frame.value("seq", 0) == 1
                         && stream.shared() == app.sharedMemory() && second.window->sharedMemory() == app.sharedMemory(),
                     frame.dump() + " shared " + std::to_string(stream.shared()) + "/" + std::to_string(app.sharedMemory()));

        // 4b. One stream, two windows: the app's stream reaches the second window's page too. One
        // slot: it comes back only once both pages handed it back (the main page holds it until
        // letGo). Removing the window: frames reach the main page only. Replacing it keeps the
        // windows it reaches. A window can't have a second stream of that name.
        std::vector<json> mainSaw, bothSeen;
        bool freed = false;
        app.on("mainSaw", [&](const json& data) { mainSaw.push_back(data); });
        second.window->on("bothSeen", [&](const json& data) { bothSeen.push_back(data); });
        uniapi::Stream* both = &app.stream("both", 2048, 1);
        both->addWindow(*second.window);
        both->onSlotFreed = [&] { freed = true; };
        bool clash = false;
        try {
            second.window->stream("both", 16, 1);
        } catch (const std::invalid_argument&) {
            clash = true;
        }
        const bool reaches = both->windows().size() == 2;
        auto publish = [&](uniapi::Stream& s, int seq, bool hold, uint64_t& total) {
            uniapi::Stream::Slot one;
            if (!s.acquire(one)) return false;
            total = 0;
            for (size_t i = 0; i < 2048; ++i) {
                one.data[i] = uint8_t((i * 7 + seq) % 253);
                total += one.data[i];
            }
            return s.publish(one, 2048, {{"seq", seq}, {"hold", hold}});
        };
        uint64_t sum1 = 0;
        const bool published = publish(*both, 1, true, sum1);
        ok = co_await WaitFor(app, [&] { return !mainSaw.empty() && !bothSeen.empty(); });
        checks.Check("a stream reaching two windows: both pages get the same frame intact (Stream::addWindow)",
                     reaches && published && ok && mainSaw[0].value("sum", uint64_t(0)) == sum1 && bothSeen[0].value("sum", uint64_t(0)) == sum1
                         && both->shared() == app.sharedMemory(),
                     std::to_string(mainSaw.size()) + " " + std::to_string(bothSeen.size()) + " shared " + std::to_string(both->shared()));
        co_await app.sleep(100ms);  // the second page has handed it back by now; the main one holds it
        uniapi::Stream::Slot probe;
        const bool heldBack = !both->acquire(probe) && both->delivered() == 0;
        app.emit("letGo");
        ok = co_await WaitFor(app, [&] { return freed; });
        checks.Check("its slot comes back once every page handed it back (onSlotFreed, delivered)",
                     heldBack && ok && both->delivered() == 1, std::to_string(heldBack) + " " + std::to_string(both->delivered()));
        both->removeWindow(*second.window);
        uint64_t sum2 = 0;
        const bool published2 = publish(*both, 2, false, sum2);
        ok = co_await WaitFor(app, [&] { return mainSaw.size() == 2; });
        co_await app.sleep(100ms);
        const bool onlyMain = published2 && ok && bothSeen.size() == 1 && both->windows().size() == 1;
        both->addWindow(*second.window);
        both = &app.stream("both", 2048, 1);  // replaced: still reaches the second window
        uint64_t sum3 = 0;
        const bool published3 = publish(*both, 3, false, sum3);
        ok = co_await WaitFor(app, [&] { return mainSaw.size() == 3 && bothSeen.size() == 2; });
        checks.Check("removed, its page gets no more frames; replaced, the stream keeps its windows; a window can't have two of a name",
                     onlyMain && clash && published3 && ok && bothSeen.back().value("sum", uint64_t(0)) == sum3
                         && bothSeen.back().value("seq", 0) == 3 && both->windows().size() == 2,
                     std::to_string(onlyMain) + " " + std::to_string(clash) + " " + std::to_string(bothSeen.size()));

        // 5. Navigating it: its page goes and a new one connects.
        second.window->navigate("/second.html?again");
        ok = co_await WaitFor(app, [&] { return second.connects.size() == 2; });
        checks.Check("navigated, a further window's page goes (onDisconnect) and the new one connects",
                     ok && second.disconnects == std::vector<std::string>{"second"} && second.connects.back() == "again",
                     std::to_string(second.connects.size()));

        // 6. A modal window: the main window takes no input until it closes.
        uniapi::WindowOptions modalOptions = Options(std::string(kTitle) + ": modal", keepOpen);
        modalOptions.modal = true;
        modal.Watch(app.openWindow(modalOptions), modalOptions.title);
        modal.window->navigate("/second.html?modal");
        ok = co_await WaitFor(app, [&] { return !modal.report.is_null(); });
#ifdef _WIN32
        const bool disabled = !IsWindowEnabled(Find(kTitle));
#elif defined(__APPLE__)
        const bool disabled = MacWindowTakesInput(kTitle) == 0;
#else
        const bool disabled = true;
#endif
        modal.window->close();
        const bool closedLater = co_await WaitFor(app, [&] { return modal.closed; });
        co_await app.sleep(50ms);
#ifdef _WIN32
        const bool enabledAgain = IsWindowEnabled(Find(kTitle)) != FALSE;
#elif defined(__APPLE__)
        const bool enabledAgain = MacWindowTakesInput(kTitle) == 1;
#else
        const bool enabledAgain = true;
#endif
        checks.Check("a modal window holds the main window until it closes (close(); onClosed; then it is gone)",
                     ok && disabled && closedLater && enabledAgain && app.windows().size() == 2,
                     std::to_string(disabled) + " " + std::to_string(closedLater) + " " + std::to_string(enabledAgain) + " "
                         + std::to_string(app.windows().size()));

        // 7. The user closes a window while C++ waits on its page: the call fails, onClosed.
        std::string pendingError;
        second.window->call("never", json::array(), [&](const uniapi::Result& result) { pendingError = result.error; });
#ifdef _WIN32
        PostMessageW(Find(second.title), WM_CLOSE, 0, 0);
#elif defined(__APPLE__)
        if (!MacCloseAsUser(second.title)) second.window->close();
#else
        second.window->close();
#endif
        ok = co_await WaitFor(app, [&] { return second.closed; });
        co_await app.sleep(50ms);
        checks.Check("closed by the user: a call it hadn't answered fails (\"the page closed\"), onClosed, then it is gone",
                     ok && pendingError == "the page closed" && second.disconnects.size() == 2 && app.windows().size() == 1,
                     pendingError + " " + std::to_string(app.windows().size()));

        // 8. A window that isn't owned: it closes with the app (checked after run()).
        uniapi::WindowOptions looseOptions = Options(std::string(kTitle) + ": not owned", keepOpen);
        looseOptions.owned = false;
        loose.Watch(app.openWindow(looseOptions), looseOptions.title);
        loose.window->navigate("/second.html?loose");
        ok = co_await WaitFor(app, [&] { return !loose.report.is_null(); });
        loosePresent = ok && loose.window && loose.window->connected();

        std::printf("\nwindows: %d passed, %d failed (and one after the app ends)\n", checks.passed, checks.failed);
        std::fflush(stdout);
        app.call("showResults", json::array({checks.lines}));
        if (!keepOpen) app.quit();
    };

    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();  // a Task runs at once, to its first co_await
    };
    // A page that never connects must not hang the test.
    int ticks = 0;
    app.every(60s, [&] {
        if (ticks++ == 0) return;  // the first call comes at once
        std::printf("the test didn't finish within 60 s\n");
        checks.failed++;
        app.quit();
    });
    app.navigate("/index.html");
    app.run();

    checks.Check("a window that isn't owned closes with the app (onClosed)", loosePresent && loose.closed,
                 std::to_string(loosePresent) + " " + std::to_string(loose.closed));
    checks.Check("so does the one opened before run()", early.closed);
    std::printf("\nwindows: %d passed, %d failed\n", checks.passed, checks.failed);
    return checks.failed == 0 && started ? 0 : 1;
}
