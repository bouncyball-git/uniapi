// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// uniapi::tray: the app's icon in the system tray: the menu bar's status items on macOS, the
// notification area on Windows, the desktop's tray on Linux (StatusNotifierItem). It has a
// tooltip, a menu, and a click of its own:
//
//   uniapi::tray::Tray tray(app, {logoPng, false, "Mixer"});
//   tray.setMenu({
//       {"Show", [&] { app.show(); }},
//       uniapi::tray::Item::separator(),
//       {"Quit", [&] { app.quit(); }},
//   });
//   tray.onClick = [&] { app.show(); };  // empty: a click opens the menu
//
// An app that keeps running in the tray hides its window when it is closed
// (Window::onCloseRequested and hide()) and starts hidden at login (AppOptions::startHidden).
//
// A part of its own: link uniapi_tray (target_link_libraries(myapp PRIVATE uniapi_tray)); apps
// that don't carry none of it. Use a Tray on the UI thread, as the App: in handlers, timers and
// posted functions, or in main() before run(). The icon goes when the Tray is destroyed, with
// remove(), or when the App ends, whichever comes first.

#include <uniapi/uniapi.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uniapi::tray {

// One entry of the tray's menu.
struct Item {
    std::string label;
    std::function<void()> onClick;  // on the UI thread
    bool checked = false;           // a check mark beside it
    bool disabled = false;          // shown greyed, not chosen
    bool isSeparator = false;       // a line between items (label and the rest unused)

    static Item separator() {
        Item item;
        item.isSeparator = true;
        return item;
    }
};

// The icon's.
struct Options {
    // The icon, PNG. macOS draws it 18 points high in the menu bar (a 36-pixel image is sharp on
    // Retina screens); Windows scales it to the notification area's size (16 pixels at 100%);
    // Linux hosts scale it.
    std::vector<unsigned char> icon;
    // macOS: the icon is a template, drawn from its shape (alpha) alone in the menu bar's colour,
    // dark or light as the system's icons are. Elsewhere it is drawn as it is.
    bool templateImage = false;
    std::string tooltip;  // shown when the pointer rests on the icon
};

// Whether there is a tray to put an icon in. Always on macOS and Windows; on Linux when the
// desktop has a tray host (a StatusNotifierWatcher: KDE Plasma, XFCE, Cinnamon, LXQt, waybar...),
// which stock GNOME hasn't without the AppIndicator extension (Ubuntu turns it on). Without one,
// an app shouldn't hide its window to the tray: nothing would bring it back.
bool available();

// The app's icon in the tray, with its menu. Not copyable.
class Tray {
public:
    // Puts the app's icon in the tray once run() runs (before run(): when it starts). Throws
    // std::runtime_error if there is no tray (available()) or the icon isn't a PNG the platform
    // can read.
    Tray(App& app, Options options);
    ~Tray();  // remove()
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // A click on the icon (macOS and Windows: the left button; Linux: as the desktop does,
    // usually the left button). Empty: the click opens the menu. The right button (macOS: also
    // Control-click) always opens the menu. On Linux, set it before the icon appears (in main(),
    // or right after making the Tray): hosts read whether a click opens the menu then.
    std::function<void()> onClick;

    // Replaces the menu. Change an item (its check mark, its label) by setting the menu again.
    void setMenu(std::vector<Item> items);
    // Changes the icon (PNG; templateImage: see Options). False if it isn't one the platform can
    // read; the icon stays as it was.
    bool setIcon(const unsigned char* png, size_t size, bool templateImage = false);
    void setTooltip(std::string_view text);
    // Takes the icon out of the tray for good (the Tray then does nothing). The App ending does
    // it too.
    void remove();
    bool removed() const;

    struct Impl;  // internal (tray/native.hpp: what the tests look at)
    Impl* internal() const { return impl_.get(); }

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace uniapi::tray
