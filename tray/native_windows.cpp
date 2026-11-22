// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The Windows tray: an icon in the notification area with a tooltip and a popup menu
// (Shell_NotifyIconW). It has a hidden window of its own for the icon's events, on the UI thread,
// whose message loop (run()'s) delivers them.

#include "native.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace uniapi::tray {

namespace {

constexpr wchar_t kTrayClass[] = L"uniapi.tray";
constexpr UINT WM_APP_TRAY = WM_APP + 0x102;  // the icon's events, to the tray's window
constexpr UINT kTrayIconId = 1;

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    return wide;
}

// Copies text into a fixed UTF-16 field, cut to fit with its terminating zero.
template <size_t N>
void CopyText(wchar_t (&field)[N], const std::string& text) {
    const std::wstring wide = Wide(text);
    const size_t count = std::min(wide.size(), N - 1);
    std::memcpy(field, wide.data(), count * sizeof(wchar_t));
    field[count] = L'\0';
}

UINT TaskbarCreated() {  // Explorer started again: icons must be added again
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

class WinNative final : public Native {
public:
    explicit WinNative(std::function<bool(int)> clicked) : clicked_(std::move(clicked)) {}
    ~WinNative() override {
        Remove();
        if (icon_) DestroyIcon(icon_);
    }

    bool Create(std::string& error) override {
        if (window_) return true;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        static const bool registered = [instance] {
            WNDCLASSEXW windowClass = {sizeof(windowClass)};
            windowClass.lpfnWndProc = &WinNative::WindowProc;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = kTrayClass;
            return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        (void)registered;
        TaskbarCreated();
        // A hidden top-level window (not message-only: the popup menu needs one that can come to
        // the foreground, or it doesn't close when you click elsewhere).
        window_ = CreateWindowExW(0, kTrayClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!window_) {
            error = "no window for the tray icon (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!Add()) {
            error = "the notification area didn't take the icon (Shell_NotifyIcon)";
            return false;
        }
        return true;
    }

    // At the notification area's size. False if Windows can't read the PNG.
    bool SetIcon(const unsigned char* png, size_t size, bool) override {
        const UINT dpi = GetDpiForSystem();
        const int pixels = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        HICON icon = CreateIconFromResourceEx(const_cast<PBYTE>(png), DWORD(size), TRUE, 0x00030000, pixels, pixels,
                                              LR_DEFAULTCOLOR);
        if (!icon) return false;
        HICON old = icon_;
        icon_ = icon;
        if (added_) {
            NOTIFYICONDATAW data = Data();
            data.uFlags = NIF_ICON;
            data.hIcon = icon_;
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        if (old) DestroyIcon(old);
        return true;
    }

    void SetTooltip(const std::string& text) override {
        tooltip_ = text;
        if (!added_) return;
        NOTIFYICONDATAW data = Data();
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        CopyText(data.szTip, tooltip_);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void SetClickOpensMenu(bool) override {}  // asked of the Tray at each click

    // The menu is built when it opens.
    void SetMenu(const std::vector<std::string>& labels, const std::vector<int>& flags) override {
        labels_ = labels;
        flags_ = flags;
    }

    void Remove() override {
        if (added_) {
            NOTIFYICONDATAW data = Data();
            Shell_NotifyIconW(NIM_DELETE, &data);
            added_ = false;
        }
        if (window_) {
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    // In the notification area: Windows knows where it is.
    bool TestPlaced() override {
        if (!added_) return false;
        NOTIFYICONIDENTIFIER identifier = {sizeof(identifier)};
        identifier.hWnd = window_;
        identifier.uID = kTrayIconId;
        RECT where;
        return SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &where));
    }

    // The tray's side only: the menu itself is Windows', built when it opens.
    void TestChoose(int index) override {
        if (index >= 0 && index < int(labels_.size())) clicked_(index);
    }

private:
    NOTIFYICONDATAW Data() const {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIconId;
        return data;
    }

    // Puts the icon in the notification area (again, after Explorer restarted).
    bool Add() {
        if (!window_ || added_) return added_;
        NOTIFYICONDATAW data = Data();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = WM_APP_TRAY;
        data.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
        CopyText(data.szTip, tooltip_);
        if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
        data.uVersion = NOTIFYICON_VERSION_4;  // events as NIN_SELECT and WM_CONTEXTMENU, the place in wParam
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        added_ = true;
        return true;
    }

    // A click (NIN_SELECT, or Enter/Space: NIN_KEYSELECT) goes to the Tray, which may open the
    // menu; a right click (WM_CONTEXTMENU) opens it. x, y: where, in screen coordinates.
    void Event(UINT kind, int x, int y) {
        switch (kind) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (!clicked_(-1)) ShowMenu(x, y);
            break;
        case WM_CONTEXTMENU:
            ShowMenu(x, y);
            break;
        }
    }

    void ShowMenu(int x, int y) {
        if (labels_.empty() || !window_) return;
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        for (size_t i = 0; i < labels_.size(); ++i) {
            const int f = flags_[i];
            if (f & kSeparator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            UINT kind = MF_STRING;
            if (f & kChecked) kind |= MF_CHECKED;
            if (f & kDisabled) kind |= MF_GRAYED;
            AppendMenuW(menu, kind, UINT_PTR(i + 1), Wide(labels_[i]).c_str());
        }
        // The menu closes when you click elsewhere only if its window is in the foreground; the
        // message after it lets the menu end properly (as Microsoft's notes on TrackPopupMenu say).
        SetForegroundWindow(window_);
        const int chosen = int(TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, window_, nullptr));
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (chosen > 0) clicked_(chosen - 1);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WinNative*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
            if (message == WM_APP_TRAY) {
                self->Event(LOWORD(lParam), short(LOWORD(wParam)), short(HIWORD(wParam)));
                return 0;
            }
            if (message == TaskbarCreated() && TaskbarCreated() != 0) {
                self->added_ = false;
                self->Add();
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::function<bool(int)> clicked_;
    HWND window_ = nullptr;
    bool added_ = false;
    HICON icon_ = nullptr;
    std::string tooltip_;
    std::vector<std::string> labels_;
    std::vector<int> flags_;
};

}  // namespace

bool NativeAvailable() { return true; }

std::unique_ptr<Native> NewNative(std::function<bool(int)> clicked) { return std::make_unique<WinNative>(std::move(clicked)); }

}  // namespace uniapi::tray
