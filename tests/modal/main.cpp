// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The modal test (Linux): whether a modal window really keeps input from the main window, with
// clicks. The windows test checks the rest of modal windows (and this on Windows and macOS); on
// Linux it can't look at the main window's input, so this test clicks.
//
//   uniapi_modal_test             GTK 3: clicks made inside the app (automatic)
//   uniapi_modal_test --by-hand   either GTK: you click where the windows say
//
// GTK 3's clicks are synthetic, handed to GTK as if from the mouse (modal/linux.cpp): they go
// through GTK's own event handling, where a modal window holds the other windows of the app, but
// not through the compositor. GTK 4 has no way to make an event: there the automatic run is
// inconclusive (exit code 3, which ctest counts as skipped), and the clicks are yours.
//
// Automatic: the same click on the main window, four times: alone (it must arrive, or the clicks
// don't work and the test can't tell), with an ordinary further window open (it arrives), with a
// modal window open (it must not; a click on the modal window must), and after the modal window
// closed (it arrives again). By hand: you click the main window, then with the modal window open
// you click the main window, then the modal window, then, once it has closed, the main window
// again. The windows appear on screen while it runs.

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

UNIAPI_ASSETS(modal_web);

int Click(const std::string& title, int x, int y);  // modal/linux.cpp

namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr char kTitle[] = "uniapi modal test";

uniapi::Task<bool> WaitFor(uniapi::App& app, std::function<bool()> done, std::chrono::milliseconds limit) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - start > limit) co_return false;
        co_await app.sleep(20ms);
    }
    co_return true;
}

}  // namespace

int main() {
    const std::vector<std::string> arguments = uniapi::arguments();
    const bool byHand = std::find(arguments.begin(), arguments.end(), "--by-hand") != arguments.end();
    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.activate = byHand;
    uniapi::App app(options);
    app.serve(modal_web);

    std::map<std::string, int> clicks;  // pointerdowns by page
    app.on("input", [&](const json& data) {
        if (data.value("kind", "") == "pointerdown") ++clicks[data.value("page", "")];
    });

    int failed = 0;
    bool inconclusive = false;
    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        if (!ok) ++failed;
        std::printf("%s%s  (%s)\n", ok ? "  pass  " : "  FAIL  ", name.c_str(), detail.c_str());
        std::fflush(stdout);
    };
    // A click in a window, inside the app: "" if made, else why not.
    auto click = [](const std::string& name, int x, int y) -> std::string {
        switch (Click(name, x, y)) {
        case -1: return "this GTK can't make clicks: build with GTK 3, or run with --by-hand";
        case 0: return "no window \"" + name + "\"";
        default: return "";
        }
    };
    // Clicks the main window's corner (outside where a dialog over it opens): whether its page saw it.
    auto clickMain = [&](std::string& error) -> uniapi::Task<bool> {
        const int before = clicks["main"];
        error = click(kTitle, 40, 40);
        if (!error.empty()) co_return false;
        co_return co_await WaitFor(app, [&] { return clicks["main"] > before; }, 700ms);
    };
    uniapi::Window* opened = nullptr;
    bool openedClosed = false;
    auto open = [&](const std::string& name, bool modal) -> uniapi::Task<bool> {
        uniapi::WindowOptions o;
        o.title = std::string(kTitle) + ": " + name;
        o.width = 420;
        o.height = 240;
        o.activate = byHand;
        o.modal = modal;
        opened = &app.openWindow(o);
        openedClosed = false;
        opened->onClosed = [&] {
            openedClosed = true;
            opened = nullptr;
        };
        opened->navigate("/index.html?name=" + name);
        const bool connected = co_await WaitFor(app, [&] { return opened && opened->connected(); }, 10s);
        co_await app.sleep(300ms);  // mapped and placed by the window manager
        co_return connected;
    };
    auto closeOpened = [&]() -> uniapi::Task<> {
        if (opened) opened->close();
        co_await WaitFor(app, [&] { return openedClosed; }, 5s);
        co_await app.sleep(300ms);
    };
    auto say = [](uniapi::Window& w, const std::string& text) {
        std::printf("  >>    %s\n", text.c_str());
        std::fflush(stdout);
        w.call("say", json::array({text}));
    };

    auto automatic = [&]() -> uniapi::Task<> {
        // 1. Alone: the click must arrive, or the clicks don't work and the test can't tell.
        std::string error;
        bool seen = co_await clickMain(error);
        if (!seen) {
            inconclusive = true;
            std::printf("  ????  a click on the main window alone didn't arrive (%s)\n", error.empty() ? "not seen" : error.c_str());
            co_return;
        }
        check("alone: the main page gets the click", true, "control");

        // 2. An ordinary further window open: the main window still takes input.
        bool connected = co_await open("plain", false);
        seen = co_await clickMain(error);
        check("an ordinary further window open: the main page still gets the click", connected && error.empty() && seen,
              "connected " + std::to_string(connected) + ", seen " + std::to_string(seen) + " " + error);
        co_await closeOpened();

        // 3. A modal window open: the main window takes none; the modal one does.
        connected = co_await open("modal", true);
        seen = co_await clickMain(error);
        check("a modal window open: the main page gets no click", connected && error.empty() && !seen,
              "connected " + std::to_string(connected) + ", seen " + std::to_string(seen) + " " + error);
        const int before = clicks["modal"];
        error = click(std::string(kTitle) + ": modal", 60, 60);
        const bool seenModal = error.empty() && co_await WaitFor(app, [&] { return clicks["modal"] > before; }, 700ms);
        check("... while the modal window itself gets its click", seenModal, error);

        // 4. The modal window closed: the main window takes input again.
        co_await closeOpened();
        seen = co_await clickMain(error);
        check("the modal window closed: the main page gets the click again", error.empty() && seen, error);
    };

    auto byHandFlow = [&]() -> uniapi::Task<> {
        constexpr auto kPatience = 90s;
        // 1. Your click on the main window must arrive, or the test can't tell.
        say(app, "Click this window.");
        if (!co_await WaitFor(app, [&] { return clicks["main"] > 0; }, kPatience)) {
            inconclusive = true;
            std::printf("  ????  no click on the main window arrived\n");
            co_return;
        }
        check("alone: the main page gets your click", true, "control");

        // 2. The modal window: clicks on the main window must not arrive; one on it must.
        say(app, "Wait...");
        const bool connected = co_await open("modal", true);
        const int mainBefore = clicks["main"];
        if (opened) say(*opened, "Click the main window behind me a few times (on its text too), then click here.");
        const bool gotModal = co_await WaitFor(app, [&] { return clicks["modal"] > 0; }, kPatience);
        const int mainDuring = clicks["main"] - mainBefore;
        check("a modal window open: the main page gets none of your clicks", connected && gotModal && mainDuring == 0,
              std::to_string(mainDuring) + " click(s) reached it");
        check("... while the modal window itself gets yours", gotModal, std::to_string(clicks["modal"]) + " click(s)");

        // 3. Closed: the main window takes your click again.
        co_await closeOpened();
        const int again = clicks["main"];
        say(app, "Click this window once more.");
        const bool clickedAgain = co_await WaitFor(app, [&] { return clicks["main"] > again; }, kPatience);
        check("the modal window closed: the main page gets your click again", clickedAgain, "");
        say(app, "Done: see the terminal.");
        co_await app.sleep(1s);
    };

    bool started = false;
    auto flow = [&]() -> uniapi::Task<> {
        co_await app.sleep(500ms);  // mapped and placed
        if (byHand) {
            co_await byHandFlow();
        } else {
            co_await automatic();
        }
        app.quit();
    };
    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();
    };
    app.navigate("/index.html?name=main");
    app.run();
    if (inconclusive) {
        std::printf("\nmodal: inconclusive\n");
        return 3;
    }
    if (failed > 0) {
        std::printf("\nmodal: %d check(s) failed\n", failed);
        return 1;
    }
    std::printf("\nmodal: all checks passed\n");
    return 0;
}
