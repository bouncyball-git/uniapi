// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The tray's C API (uniapi/tray.h) over uniapi::tray::Tray.

#include <uniapi/tray.h>
#include <uniapi/tray.hpp>

#include "c/c_internal.hpp"
#include "native.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using uniapi::c_api::Guard;
using uniapi::c_api::GuardVoid;

extern "C" {

int uniapi_tray_available(void) {
    return Guard(0, [] { return uniapi::tray::available() ? 1 : 0; });
}

uniapi_tray* uniapi_tray_new(uniapi_app* app, const unsigned char* png, size_t size, int template_image, const char* tooltip) {
    return Guard<uniapi_tray*>(nullptr, [&] {
        if (!app) throw std::invalid_argument("uniapi_tray_new: no app");
        uniapi::tray::Options options;
        if (png && size) options.icon.assign(png, png + size);
        options.templateImage = template_image != 0;
        if (tooltip) options.tooltip = tooltip;
        auto handle = std::make_unique<uniapi_tray>();
        handle->tray = std::make_unique<uniapi::tray::Tray>(app->app, std::move(options));
        return handle.release();
    });
}

void uniapi_tray_free(uniapi_tray* tray) { delete tray; }

void uniapi_tray_set_menu(uniapi_tray* tray, const uniapi_tray_item* items, int count) {
    GuardVoid([&] {
        std::vector<uniapi::tray::Item> menu;
        for (int i = 0; i < count && items; ++i) {
            const uniapi_tray_item& c = items[i];
            uniapi::tray::Item item;
            item.label = c.label ? c.label : "";
            item.isSeparator = (c.flags & UNIAPI_TRAY_SEPARATOR) != 0;
            item.checked = (c.flags & UNIAPI_TRAY_CHECKED) != 0;
            item.disabled = (c.flags & UNIAPI_TRAY_DISABLED) != 0;
            if (c.on_click) item.onClick = [fn = c.on_click, user = c.user] { fn(user); };
            menu.push_back(std::move(item));
        }
        tray->tray->setMenu(std::move(menu));
    });
}

int uniapi_tray_set_icon(uniapi_tray* tray, const unsigned char* png, size_t size, int template_image) {
    return Guard(0, [&] { return tray->tray->setIcon(png, size, template_image != 0) ? 1 : 0; });
}

void uniapi_tray_set_tooltip(uniapi_tray* tray, const char* text) {
    GuardVoid([&] { tray->tray->setTooltip(text ? text : ""); });
}

void uniapi_tray_on_click(uniapi_tray* tray, uniapi_callback handler, void* user) {
    GuardVoid([&] {
        tray->tray->onClick = handler ? std::function<void()>([handler, user] { handler(user); }) : nullptr;
    });
}

}  // extern "C"
