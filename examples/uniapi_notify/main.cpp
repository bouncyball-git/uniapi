// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// System notifications: the page asks C++ to show one (App::notify), and a click on it comes back
// (App::onNotificationClicked), bringing the window forward. The page is web/index.html.
//
// Notifications come from installed apps: on macOS only from an .app bundle, and on Linux the
// desktop shows the app's name and icon from its .desktop file. uniapi_install() makes both:
//
//   cmake --install build --prefix ~/Applications   macOS: then open "uniapi notify"
//   cmake --install build --prefix ~/.local         Linux: then run uniapi_notify
//
// Run from the build folder on macOS, it shows the reason it can't instead.

#include <uniapi/uniapi.hpp>

UNIAPI_ASSETS(notify_web);

int main() {
    uniapi::AppOptions options;
    options.title = "uniapi notify";
    options.width = 560;
    options.height = 460;
    uniapi::App app(options);
    app.serve(notify_web);

    // Answered once the system has it (or says why not): through the Reply, with its id.
    app.expose("notify", [&app](const uniapi::json& args, uniapi::Reply reply) {
        auto text = [&args](size_t i) {
            return i < args.size() && args[i].is_string() ? args[i].get<std::string>() : std::string();
        };
        uniapi::Notification n;
        n.title = text(0);
        n.body = text(1);
        app.notify(n, [reply](const uniapi::Result& result) {
            if (result.ok) {
                reply.resolve(result.value);
            } else {
                reply.reject(result.error);
            }
        });
    });

    // A click on one: the window comes forward, and the page logs it. (Without this handler
    // uniapi shows the main window by itself.)
    app.onNotificationClicked = [&app](const std::string& id) {
        app.show();
        app.emit("clicked", id);
    };

    app.navigate("/index.html");
    return app.run();
}
