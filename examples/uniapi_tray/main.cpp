// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The tray: the app's icon in the menu bar (macOS), the notification area (Windows) or the
// desktop's tray (Linux), with a menu. Closing the window hides it; the tray brings it back, and
// its Quit ends the app. With --hidden the app starts in the tray only, as an app started at
// login would. The page is web/index.html.
//
//   uniapi_tray            the window, and the icon in the tray
//   uniapi_tray --hidden   the icon only; "Show window" in its menu opens the window
//   uniapi_tray --click    a click on the icon shows the window (the right button: the menu);
//                          the default on Windows and Linux, where a click usually opens the app
//
// Without a tray (stock GNOME has none), the app runs as any other: closing the window ends it.

#include <uniapi/tray.hpp>
#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

UNIAPI_ASSETS(tray_web);
UNIAPI_ASSETS(tray_icons);

namespace {

std::vector<unsigned char> Icon(const char* name) {
    const uniapi::Asset* asset = tray_icons.find(name);
    if (!asset) return {};
    const uniapi::AssetData data = uniapi::assetData(*asset);
    return std::vector<unsigned char>(data.data, data.data + data.size);
}

}  // namespace

int main() {
    const std::vector<std::string> arguments = uniapi::arguments();
    auto has = [&](const char* flag) { return std::find(arguments.begin(), arguments.end(), flag) != arguments.end(); };
    const bool hidden = has("--hidden") && uniapi::tray::available();
#ifdef __APPLE__
    const bool clickShows = has("--click");
#else
    const bool clickShows = true;
#endif

    uniapi::AppOptions options;
    options.title = "uniapi tray";
    options.width = 520;
    options.height = 400;
    options.startHidden = hidden;
    uniapi::App app(options);
    app.serve(tray_web);

    // The icon: on macOS a template (drawn in the menu bar's colour), elsewhere the logo.
#ifdef __APPLE__
    uniapi::tray::Options icon{Icon("uniapi-template-36.png"), true, "uniapi tray"};
#else
    uniapi::tray::Options icon{Icon("uniapi-32.png"), false, "uniapi tray"};
#endif
    std::unique_ptr<uniapi::tray::Tray> tray;
    try {
        tray = std::make_unique<uniapi::tray::Tray>(app, icon);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "no tray: %s\n", e.what());
    }

    bool muted = false;
    int clicks = 0;
    // state tells the page what the tray did.
    auto state = [&] { app.emit("state", {{"tray", tray != nullptr}, {"muted", muted}, {"clicks", clicks}}); };
    std::function<void()> menu;
    auto show = [&] {
        app.show();
        menu();
    };
    auto hide = [&] {
        app.hide();
        menu();
    };
    // The menu, again whenever what it shows changes.
    menu = [&] {
        if (!tray) return;
        tray->setMenu({
            {"Show window", show, false, app.visible()},
            {"Hide window", hide, false, !app.visible()},
            uniapi::tray::Item::separator(),
            {"Muted",
             [&] {
                 muted = !muted;
                 menu();
                 state();
             },
             muted},
            {"Tray clicks: " + std::to_string(clicks), {}, false, true},
            uniapi::tray::Item::separator(),
            {"Quit", [&] { app.quit(); }},
        });
    };
    if (tray) {
        if (clickShows) {
            tray->onClick = [&] {
                ++clicks;
                if (app.visible()) {
                    hide();
                } else {
                    show();
                }
                state();
            };
        }
        menu();
        // Closing the window hides it: the app keeps running in the tray. macOS's Quit (Cmd+Q)
        // still ends it.
        app.onCloseRequested = [&](uniapi::CloseReason reason) {
            if (reason == uniapi::CloseReason::Quit) return true;
            hide();
            return false;
        };
    }

    app.onConnect = [&](const std::string&) { state(); };
    app.expose("hide", [&] { hide(); });
    app.on("muted", [&](const uniapi::json& data) {
        muted = data.is_boolean() && data.get<bool>();
        menu();
    });
    app.expose("quit", [&] { app.quit(); });

    app.navigate("/index.html");
    const int code = app.run();
    tray.reset();  // before the App: the icon goes now (the App's end would take it away too)
    return code;
}
