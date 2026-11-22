// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// uniapi latency: how long messages take between C++ and the page, and how long C++'s answer to
// a click takes to come back. Not a test with a verdict: a measure to run by hand (or with
// --exit for the first part only), on a machine and a WebView2 you want to know about.
//
//   1. Messages (runs by itself): the page calls C++ 100 times, idle, with the page busy (10 ms
//      of work every frame) and with C++ busy (8 ms of work every 33 ms on its thread), and
//      shows how long each way takes (on a clock synced to C++'s) and the round trip.
//   2. After a click (by hand): each click on the pad calls C++; the time from the click (the
//      input event's own time) until the answer is back. Clicks alternate between changing the
//      screen and not; "Animate the page" adds an animation every frame.
//
//   uniapi_latency                          both parts, results on the page and in the console
//   uniapi_latency --exit                   the first part only, printed; then it closes
//   uniapi_latency --keep-defer             Chromium's deferral of tasks after an input kept
//                                           (AppOptions::deferTasksAfterInput), to compare
//   uniapi_latency --browser-arguments=...  extra switches for the web engine (Windows)
//
// The results print to the console when it is started from one (stdout).

#include <uniapi/uniapi.hpp>

#include <chrono>
#include <cstdio>
#include <string>

UNIAPI_ASSETS(latency_web);

int main() {
    bool exitAfter = false;
    uniapi::AppOptions options;
    options.title = "uniapi latency";
    options.width = 720;
    options.height = 760;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--exit") {
            exitAfter = true;
            options.activate = false;  // unattended: don't take keyboard focus
        } else if (argument == "--keep-defer") {
            options.deferTasksAfterInput = true;
        } else if (argument.rfind("--browser-arguments=", 0) == 0) {
            options.browserArguments = argument.substr(20);
        } else {
            std::printf("unknown option %s\nusage: uniapi_latency [--exit] [--keep-defer] [--browser-arguments=...]\n",
                        argument.c_str());
            return 2;
        }
    }
    uniapi::App app(options);
    app.serve(latency_web);

    const auto start = std::chrono::steady_clock::now();
    const auto now = [start] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    // C++'s clock when the call arrives: the first thing its handler does.
    app.expose("arrive", [now]() { return now(); });
    // How this run is set up, for the page to show.
    app.expose("setup", [&]() {
        return uniapi::json{{"exit", exitAfter},
                            {"deferTasksAfterInput", options.deferTasksAfterInput},
                            {"browserArguments", options.browserArguments},
                            {"sharedMemory", app.sharedMemory()}};
    });
    // C++ busy: `ms` of work every 33 ms on its thread (0: none).
    double busyMs = 0;
    app.expose("setBusy", [&busyMs](double ms) {
        busyMs = ms;
        return true;
    });
    app.every(std::chrono::milliseconds(33), [&busyMs, now] {
        const double until = now() + busyMs;
        while (now() < until) {
        }
    });
    app.expose("print", [](const std::string& text) {
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
        return true;
    });
    app.expose("done", [&app, exitAfter]() {
        if (exitAfter) app.quit();
        return exitAfter;
    });

    app.navigate("/index.html");
    return app.run();
}
