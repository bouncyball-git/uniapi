// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// Windows backend: Win32 windows hosting WebView2 (Chromium): the main window and further ones
// (App::openWindow), each a WebWindow, in one WebView2 environment (one browser process).
//
//   page <-> app messages   postMessage / WebMessageReceived, PostWebMessageAsString
//   page files and fetches  WebResourceRequested on https://<host>/
//   shared memory           ICoreWebView2SharedBuffer (WebView2 Runtime 1.0.1661 or newer)

#include "core/backend.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <wrl/ftm.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace uniapi {
namespace {

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), utf8.data(), length, nullptr,
                        nullptr);
    return utf8;
}

// The full path, also beyond MAX_PATH (long paths enabled); empty if it can't be read.
std::wstring ExecutablePathW() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {  // otherwise it was cut short: try a bigger buffer
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768) return {};  // the longest path Windows has
        path.resize(path.size() * 2);
    }
}

}  // namespace

std::string executablePath() { return Utf8(ExecutablePathW()); }

std::vector<std::string> arguments() {
    // From the UTF-16 command line: main()'s argv uses the ANSI code page, which can't hold
    // every character a path may contain.
    int count = 0;
    LPWSTR* list = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> result;
    for (int i = 1; list && i < count; ++i) result.push_back(Utf8(list[i]));
    LocalFree(list);
    return result;
}

void alert(std::string_view title, std::string_view message) {
    MessageBoxW(nullptr, Wide(message).c_str(), Wide(title).c_str(), MB_ICONINFORMATION);
}

namespace detail {
namespace {

constexpr UINT WM_APP_POSTED = WM_APP + 0x100;
constexpr UINT WM_APP_NOTICE = WM_APP + 0x101;  // the notification icon's events, to the main window
constexpr UINT kNoticeIconId = 1;
constexpr wchar_t kWindowClass[] = L"uniapi.window";

// The program's icon: its first icon group, at the title bar's and Alt+Tab's sizes (none when
// the program has no icon resource: Windows then shows its generic one).
struct ProgramIcons {
    HICON big = nullptr;
    HICON little = nullptr;
};

BOOL CALLBACK LoadFirstIcon(HMODULE module, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto* icons = reinterpret_cast<ProgramIcons*>(param);
    icons->big = static_cast<HICON>(LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                               GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    icons->little = static_cast<HICON>(LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    return FALSE;  // the first one only
}

class WindowsRegion final : public SharedRegion {
public:
    ComPtr<ICoreWebView2SharedBuffer> buffer;
    ~WindowsRegion() override {
        if (buffer) buffer->Close();
    }
};

// Read-only stream over memory owned by keepAlive. WebView2 reads responses on its own
// threads: FtmBase makes the object free-threaded, so those reads call straight in instead
// of being marshaled back to the UI thread one Read() at a time (that cost ~70 ms per 8 MB).
// The reference count is thread-safe and the data doesn't change while it's read.
class MemoryStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                         ChainInterfaces<IStream, ISequentialStream>, FtmBase> {
public:
    MemoryStream(const uint8_t* data, size_t size, std::shared_ptr<const void> keepAlive)
        : data_(data), size_(size), keepAlive_(std::move(keepAlive)) {}

    STDMETHODIMP Read(void* buffer, ULONG bytes, ULONG* read) override {
        size_t count = position_ < size_ ? std::min<size_t>(bytes, size_ - position_) : 0;
        if (count) std::memcpy(buffer, data_ + position_, count);
        position_ += count;
        if (read) *read = ULONG(count);
        return count < bytes ? S_FALSE : S_OK;
    }
    STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        LONGLONG base = origin == STREAM_SEEK_SET   ? 0
                        : origin == STREAM_SEEK_CUR ? LONGLONG(position_)
                        : origin == STREAM_SEEK_END ? LONGLONG(size_)
                                                    : -1;
        LONGLONG target = base + move.QuadPart;
        if (base < 0 || target < 0) return STG_E_INVALIDFUNCTION;
        position_ = size_t(target);
        if (result) result->QuadPart = ULONGLONG(position_);
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = ULONGLONG(size_);
        return S_OK;
    }
    STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
    std::shared_ptr<const void> keepAlive_;
};

// Reads a request body (POST data) to the end.
void ReadAll(IStream* stream, std::vector<uint8_t>& out) {
    constexpr ULONG kChunk = 1 << 20;
    for (;;) {
        const size_t used = out.size();
        out.resize(used + kChunk);
        ULONG read = 0;
        const HRESULT hr = stream->Read(out.data() + used, kChunk, &read);
        out.resize(used + read);
        if (FAILED(hr) || read == 0) break;
    }
}

// A dark window's own background (at its edges while it resizes): Windows' dark title bar grey.
constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
// The window is cloaked until its first page has loaded, then shown after kRevealAfterLoad (the
// page's first frame reaching the screen), or after kRevealTimeout whatever happens.
constexpr UINT_PTR kRevealTimer = 0x75A1;
constexpr UINT kRevealAfterLoad = 50;   // ms
constexpr UINT kRevealTimeout = 3000;   // ms
constexpr UINT kRevealTimeoutReady = 5000;  // ms, when the page shows it (showWhenReady)

// Makes the page take dropped files (a dragover that accepts them, so the browser doesn't open the
// file instead) and hands them to the app as File objects, which WebView2 gives the app with
// their paths. In the top frame only, before the page's own scripts; the page's drop handlers
// run as usual.
constexpr wchar_t kDropMessage[] = L"$uniapi.drop:";  // then "x,y"; the files come as additional objects
constexpr wchar_t kDropScript[] =
    L"(() => {"
    L"  if (window.top !== window || !window.chrome || !chrome.webview || !chrome.webview.postMessageWithAdditionalObjects) return;"
    L"  const files = (e) => e.dataTransfer && Array.from(e.dataTransfer.types || []).includes('Files');"
    L"  addEventListener('dragover', (e) => { if (files(e)) e.preventDefault(); }, true);"
    L"  addEventListener('drop', (e) => {"
    L"    if (!files(e)) return;"
    L"    e.preventDefault();"
    L"    if (e.dataTransfer.files.length) {"
    L"      chrome.webview.postMessageWithAdditionalObjects('$uniapi.drop:' + Math.round(e.clientX) + ',' + Math.round(e.clientY), e.dataTransfer.files);"
    L"    }"
    L"  }, true);"
    L"})();";

// The switches the web engine starts with: AppOptions::browserArguments and, unless
// deferTasksAfterInput, Chromium's deferral of tasks after an input switched off (it holds C++'s
// messages until the next frame after a click or key: a frame later). Chromium keeps only the
// last --disable-features, so the feature joins one the app gave rather than adding another.
std::string BrowserArguments(const AppOptions& options) {
    std::string arguments = options.browserArguments;
    if (options.deferTasksAfterInput) return arguments;
    const std::string feature = "DeferRendererTasksAfterInput";
    const std::string key = "--disable-features=";
    const size_t at = arguments.find(key);
    if (at != std::string::npos) {
        size_t end = arguments.find(' ', at);
        if (end == std::string::npos) end = arguments.size();
        arguments.insert(end, (end > at + key.size() ? "," : "") + feature);
    } else {
        arguments += (arguments.empty() ? "" : " ") + key + feature;
    }
    return arguments;
}

class WindowsBackend;

// Whether the system's apps are set to dark (Settings > Personalization > Colors).
bool SystemAppsDark() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return status == ERROR_SUCCESS && light == 0;
}

HICON MakeIcon(const unsigned char* png, size_t size, int pixels) {
    return CreateIconFromResourceEx(const_cast<PBYTE>(png), DWORD(size), TRUE, 0x00030000, pixels, pixels,
                                    LR_DEFAULTCOLOR);
}

// AppOptions::contextMenu off: WebView2's right-click menu keeps its text editing items only,
// with separators only between them; no menu at all when none is left.
void KeepEditingItems(ICoreWebView2ContextMenuRequestedEventArgs* args) {
    static const wchar_t* const kEditing[] = {L"undo", L"redo", L"cut", L"copy", L"paste", L"pasteAndMatchStyle",
                                              L"selectAll"};
    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
    if (FAILED(args->get_MenuItems(&items)) || !items) return;
    UINT32 count = 0;
    items->get_Count(&count);
    UINT32 kept = 0;             // items (not separators) left
    bool afterSeparator = true;  // (or at the start): no separator may follow
    for (UINT32 i = 0; i < count;) {
        ComPtr<ICoreWebView2ContextMenuItem> item;
        COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind = COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND;
        bool keep = false;
        if (SUCCEEDED(items->GetValueAtIndex(i, &item)) && item && SUCCEEDED(item->get_Kind(&kind))) {
            if (kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR) {
                keep = !afterSeparator;
            } else {
                LPWSTR name = nullptr;
                if (SUCCEEDED(item->get_Name(&name)) && name) {
                    for (const wchar_t* editing : kEditing) keep = keep || wcscmp(name, editing) == 0;
                    CoTaskMemFree(name);
                }
            }
        }
        if (!keep) {
            items->RemoveValueAtIndex(i);
            --count;
            continue;
        }
        afterSeparator = kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR;
        if (!afterSeparator) ++kept;
        ++i;
    }
    if (count && afterSeparator) items->RemoveValueAtIndex(count - 1);  // a separator at the end
    if (kept == 0) args->put_Handled(TRUE);                              // nothing to show: no menu
}

// One window with its WebView2 web view: the main window, or a further one (App::openWindow).
class WebWindow final : public BackendWindow {
public:
    struct Setup {
        std::string title;
        int width = 800;
        int height = 600;
        int minWidth = 0;
        int minHeight = 0;
        bool resizable = true;
        bool keepAspectRatio = false;
        bool fullscreen = false;
        TitleBar titleBar = TitleBar::System;
        bool showWhenReady = false;
        bool activate = true;
        bool hidden = false; // hide(), startHidden: not shown until SetVisible
        bool main = false;   // the app's main window: closing it ends the app
        bool owned = false;  // owned by the main window
        bool modal = false;  // the main window takes no input while it is open
        bool devTools = false;  // AppOptions::devTools: F12, "Inspect", OpenDevTools
        bool contextMenu = true;  // AppOptions::contextMenu: the right-click menu in full
        int maxWidth = 0;       // 0: no maximum
        int maxHeight = 0;
        int background = -1;    // 0xRRGGBB behind the page, or -1 (ColorValue)
    };

    WebWindow(WindowsBackend& backend, Setup setup, BackendEvents events)
        : backend_(backend), setup_(std::move(setup)), events_(std::move(events)) {}

    ~WebWindow() override;

    HWND Handle() const { return window_; }
    const Setup& GetSetup() const { return setup_; }
    bool Started() const { return started_; }

    // The native window, cloaked until its page is drawn; `owner` for an owned window.
    bool Create(HWND owner);
    // Its web view, in the app's WebView2 environment (once it exists).
    void Start(ICoreWebView2Environment* environment);

    void Navigate(const std::string& path) override;
    void PostToPage(const std::string& text) override {
        if (webview_) webview_->PostWebMessageAsString(Wide(text).c_str());
    }
    void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) override {
        auto& windowsRegion = static_cast<WindowsRegion&>(region);
        if (!webview17_) return;
        // A sink's slot: the page writes, the app reads that very memory. A stream's: read-only.
        webview17_->PostSharedBufferToScript(windowsRegion.buffer.Get(),
                                             writable ? COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_WRITE
                                                      : COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
                                             Wide(infoJson).c_str());
    }
    bool SharedBuffers() const { return webview17_ != nullptr; }

    void SetTitle(const std::string& title) override {
        setup_.title = title;
        if (window_) SetWindowTextW(window_, Wide(title).c_str());
    }

    void SetSize(int width, int height) override {
        setup_.width = width;  // also the shape keepAspectRatio keeps (WM_SIZING)
        setup_.height = height;
        HWND window = window_;
        if (!window || fullscreen_) return;  // before it exists: it opens at this size
        if (IsZoomed(window)) ShowWindow(window, SW_RESTORE);
        const UINT dpi = GetDpiForWindow(window);
        RECT frame = {0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
        AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, dpi);
        SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Full screen: a borderless popup covering its monitor; leaving restores style and place.
    void SetFullscreen(bool on) override {
        HWND window = window_;
        if (!window) {
            setup_.fullscreen = on;  // applied when the window opens
            return;
        }
        if (on == fullscreen_) return;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        // Set before the window changes: its WM_SIZE and WM_MOVE mustn't take the full-screen frame
        // as the normal one (TrackNormal).
        fullscreen_ = on;
        if (on) {
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(window, &savedPlacement_);
            MONITORINFO monitor = {sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(window, GWL_STYLE, (style & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP);
            const RECT& area = monitor.rcMonitor;
            SetWindowPos(window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, (style & ~LONG_PTR(WS_POPUP)) | WindowStyle());
            SetWindowPlacement(window, &savedPlacement_);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                             SWP_NOACTIVATE);
            TrackNormal();
        }
    }

    bool IsFullscreen() const override { return window_ ? fullscreen_ : setup_.fullscreen; }

    void SetTitleBar(TitleBar mode) override {
        setup_.titleBar = mode;
        ApplyTitleBar();  // before it exists: when the window opens
    }

    TitleBar GetTitleBar() const override { return setup_.titleBar; }

    void Show() override {
        if (window_) Reveal();
    }

    // Shows the window again (in front, restored if minimised) or hides it; the page runs
    // either way. A modal window holds its owner only while it is shown. Before the window
    // exists: how it opens.
    void SetVisible(bool visible) override {
        setup_.hidden = !visible;
        HWND window = window_;
        if (!window) return;
        const bool modalOwner = setup_.modal && owner_;
        if (!visible) {
            if (modalOwner) EnableWindow(owner_, TRUE);
            ShowWindow(window, SW_HIDE);
            return;
        }
        if (!everShown_) {
            everShown_ = true;
            // Its page may have been revealed while the window was hidden (the timeout), without
            // a frame drawn: cloaked again until it has had a moment to paint. Otherwise the
            // reveal still pending shows it.
            if (revealed_) {
                revealed_ = false;
                const BOOL cloak = TRUE;
                DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
                SetTimer(window, kRevealTimer, kRevealAfterLoad, nullptr);
            }
        }
        if (modalOwner) EnableWindow(owner_, FALSE);
        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        } else {
            ShowWindow(window, setup_.activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        }
        if (setup_.activate) {
            SetForegroundWindow(window);
        } else {
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (place_.pending) {  // minimize(), maximize() while it was hidden
            place_.pending = false;
            ApplyState();
        }
    }

    bool IsVisible() const override { return !setup_.hidden; }

    // --- Placement ---
    // Windows works in physical pixels on the virtual desktop (the primary monitor's top-left
    // corner is 0,0); uniapi's are logical: divided by the window's DPI scale (a window spanning
    // monitors of different scales has one: its own). A position is the window rectangle's
    // top-left corner (GetWindowRect: with Windows 10's invisible resize borders, so what
    // SetPosition takes Position gives back).
    UINT Dpi() const {
        HWND window = window_;
        return window ? GetDpiForWindow(window) : GetDpiForSystem();
    }
    static int ToLogical(LONG physical, UINT dpi) { return int((int64_t(physical) * 96 + dpi / 2) / dpi); }

    Size CurrentSize() const override {
        HWND window = window_;
        if (!window) return {setup_.width, setup_.height};
        RECT client;
        GetClientRect(window, &client);
        const UINT dpi = GetDpiForWindow(window);
        return {ToLogical(client.right - client.left, dpi), ToLogical(client.bottom - client.top, dpi)};
    }

    std::optional<Point> Position() const override {
        HWND window = window_;
        if (!window) return place_.hasPosition ? std::optional<Point>(Point{place_.x, place_.y}) : std::nullopt;
        RECT frame;
        GetWindowRect(window, &frame);
        const UINT dpi = GetDpiForWindow(window);
        return Point{ToLogical(frame.left, dpi), ToLogical(frame.top, dpi)};
    }

    bool SetPosition(int x, int y) override {
        place_.x = x;
        place_.y = y;
        place_.hasPosition = true;
        place_.center = false;
        HWND window = window_;
        if (window && !fullscreen_) {
            const UINT dpi = GetDpiForWindow(window);
            SetWindowPos(window, nullptr, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return true;
    }

    // On its monitor's work area (without the taskbar).
    void Center() override {
        place_.hasPosition = false;
        place_.center = true;
        HWND window = window_;
        if (!window || fullscreen_) return;  // centred once made (Create)
        RECT frame;
        GetWindowRect(window, &frame);
        MONITORINFO monitor = {sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& work = monitor.rcWork;
        const LONG width = frame.right - frame.left, height = frame.bottom - frame.top;
        const LONG left = std::max(work.left, work.left + (work.right - work.left - width) / 2);
        const LONG top = std::max(work.top, work.top + (work.bottom - work.top - height) / 2);
        SetWindowPos(window, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    WindowState State() const override {
        HWND window = window_;
        if (!window || setup_.hidden) return place_.pending ? place_.state : WindowState::Normal;
        if (IsIconic(window)) return WindowState::Minimized;
        return IsZoomed(window) ? WindowState::Maximized : WindowState::Normal;
    }

    // On a window shown; a hidden one (or one not yet made) takes it when shown.
    void SetState(WindowState state) override {
        place_.state = state;
        if (!window_ || setup_.hidden) {
            place_.pending = true;
            return;
        }
        place_.pending = false;
        ApplyState();
    }

    Bounds NormalBounds() const override {
        HWND window = window_;
        if (!window || !place_.hasNormal) {
            const Size size = CurrentSize();
            const std::optional<Point> point = Position();
            return {point ? point->x : 0, point ? point->y : 0, size.width, size.height, point.has_value()};
        }
        const UINT dpi = GetDpiForWindow(window);
        // The page area's size within that rectangle: the frame's size taken off.
        RECT frame = {0, 0, 0, 0};
        AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, dpi);
        const RECT& n = place_.normal;
        return {ToLogical(n.left, dpi), ToLogical(n.top, dpi),
                ToLogical(n.right - n.left - (frame.right - frame.left), dpi),
                ToLogical(n.bottom - n.top - (frame.bottom - frame.top), dpi), true};
    }

    bool SetAlwaysOnTop(bool on) override {
        place_.onTop = on;
        if (HWND window = window_) {
            SetWindowPos(window, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return true;
    }

    bool IsAlwaysOnTop() const override {
        HWND window = window_;
        if (!window) return place_.onTop;
        return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    }

    // Each monitor's whole area at its own scale, the one most of the window is on marked
    // current (none before the window exists).
    std::vector<Screen> Screens() const override {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&monitors));
        HWND window = window_;
        HMONITOR current = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
        std::vector<Screen> screens;
        for (HMONITOR monitor : monitors) {
            MONITORINFO info = {sizeof(info)};
            if (!GetMonitorInfoW(monitor, &info)) continue;
            UINT dpiX = 96, dpiY = 96;
            if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) dpiX = 96;
            const RECT& area = info.rcMonitor;
            Screen screen;
            screen.x = ToLogical(area.left, dpiX);
            screen.y = ToLogical(area.top, dpiX);
            screen.width = ToLogical(area.right - area.left, dpiX);
            screen.height = ToLogical(area.bottom - area.top, dpiX);
            screen.scale = double(dpiX) / 96;
            screen.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            screen.current = current && monitor == current;
            screens.push_back(screen);
        }
        return screens;
    }

    bool SetIcon(const unsigned char* png, size_t size) override {
        // Windows reads PNG data as an icon image, scaled to each size it needs.
        const UINT dpi = GetDpiForSystem();
        // (Not "small": the Windows headers define that as a macro.)
        HICON large = MakeIcon(png, size, GetSystemMetricsForDpi(SM_CXICON, dpi));
        HICON little = MakeIcon(png, size, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
        if (!large || !little) {
            if (large) DestroyIcon(large);
            if (little) DestroyIcon(little);
            return false;
        }
        std::swap(large, bigIcon_);
        std::swap(little, smallIcon_);
        ApplyIcon();
        if (large) DestroyIcon(large);  // the previous icons, no longer used by the window
        if (little) DestroyIcon(little);
        return true;
    }
    HICON BigIcon() const { return bigIcon_; }
    HICON SmallIcon() const { return smallIcon_; }

    // Closes it without asking onCloseRequested (its WM_CLOSE knows: closeAsked_).
    // WebView2's DevTools window (what F12 opens); nothing without AppOptions::devTools.
    void OpenDevTools() override {
        if (webview_ && setup_.devTools) webview_->OpenDevToolsWindow();
    }

    void Close() override {
        HWND window = window_;
        if (!window) return;
        closeAsked_ = true;
        PostMessageW(window, WM_CLOSE, 0, 0);
    }

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // Each monitor's work area (without the taskbar), at its own scale.
    static std::vector<Rect> ScreenAreas() {
        std::vector<Rect> areas;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                MONITORINFO info = {sizeof(info)};
                if (!GetMonitorInfoW(monitor, &info)) return TRUE;
                UINT dpiX = 96, dpiY = 96;
                if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) dpiX = 96;
                const RECT& work = info.rcWork;
                reinterpret_cast<std::vector<Rect>*>(data)->push_back(
                    {ToLogical(work.left, dpiX), ToLogical(work.top, dpiX), ToLogical(work.right - work.left, dpiX),
                     ToLogical(work.bottom - work.top, dpiX)});
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&areas));
        return areas;
    }

    void Fail(const wchar_t* what, HRESULT hr) {
        wchar_t text[512];
        swprintf_s(text, L"%s\n\nHRESULT 0x%08X", what, static_cast<unsigned>(hr));
        MessageBoxW(window_, text, Wide(setup_.title).c_str(), MB_ICONERROR);
    }

private:
    DWORD WindowStyle() const {
        return setup_.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    }

    void ApplyIcon();

    void ApplyState() {
        HWND window = window_;
        if (!window) return;
        switch (place_.state) {
        case WindowState::Minimized:
            ShowWindow(window, SW_MINIMIZE);
            break;
        case WindowState::Maximized:
            ShowWindow(window, SW_SHOWMAXIMIZED);
            break;
        case WindowState::Normal:
            if (IsIconic(window) || IsZoomed(window)) ShowWindow(window, SW_RESTORE);
            break;
        }
    }

    // The window rectangle while it is normal (not minimised, maximised or full screen); from
    // WM_MOVE and WM_SIZE.
    void TrackNormal() {
        HWND window = window_;
        if (!window || fullscreen_ || IsIconic(window) || IsZoomed(window)) return;
        GetWindowRect(window, &place_.normal);
        place_.hasNormal = true;
    }

    // Uncloaks the window (see Create): once, after its first page has loaded (or the page said
    // so: showWhenReady), or the timeout.
    void Reveal() {
        if (!window_) return;
        KillTimer(window_, kRevealTimer);
        if (revealed_) return;
        revealed_ = true;
        const BOOL cloak = FALSE;
        DwmSetWindowAttribute(window_, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }

    // Dark or light title bar and frame (DWM). Attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE
    // (Windows 10 20H1 and 11); builds before 20H1 took 19. Older Windows: no dark title bars.
    void ApplyTitleBar() {
        if (!window_) return;
        const TitleBar mode = setup_.titleBar;
        const BOOL dark = mode == TitleBar::Dark || (mode == TitleBar::System && SystemAppsDark());
        dark_ = dark != FALSE;
        if (FAILED(DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark)))) {
            DwmSetWindowAttribute(window_, 19, &dark, sizeof(dark));
        }
        InvalidateRect(window_, nullptr, TRUE);  // the window's own background too (WM_ERASEBKGND)
        // Repaint the frame now (Windows 10 otherwise waits for the next activation).
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HRESULT OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);

    // A drop message (kDropScript): its files' paths (ICoreWebView2File), and where ("x,y").
    void OnDropMessage(ICoreWebView2WebMessageReceivedEventArgs* args, const std::wstring& where) {
        int x = 0, y = 0;
        swscanf_s(where.c_str(), L"%d,%d", &x, &y);
        ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
        ComPtr<ICoreWebView2ObjectCollectionView> objects;
        if (FAILED(args->QueryInterface(IID_PPV_ARGS(&args2))) || FAILED(args2->get_AdditionalObjects(&objects)) || !objects) {
            return;  // a runtime without additional objects (before 1.0.1185)
        }
        UINT32 count = 0;
        objects->get_Count(&count);
        std::vector<std::string> paths;
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IUnknown> item;
            ComPtr<ICoreWebView2File> file;
            if (FAILED(objects->GetValueAtIndex(i, &item)) || !item || FAILED(item.As(&file))) continue;
            LPWSTR path = nullptr;
            if (SUCCEEDED(file->get_Path(&path)) && path && *path) paths.push_back(Utf8(path));
            CoTaskMemFree(path);
        }
        if (!paths.empty() && events_.onFileDrop) events_.onFileDrop(paths, x, y);
    }

    WindowsBackend& backend_;
    Setup setup_;
    BackendEvents events_;
    HWND owner_ = nullptr;
    std::atomic<HWND> window_{nullptr};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed
    bool silent_ = false;    // being destroyed by its owner object: no events
    bool closeAsked_ = false;  // Close: its WM_CLOSE doesn't ask onCloseRequested
    bool everShown_ = false;   // it has been on screen (a window started hidden: once shown)
    bool started_ = false;   // its web view is being made
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_ = {sizeof(WINDOWPLACEMENT)};
    // Placement: before the window exists, how it opens; then its normal rectangle, tracked.
    struct Place {
        bool hasPosition = false;
        int x = 0, y = 0;  // logical: the window rectangle's top-left
        bool center = false;
        WindowState state = WindowState::Normal;  // the state asked for last
        bool pending = false;  // asked for while hidden or before it existed: taken when shown
        bool onTop = false;
        RECT normal = {};      // physical window rectangle, while normal
        bool hasNormal = false;
    } place_;
    HICON bigIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    bool dark_ = false;      // the title bar is dark: so is the window's own background
    bool revealed_ = false;  // uncloaked (Reveal)
    ComPtr<ICoreWebView2> webview_;
    ComPtr<ICoreWebView2_17> webview17_;
};

class WindowsBackend final : public Backend {
public:
    WindowsBackend(const AppOptions& options, BackendEvents events)
        : options_(options), origin_(L"https://" + Wide(options.host)), uiThread_(GetCurrentThreadId()) {
        // WebView2 needs a single-threaded apartment on the UI thread.
        comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSEXW windowClass = {sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // The program's own icon (its first icon resource: what Explorer and the taskbar show;
        // CMake's uniapi_icon() puts one there) in every window's title bar, unless setIcon
        // gives the window another.
        ProgramIcons icons;
        EnumResourceNamesW(windowClass.hInstance, RT_GROUP_ICON, LoadFirstIcon, reinterpret_cast<LONG_PTR>(&icons));
        windowClass.hIcon = icons.big;
        windowClass.hIconSm = icons.little;
        programIcons_ = icons;  // also the notification's
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        RegisterClassExW(&windowClass);  // once per process; again is a harmless failure
        WebWindow::Setup setup;
        setup.title = options.title;
        setup.width = options.width;
        setup.height = options.height;
        setup.minWidth = options.minWidth;
        setup.minHeight = options.minHeight;
        setup.resizable = options.resizable;
        setup.keepAspectRatio = options.keepAspectRatio;
        setup.fullscreen = options.fullscreen;
        setup.titleBar = options.titleBar;
        setup.showWhenReady = options.showWhenReady;
        setup.activate = options.activate;
        setup.hidden = options.startHidden;
        setup.main = true;
        setup.devTools = options.devTools;
        setup.contextMenu = options.contextMenu;
        setup.maxWidth = options.maxWidth;
        setup.maxHeight = options.maxHeight;
        setup.background = ColorValue(options.backgroundColor);
        main_ = std::make_unique<WebWindow>(*this, std::move(setup), std::move(events));
    }

    ~WindowsBackend() override {
        RemoveNoticeIcon();  // before the main window: it belongs to it
        for (auto& entry : timers_) CloseHandle(entry.second.handle);
        *alive_ = false;  // WebView2 callbacks still pending must not touch this object
        // Every WebView2 object goes before COM shuts down on this thread (further windows are
        // destroyed by the core first).
        main_.reset();
        environment12_.Reset();
        environment_.Reset();
        if (comInitialized_) CoUninitialize();
    }

    const std::wstring& Origin() const { return origin_; }
    HWND MainHandle() const { return main_ ? main_->Handle() : nullptr; }
    WebWindow& Main() { return *main_; }
    ICoreWebView2Environment* Environment() const { return environment_.Get(); }
    void Register(WebWindow* window) { others_.push_back(window); }
    void Unregister(WebWindow* window) { others_.erase(std::remove(others_.begin(), others_.end(), window), others_.end()); }

    int Run() override {
        if (!main_->Create(nullptr)) return 1;
        running_ = true;
        // Further windows opened before run(): now, with (and owned by) the main window.
        for (WebWindow* window : std::vector<WebWindow*>(others_)) {
            if (!window->Handle()) window->Create(window->GetSetup().owned ? MainHandle() : nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(postMutex_);
            if (!posted_.empty()) PostMessageW(MainHandle(), WM_APP_POSTED, 0, 0);
        }
        CreateEnvironment();

        // Sleeps until a window message arrives or a timer is due, keeping everything on this
        // thread as WebView2 requires. Under heavy WebView2 traffic the queue never runs dry and
        // MsgWait keeps reporting input rather than signaled timers, so each pass checks every
        // timer itself and handles messages in bounded batches; otherwise timers starve.
        constexpr int kMessagesPerPass = 64;
        MSG msg = {};
        for (;;) {
            std::vector<HANDLE> handles;
            std::vector<int> ids;
            for (auto& entry : timers_) {
                ids.push_back(entry.first);
                handles.push_back(entry.second.handle);
            }
            const DWORD count = DWORD(handles.size());
            const DWORD wait = MsgWaitForMultipleObjectsEx(count, handles.data(), INFINITE, QS_ALLINPUT,
                                                           MWMO_INPUTAVAILABLE);
            for (DWORD i = 0; i < count; ++i) {
                // An earlier callback in this pass may have cancelled this timer, closing its
                // handle: look it up before touching the handle.
                auto it = timers_.find(ids[i]);
                if (it == timers_.end()) continue;
                // The wait already reset the timer it reported; poll (and reset) the others.
                const bool due =
                    wait == WAIT_OBJECT_0 + i || WaitForSingleObject(it->second.handle, 0) == WAIT_OBJECT_0;
                if (due) {
                    // Held by a shared_ptr copy: the callback may cancel its own timer, and its
                    // state (a mutable lambda's captures) lasts from tick to tick.
                    std::shared_ptr<std::function<void()>> fn = it->second.fn;
                    (*fn)();
                }
            }
            for (int i = 0; i < kMessagesPerPass && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
                if (msg.message == WM_QUIT) {
                    WaitForBrowserExit();
                    return int(msg.wParam);
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    void Quit() override { main_->Close(); }

    // The main window is going: further windows that aren't owned by it close too (owned ones
    // Windows destroys with it), then the loop ends.
    void MainDestroyed() {
        for (WebWindow* window : std::vector<WebWindow*>(others_)) {
            if (window->Handle() && !window->GetSetup().owned) DestroyWindow(window->Handle());
        }
        PostQuitMessage(0);
    }

    // After the window closed (its controller is closed), waits for WebView2's browser process
    // to exit, pumping messages meanwhile; it takes about 0.3 s. Leaving earlier left it shutting
    // down after the app had gone, and the same app started again in that moment (a test run, a
    // quick restart) found its profile's browser still dying and stalled for about 6 s before
    // its page connected. Bounded: if the browser takes longer, the app leaves anyway. (A new
    // profile does first-time setup in the background that keeps its browser up about 6 s
    // after each run, until one run lasts long enough to finish it, up to a minute.)
    void WaitForBrowserExit() {
        constexpr ULONGLONG kMaxWaitMs = 1500;
        ComPtr<ICoreWebView2Environment5> environment5;
        if (!environment_ || FAILED(environment_.As(&environment5))) return;
        const ULONGLONG start = GetTickCount64();
        bool exited = false;
        EventRegistrationToken token;
        environment5->add_BrowserProcessExited(
            Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                [&exited](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
                    exited = true;
                    return S_OK;
                }).Get(),
            &token);
        MSG msg;
        while (!exited && GetTickCount64() - start < kMaxWaitMs) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        environment5->remove_BrowserProcessExited(token);
    }

    void Navigate(const std::string& path) override { main_->Navigate(path); }
    void PostToPage(const std::string& text) override { main_->PostToPage(text); }

    // Shared buffers come from the environment, so every window's web view can take them.
    std::unique_ptr<SharedRegion> CreateSharedRegion(size_t bytes) override {
        if (!environment12_ || !main_->SharedBuffers()) return nullptr;
        auto region = std::make_unique<WindowsRegion>();
        if (FAILED(environment12_->CreateSharedBuffer(bytes, &region->buffer))) return nullptr;
        BYTE* data = nullptr;
        if (FAILED(region->buffer->get_Buffer(&data))) return nullptr;
        region->data = data;
        region->size = bytes;
        return region;
    }

    void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) override {
        main_->ShareRegion(region, infoJson, writable);
    }

    void Post(std::function<void()> fn) override {
        bool wasEmpty;
        {
            std::lock_guard<std::mutex> lock(postMutex_);
            wasEmpty = posted_.empty();
            posted_.push_back(std::move(fn));
        }
        HWND window = mainHandle_;
        if (wasEmpty && window) PostMessageW(window, WM_APP_POSTED, 0, 0);
    }
    void SetMainHandle(HWND window) { mainHandle_ = window; }

    void RunPosted() {
        std::vector<std::function<void()>> posted;
        {
            std::lock_guard<std::mutex> lock(postMutex_);
            posted.swap(posted_);
        }
        for (auto& fn : posted) fn();
    }

    bool OnUiThread() const override { return GetCurrentThreadId() == uiThread_; }

    void SetTitle(const std::string& title) override { main_->SetTitle(title); }
    void SetSize(int width, int height) override { main_->SetSize(width, height); }
    void SetFullscreen(bool on) override { main_->SetFullscreen(on); }
    bool IsFullscreen() const override { return main_->IsFullscreen(); }
    void SetTitleBar(TitleBar mode) override { main_->SetTitleBar(mode); }
    TitleBar GetTitleBar() const override { return main_->GetTitleBar(); }
    void Show() override { main_->Show(); }
    void SetVisible(bool visible) override { main_->SetVisible(visible); }
    bool IsVisible() const override { return main_->IsVisible(); }
    bool SetIcon(const unsigned char* png, size_t size) override { return main_->SetIcon(png, size); }
    void OpenDevTools() override { main_->OpenDevTools(); }
    Size CurrentSize() const override { return main_->CurrentSize(); }
    std::optional<Point> Position() const override { return main_->Position(); }
    bool SetPosition(int x, int y) override { return main_->SetPosition(x, y); }
    void Center() override { main_->Center(); }
    WindowState State() const override { return main_->State(); }
    void SetState(WindowState state) override { main_->SetState(state); }
    Bounds NormalBounds() const override { return main_->NormalBounds(); }
    bool SetAlwaysOnTop(bool on) override { return main_->SetAlwaysOnTop(on); }
    bool IsAlwaysOnTop() const override { return main_->IsAlwaysOnTop(); }
    std::vector<Screen> Screens() const override { return main_->Screens(); }
    std::vector<Rect> ScreenAreas() const override { return WebWindow::ScreenAreas(); }
    // The WebView2 runtime the environment started with; no version before it has.
    WebEngine Engine() const override {
        WebEngine engine;
        engine.name = "WebView2";
        engine.version = engineVersion_;
        return engine;
    }

    // The clipboard (CF_UNICODETEXT). It needs an owner window (EmptyClipboard without one makes
    // SetClipboardData fail): the main window, so once run() has made it.
    void ClipboardText(std::function<void(std::optional<std::string>)> done) override {
        std::optional<std::string> text;
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboardForAWhile(MainHandle())) {
            if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* units = static_cast<const wchar_t*>(GlobalLock(handle))) {
                    text = Utf8(units);
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        done(std::move(text));
    }

    bool SetClipboardText(const std::string& text) override {
        HWND owner = MainHandle();
        if (!owner) return false;
        std::wstring wide = Wide(text);
        wide.erase(std::remove(wide.begin(), wide.end(), L'\0'), wide.end());
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) return false;
        void* data = GlobalLock(memory);
        if (!data) {
            GlobalFree(memory);
            return false;
        }
        std::memcpy(data, wide.c_str(), bytes);
        GlobalUnlock(memory);
        if (!OpenClipboardForAWhile(owner)) {
            GlobalFree(memory);
            return false;
        }
        EmptyClipboard();
        const bool taken = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        if (!taken) GlobalFree(memory);  // not taken: still ours
        CloseClipboard();
        return taken;
    }

    // --- Notifications ---
    // A balloon from an icon in the notification area (Shell_NotifyIconW), which Windows 10 and
    // 11 show as a notification. A toast proper needs an AppUserModelID registered with a Start
    // menu shortcut, which a program that isn't installed lacks; a balloon needs neither. The
    // icon (the program's) is added for the balloon and removed once it is clicked or gone, and
    // when the app ends. One balloon at a time: a new one replaces the last.
    void Notify(const Notification& n, std::function<void(const std::string&)> done) override {
        HWND owner = MainHandle();
        if (!owner) {
            done("the app has no window to show it from");
            return;
        }
        // The main window's icon if setIcon gave it one, else the program's; else Windows' own.
        HICON little = main_->SmallIcon(), big = main_->BigIcon();
        if (!little) {
            little = programIcons_.little;
            big = programIcons_.big;
        }
        if (!little) little = LoadIconW(nullptr, IDI_APPLICATION);
        if (!noticeAdded_) {
            NOTIFYICONDATAW data = NoticeData(owner);
            data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            data.uCallbackMessage = WM_APP_NOTICE;
            data.hIcon = little;
            CopyText(data.szTip, options_.title);
            if (!Shell_NotifyIconW(NIM_ADD, &data)) {
                done("the notification area didn't take the icon (Shell_NotifyIcon)");
                return;
            }
            data.uVersion = NOTIFYICON_VERSION_4;  // events in lParam's low word (NoticeEvent)
            Shell_NotifyIconW(NIM_SETVERSION, &data);
            noticeAdded_ = true;
        }
        NOTIFYICONDATAW data = NoticeData(owner);
        data.uFlags = NIF_INFO;
        CopyText(data.szInfoTitle, n.title);
        CopyText(data.szInfo, n.body.empty() ? std::string(" ") : n.body);  // an empty text removes the balloon
        data.dwInfoFlags = NIIF_USER;
        data.hBalloonIcon = little;
        if (big) {
            data.dwInfoFlags |= NIIF_LARGE_ICON;
            data.hBalloonIcon = big;
        }
        if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
            done("the notification area didn't take the notification (Shell_NotifyIcon)");
            return;
        }
        noticeId_ = n.id;
        done("");
    }

    // The icon's event (NOTIFYICON_VERSION_4: in lParam's low word). A click on the balloon is a
    // click on the notification; once it is gone, so is the icon.
    void NoticeEvent(LPARAM lParam, const BackendEvents& mainEvents) {
        switch (LOWORD(lParam)) {
        case NIN_BALLOONUSERCLICK: {
            const std::string id = noticeId_;
            RemoveNoticeIcon();
            if (!id.empty() && mainEvents.onNotificationClicked) mainEvents.onNotificationClicked(id);
            break;
        }
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
            RemoveNoticeIcon();
            break;
        }
    }

    void RemoveNoticeIcon() {
        if (!noticeAdded_) return;
        if (HWND owner = MainHandle()) {
            NOTIFYICONDATAW data = NoticeData(owner);
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        noticeAdded_ = false;
        noticeId_.clear();
    }

    static NOTIFYICONDATAW NoticeData(HWND owner) {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = owner;
        data.uID = kNoticeIconId;
        return data;
    }

    // Copies text into a fixed UTF-16 field, cut to fit with its terminating zero.
    template <size_t N>
    static void CopyText(wchar_t (&field)[N], const std::string& text) {
        const std::wstring wide = Wide(text);
        const size_t count = std::min(wide.size(), N - 1);
        std::memcpy(field, wide.data(), count * sizeof(wchar_t));
        field[count] = L'\0';
    }

    // Opens the clipboard, trying again for a moment while another app has it open.
    static bool OpenClipboardForAWhile(HWND owner) {
        for (int i = 0; i < 10; ++i) {
            if (OpenClipboard(owner)) return true;
            Sleep(5);
        }
        return false;
    }

    std::unique_ptr<BackendWindow> OpenWindow(const WindowOptions& options, BackendEvents events) override {
        WebWindow::Setup setup;
        setup.title = options.title;
        setup.width = options.width;
        setup.height = options.height;
        setup.minWidth = options.minWidth;
        setup.minHeight = options.minHeight;
        setup.resizable = options.resizable;
        setup.keepAspectRatio = options.keepAspectRatio;
        setup.titleBar = options.titleBar;
        setup.showWhenReady = options.showWhenReady;
        setup.activate = options.activate;
        setup.hidden = options.startHidden;
        setup.owned = options.owned || options.modal;
        setup.modal = options.modal;
        setup.devTools = options_.devTools;
        setup.contextMenu = options_.contextMenu;
        setup.maxWidth = options.maxWidth;
        setup.maxHeight = options.maxHeight;
        setup.background = ColorValue(options.backgroundColor);
        auto window = std::make_unique<WebWindow>(*this, std::move(setup), std::move(events));
        Register(window.get());
        if (running_) {
            // Before run(): Run() creates it with the main window.
            if (!window->Create(window->GetSetup().owned ? MainHandle() : nullptr)) return nullptr;
            if (environment_) window->Start(environment_.Get());
        }
        return window;
    }

    int AddTimer(std::chrono::milliseconds interval, std::function<void()> fn) override {
        if (timers_.size() >= MAXIMUM_WAIT_OBJECTS - 1) return 0;
        // High-resolution waitable timer: SetTimer is quantized to the ~15.6 ms system tick.
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        if (!handle) handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!handle) return 0;
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -1;  // relative, fire almost immediately
        // A period of 0 would fire only once: at least 1 ms, at most what a LONG holds.
        const LONG period = LONG(std::clamp<long long>(interval.count(), 1, LONG_MAX));
        if (!SetWaitableTimer(handle, &dueTime, period, nullptr, nullptr, FALSE)) {
            CloseHandle(handle);
            return 0;
        }
        int id = nextTimer_++;
        timers_[id] = {handle, std::make_shared<std::function<void()>>(std::move(fn))};
        return id;
    }

    void RemoveTimer(int id) override {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        CancelWaitableTimer(it->second.handle);
        CloseHandle(it->second.handle);
        timers_.erase(it);
    }

    // The window a dialog belongs to: the active one if it is ours, else the main window.
    HWND DialogOwner() const {
        HWND active = GetActiveWindow();
        if (active == MainHandle()) return active;
        for (WebWindow* window : others_) {
            if (window->Handle() == active) return active;
        }
        return MainHandle();
    }

    // Gives a dialog its file types; the first is the one selected. "*" (every file) is the
    // shell's "*.*". Returns the first pattern ("" without types).
    static std::wstring SetFileTypes(IFileDialog* dialog, const std::vector<FileFilter>& filters) {
        if (filters.empty()) return L"";
        std::vector<std::wstring> texts;  // names and patterns, alive for the call
        texts.reserve(filters.size() * 2);
        for (const FileFilter& f : filters) {
            const std::string pattern = f.pattern == "*" ? "*.*" : f.pattern;
            texts.push_back(Wide(f.name.empty() ? pattern : f.name));
            texts.push_back(Wide(pattern));
        }
        std::vector<COMDLG_FILTERSPEC> types;
        for (size_t i = 0; i < texts.size(); i += 2) types.push_back({texts[i].c_str(), texts[i + 1].c_str()});
        dialog->SetFileTypes(UINT(types.size()), types.data());
        return Wide(filters.front().pattern);
    }

    // Makes a dialog open in directory, if that is a folder that is there.
    static void SetFolder(IFileDialog* dialog, const std::string& directory) {
        if (!IsFolder(directory)) return;
        std::wstring path = Wide(directory);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    std::optional<std::string> SaveFileDialog(const SaveDialogOptions& options) override {
        ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) {
            return std::nullopt;
        }
        const std::wstring pattern = SetFileTypes(dialog.Get(), DialogFilters(options));
        if (!pattern.empty()) {
            // The default extension comes from the first type's first pattern: "*.csv;*.txt" -> "csv".
            const std::wstring first = pattern.substr(0, pattern.find(L';'));
            const size_t dot = first.find_last_of(L'.');
            if (dot != std::wstring::npos && dot + 1 < first.size() && first.find(L'*', dot) == std::wstring::npos) {
                dialog->SetDefaultExtension(first.c_str() + dot + 1);
            }
        }
        SetFolder(dialog.Get(), options.directory);
        dialog->SetTitle(Wide(options.title).c_str());
        if (!options.defaultName.empty()) dialog->SetFileName(Wide(options.defaultName).c_str());
        if (FAILED(dialog->Show(DialogOwner()))) return std::nullopt;  // includes cancel
        ComPtr<IShellItem> item;
        LPWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            return std::nullopt;
        }
        std::string result = Utf8(path);
        CoTaskMemFree(path);
        return result;
    }

    std::vector<std::string> OpenFileDialog(const OpenDialogOptions& options) override {
        std::vector<std::string> paths;
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) {
            return paths;
        }
        FILEOPENDIALOGOPTIONS flags = 0;
        dialog->GetOptions(&flags);
        flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        flags |= options.folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
        if (options.multiple) flags |= FOS_ALLOWMULTISELECT;
        dialog->SetOptions(flags);
        if (!options.folder) {
            std::vector<FileFilter> filters = DialogFilters(options);
            // With filterName/filterPattern alone, "All files" follows it, as it always has; a list
            // in filters is shown as given.
            if (!filters.empty() && options.filters.empty()) filters.push_back({"All files", "*.*"});
            SetFileTypes(dialog.Get(), filters);
        }
        SetFolder(dialog.Get(), options.directory);
        dialog->SetTitle(Wide(options.title).c_str());
        if (FAILED(dialog->Show(DialogOwner()))) return paths;  // includes cancel
        ComPtr<IShellItemArray> items;
        DWORD count = 0;
        if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count))) return paths;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            LPWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                paths.push_back(Utf8(path));
                CoTaskMemFree(path);
            }
        }
        return paths;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<WebWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) return self->HandleMessage(window, message, wParam, lParam);
        return DefWindowProcW(window, message, wParam, lParam);
    }

private:
    struct Timer {
        HANDLE handle;
        std::shared_ptr<std::function<void()>> fn;
    };

    // WebView2's profile folder (cache, storage): AppOptions::dataDirectory, or by default
    // %LOCALAPPDATA%\<exe name>, which is writable wherever the app is installed. WebView2
    // creates it if needed.
    std::wstring DataDirectory() const {
        if (!options_.dataDirectory.empty()) return Wide(options_.dataDirectory);
        std::wstring directory;
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
            std::wstring name = ExecutablePathW();
            name = name.substr(name.find_last_of(L"\\/") + 1);
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) name.erase(dot);
            directory = std::wstring(base) + L"\\" + name;
        }
        CoTaskMemFree(base);
        return directory;  // empty: WebView2's own default, "<exe name>.WebView2" next to the exe
    }

    // One WebView2 environment (one browser process) for every window; each window's web view
    // starts once it exists.
    void CreateEnvironment() {
        const std::wstring dataDirectory = DataDirectory();
        // WebView2 starts asynchronously. If the app ends first, the backend may be gone when
        // these callbacks run (in a later message loop, e.g. a dialog's): they check `alive`.
        std::shared_ptr<bool> alive = alive_;
        // AppOptions::browserArguments: extra Chromium switches. (WebView2 ignores the
        // WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS environment variable on current runtimes;
        // the options object always works.)
        const std::string arguments = BrowserArguments(options_);
        ComPtr<CoreWebView2EnvironmentOptions> environmentOptions;
        if (!arguments.empty()) {
            environmentOptions = Make<CoreWebView2EnvironmentOptions>();
            environmentOptions->put_AdditionalBrowserArguments(Wide(arguments).c_str());
        }
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataDirectory.empty() ? nullptr : dataDirectory.c_str(), environmentOptions.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, alive](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (!*alive || !MainHandle()) return S_OK;  // closed while starting
                    if (FAILED(result)) {
                        main_->Fail(L"Creating the WebView2 environment failed. Is the WebView2 Runtime installed?", result);
                        return result;
                    }
                    environment_ = environment;
                    LPWSTR browserVersion = nullptr;
                    if (SUCCEEDED(environment->get_BrowserVersionString(&browserVersion)) && browserVersion) {
                        engineVersion_ = RuntimeVersion(Utf8(browserVersion));
                        CoTaskMemFree(browserVersion);
                    }
                    environment_.As(&environment12_);  // fails on runtimes without shared buffers
                    main_->Start(environment);
                    for (WebWindow* window : std::vector<WebWindow*>(others_)) {
                        if (window->Handle() && !window->Started()) window->Start(environment);
                    }
                    return S_OK;
                }).Get());
        if (FAILED(hr)) main_->Fail(L"Starting WebView2 failed. Is the WebView2 Runtime installed?", hr);
    }

    AppOptions options_;
    std::wstring origin_;  // "https://<host>"
    DWORD uiThread_;
    bool comInitialized_ = false;
    bool running_ = false;
    std::atomic<HWND> mainHandle_{nullptr};  // where posted functions are delivered
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed
    std::unique_ptr<WebWindow> main_;
    std::vector<WebWindow*> others_;  // further windows (owned by the core)
    ProgramIcons programIcons_;       // the program's icon (its first icon resource), or none
    bool noticeAdded_ = false;        // the notification icon is in the notification area
    std::string noticeId_;            // the core's id of the balloon shown
    ComPtr<ICoreWebView2Environment> environment_;
    std::string engineVersion_;  // the WebView2 runtime's, once the environment exists
    ComPtr<ICoreWebView2Environment12> environment12_;
    std::mutex postMutex_;
    std::vector<std::function<void()>> posted_;
    std::map<int, Timer> timers_;
    int nextTimer_ = 1;
};

// --- WebWindow -----------------------------------------------------------------------------------

WebWindow::~WebWindow() {
    *alive_ = false;  // WebView2 callbacks still pending must not touch this object
    if (!setup_.main) backend_.Unregister(this);
    if (window_) {
        silent_ = true;  // the core destroys it (the App ending): no events
        if (setup_.modal && owner_) EnableWindow(owner_, TRUE);
        DestroyWindow(window_);
    }
    if (controller_) controller_->Close();
    webview17_.Reset();
    webview_.Reset();
    controller_.Reset();
    environment_.Reset();
    if (bigIcon_) DestroyIcon(bigIcon_);
    if (smallIcon_) DestroyIcon(smallIcon_);
}

bool WebWindow::Create(HWND owner) {
    // width/height are the page's area; the window adds its title bar and borders.
    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    RECT frame = {0, 0, MulDiv(setup_.width, dpi, 96), MulDiv(setup_.height, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (place_.hasPosition) {  // setPosition before it existed, or a remembered placement
        x = MulDiv(place_.x, dpi, 96);
        y = MulDiv(place_.y, dpi, 96);
    } else if (owner) {
        // An owned window opens centred over its owner, kept on the owner's screen.
        RECT over;
        GetWindowRect(owner, &over);
        MONITORINFO monitor = {sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& work = monitor.rcWork;
        x = std::clamp(int((over.left + over.right - width) / 2), int(work.left), std::max(int(work.left), int(work.right - width)));
        y = std::clamp(int((over.top + over.bottom - height) / 2), int(work.top), std::max(int(work.top), int(work.bottom - height)));
    }
    HWND window = CreateWindowExW(0, kWindowClass, Wide(setup_.title).c_str(), WindowStyle(), x, y, width, height,
                                  owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!window) return false;
    window_ = window;
    owner_ = owner;
    if (setup_.main) backend_.SetMainHandle(window);
    ApplyIcon();
    ApplyTitleBar();  // before it shows: no light title bar flashing first
    // Shown but cloaked: invisible on screen (no empty or white window flashing) while the
    // web view starts and the page loads and paints; Reveal() uncloaks it then, or after
    // kRevealTimeout if no page loads. Unlike a hidden window, the page runs as if shown.
    const BOOL cloak = TRUE;
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
    // Started hidden (or hidden before it was made): made, its web view and page too, but shown
    // only by SetVisible.
    everShown_ = !setup_.hidden;
    if (!setup_.hidden) {
        if (setup_.modal && owner) EnableWindow(owner, FALSE);  // a dialog: its owner waits for it
        int show = setup_.activate ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE;
        if (place_.pending) {  // minimize(), maximize() before it existed
            place_.pending = false;
            if (place_.state == WindowState::Maximized) show = SW_SHOWMAXIMIZED;
            if (place_.state == WindowState::Minimized) show = SW_SHOWMINNOACTIVE;
        }
        ShowWindow(window, show);
    }
    if (place_.center) Center();
    if (place_.onTop) SetAlwaysOnTop(true);
    TrackNormal();
    SetTimer(window, kRevealTimer, setup_.showWhenReady ? kRevealTimeoutReady : kRevealTimeout, nullptr);
    if (setup_.fullscreen) {
        setup_.fullscreen = false;
        SetFullscreen(true);
    }
    return true;
}

void WebWindow::ApplyIcon() {
    HWND window = window_;
    if (!window) return;
    // A further window without an icon of its own shows the main window's.
    WebWindow& main = backend_.Main();
    HICON big = bigIcon_ ? bigIcon_ : (&main != this ? main.BigIcon() : nullptr);
    HICON little = smallIcon_ ? smallIcon_ : (&main != this ? main.SmallIcon() : nullptr);
    if (big) SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (little) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
}

void WebWindow::Navigate(const std::string& path) {
    if (webview_) webview_->Navigate((backend_.Origin() + Wide(path)).c_str());
}

void WebWindow::Start(ICoreWebView2Environment* environment) {
    if (started_ || !window_ || !environment) return;
    started_ = true;
    environment_ = environment;
    std::shared_ptr<bool> alive = alive_;
    const HRESULT hr = environment->CreateCoreWebView2Controller(
        window_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, alive](HRESULT created, ICoreWebView2Controller* controller) {
                if (!*alive) {
                    if (controller) controller->Close();
                    return S_OK;
                }
                return OnControllerCreated(created, controller);
            }).Get());
    if (FAILED(hr)) Fail(L"Creating the WebView2 controller failed.", hr);
}

HRESULT WebWindow::OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller) {
    if (!window_) {  // the window closed while WebView2 was starting: nothing to show
        if (controller) controller->Close();
        return S_OK;
    }
    if (FAILED(result)) {
        Fail(L"Creating the WebView2 controller failed.", result);
        return result;
    }
    controller_ = controller;
    const HRESULT got = controller_->get_CoreWebView2(&webview_);
    if (FAILED(got) || !webview_) {
        Fail(L"Getting the WebView2 web view failed.", FAILED(got) ? got : E_POINTER);
        return got;
    }
    webview_.As(&webview17_);
    // AppOptions::backgroundColor: what the web view shows through a page with no background,
    // and before a page is there.
    if (const int rgb = setup_.background; rgb >= 0) {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller_.As(&controller2))) {
            COREWEBVIEW2_COLOR color;
            color.A = 255;
            color.R = BYTE((rgb >> 16) & 0xff);
            color.G = BYTE((rgb >> 8) & 0xff);
            color.B = BYTE(rgb & 0xff);
            controller2->put_DefaultBackgroundColor(color);
        }
    }
    // AppOptions::devTools: off (the default), no F12 and no "Inspect" in the page's menu.
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) settings->put_AreDevToolsEnabled(setup_.devTools);
    // AppOptions::contextMenu off: the right-click menu keeps its text editing items only.
    if (!setup_.contextMenu) {
        ComPtr<ICoreWebView2_11> webview11;
        if (SUCCEEDED(webview_.As(&webview11))) {
            EventRegistrationToken menu;
            webview11->add_ContextMenuRequested(
                Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                        KeepEditingItems(args);
                        return S_OK;
                    }).Get(),
                &menu);
        }
    }
    RECT bounds;
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
    // The window stays cloaked (Create) until its first page has loaded and painted.
    EventRegistrationToken loaded;
    webview_->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                // A moment for the page's first frame to reach the screen, then show it;
                // unless the page shows it itself (showWhenReady: app.show()).
                if (!revealed_ && window_ && !setup_.showWhenReady) SetTimer(window_, kRevealTimer, kRevealAfterLoad, nullptr);
                return S_OK;
            }).Get(),
        &loaded);
    // The window got focus before the WebView existed; hand it over now.
    if (GetFocus() == window_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

    const std::wstring& origin = backend_.Origin();
    EventRegistrationToken token;
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, origin](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR source = nullptr;
                args->get_Source(&source);
                const std::wstring prefix = origin + L"/";
                const bool trusted = source && wcsncmp(source, prefix.c_str(), prefix.size()) == 0;
                CoTaskMemFree(source);
                LPWSTR text = nullptr;
                if (trusted && SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text) {
                    std::wstring wide(text);
                    CoTaskMemFree(text);
                    if (wide.rfind(kDropMessage, 0) == 0) {  // kDropScript's: files dropped on the page
                        OnDropMessage(args, wide.substr(wcslen(kDropMessage)));
                        return S_OK;
                    }
                    if (events_.onMessage) events_.onMessage(Utf8(wide));
                }
                return S_OK;
            }).Get(),
        &token);

    // Where the page may go: the app's pages; not to a file (kDropScript takes dropped files, but
    // a link to one must not replace the page either); anywhere else, outside the app (the core
    // decides). NavigationStarting is the top frame's only: iframes load what they like.
    const std::string originUtf8 = Utf8(origin);
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this, originUtf8](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                std::string target;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) target = Utf8(uri);
                CoTaskMemFree(uri);
                const Destination where = DestinationOf(originUtf8, target);
                if (where != Destination::App) {
                    args->put_Cancel(TRUE);
                    if (where == Destination::Outside && events_.onExternalNavigation) events_.onExternalNavigation(target);
                    return S_OK;
                }
                if (events_.onNavigationStarting) events_.onNavigationStarting();
                return S_OK;
            }).Get(),
        &token);

    // window.open() and target=_blank links: no window of the page's own (WebView2's default would
    // be a bare browser window outside the app); one to another site opens outside.
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this, originUtf8](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                std::string target;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) target = Utf8(uri);
                CoTaskMemFree(uri);
                args->put_Handled(TRUE);
                if (DestinationOf(originUtf8, target) == Destination::Outside && events_.onExternalNavigation) {
                    events_.onExternalNavigation(target);
                }
                return S_OK;
            }).Get(),
        &token);
    webview_->AddScriptToExecuteOnDocumentCreated(kDropScript, nullptr);

    webview_->AddWebResourceRequestedFilter((origin + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview_->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, origin](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                // Unanswered, the request goes to the network and fails (the host is made up).
                if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                LPWSTR uri = nullptr;
                request->get_Uri(&uri);
                std::wstring url(uri ? uri : L"");
                CoTaskMemFree(uri);
                Response response;
                if (url.size() > origin.size() && events_.onRequest) {
                    Request info;
                    info.path = Utf8(url.substr(origin.size()));
                    LPWSTR method = nullptr;
                    if (SUCCEEDED(request->get_Method(&method)) && method) info.method = Utf8(method);
                    CoTaskMemFree(method);
                    if (info.method == "POST") {
                        ComPtr<IStream> content;
                        if (SUCCEEDED(request->get_Content(&content)) && content) ReadAll(content.Get(), info.body);
                    }
                    events_.onRequest(info, response);
                }
                ComPtr<ICoreWebView2WebResourceResponse> reply;
                if (response.status == 200) {
                    ComPtr<IStream> stream = Make<MemoryStream>(response.data, response.size,
                                                                std::move(response.keepAlive));
                    std::wstring headers = L"Content-Type: " + Wide(response.mimeType) +
                                           L"\r\nCache-Control: no-store";
                    environment_->CreateWebResourceResponse(stream.Get(), 200, L"OK",
                                                            headers.c_str(), &reply);
                } else {
                    environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found",
                                                            L"Content-Type: text/plain", &reply);
                }
                return args->put_Response(reply.Get());
            }).Get(),
        &token);

    ComPtr<ICoreWebView2Environment12> environment12;
    const bool shared = SUCCEEDED(environment_.As(&environment12)) && webview17_;
    if (events_.onReady) events_.onReady(shared);
    return S_OK;
}

LRESULT WebWindow::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_GETMINMAXINFO:
        if (setup_.minWidth > 0 || setup_.minHeight > 0) {
            // The minimum is for the page's area, like width/height; add the frame.
            const UINT dpi = GetDpiForWindow(window);
            RECT frame = {0, 0, MulDiv(std::max(setup_.minWidth, 0), dpi, 96),
                          MulDiv(std::max(setup_.minHeight, 0), dpi, 96)};
            AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, dpi);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            if (setup_.minWidth > 0) limits->ptMinTrackSize.x = frame.right - frame.left;
            if (setup_.minHeight > 0) limits->ptMinTrackSize.y = frame.bottom - frame.top;
        }
        if (setup_.maxWidth > 0 || setup_.maxHeight > 0) {
            // The maximum likewise: the page's area, plus the frame. It limits dragging (the
            // track size) and what maximising gives (the maximised size).
            const UINT dpi = GetDpiForWindow(window);
            RECT frame = {0, 0, MulDiv(std::max(setup_.maxWidth, 0), dpi, 96),
                          MulDiv(std::max(setup_.maxHeight, 0), dpi, 96)};
            AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, dpi);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            if (setup_.maxWidth > 0) {
                limits->ptMaxTrackSize.x = frame.right - frame.left;
                limits->ptMaxSize.x = std::min(limits->ptMaxSize.x, limits->ptMaxTrackSize.x);
            }
            if (setup_.maxHeight > 0) {
                limits->ptMaxTrackSize.y = frame.bottom - frame.top;
                limits->ptMaxSize.y = std::min(limits->ptMaxSize.y, limits->ptMaxTrackSize.y);
            }
        }
        return 0;
    case WM_SIZING:
        // keepAspectRatio: adjust the rectangle being dragged so the page area (the client
        // area, without the frame) keeps setup_.width : setup_.height.
        if (setup_.keepAspectRatio && setup_.width > 0 && setup_.height > 0) {
            auto* rect = reinterpret_cast<RECT*>(lParam);
            RECT frame = {0, 0, 0, 0};
            AdjustWindowRectExForDpi(&frame, WindowStyle(), FALSE, 0, GetDpiForWindow(window));
            const int frameWidth = frame.right - frame.left;
            const int frameHeight = frame.bottom - frame.top;
            const double ratio = double(setup_.width) / setup_.height;
            const int width = rect->right - rect->left - frameWidth;
            const int height = rect->bottom - rect->top - frameHeight;
            switch (wParam) {
            case WMSZ_TOP:
            case WMSZ_BOTTOM:  // the height is dragged: the width follows
                rect->right = rect->left + int(height * ratio + 0.5) + frameWidth;
                break;
            case WMSZ_TOPLEFT:
            case WMSZ_TOPRIGHT:  // the top moves: keep the bottom edge
                rect->top = rect->bottom - int(width / ratio + 0.5) - frameHeight;
                break;
            default:  // left, right and the bottom corners: the height follows the width
                rect->bottom = rect->top + int(width / ratio + 0.5) + frameHeight;
                break;
            }
            return TRUE;
        }
        break;
    case WM_SETTINGCHANGE:
        // The system's light or dark setting changed: follow it.
        if (lParam && setup_.titleBar == TitleBar::System
            && lstrcmpW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            ApplyTitleBar();
        }
        break;
    case WM_TIMER:
        if (wParam == kRevealTimer) {
            Reveal();
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        // What shows where the web view doesn't (its edges while resizing): the background
        // colour the app gave, else dark or light, as the title bar.
        RECT area;
        GetClientRect(window, &area);
        const int rgb = setup_.background;
        const COLORREF color = rgb >= 0 ? RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff)
                               : dark_  ? kDarkBackground
                                        : GetSysColor(COLOR_WINDOW);
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(reinterpret_cast<HDC>(wParam), &area, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_DPICHANGED: {
        // Moved to a screen with another scale: take the size Windows suggests for it.
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (controller_) {
            RECT bounds;
            GetClientRect(window, &bounds);
            controller_->put_Bounds(bounds);
        }
        TrackNormal();
        if (events_.onPlacementChanged) events_.onPlacementChanged();
        return 0;
    case WM_MOVE:
        TrackNormal();
        if (events_.onPlacementChanged) events_.onPlacementChanged();
        return 0;
    case WM_SETFOCUS:
        // Pass keyboard focus on to the page, so keyboard shortcuts work without a click.
        if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_APP_POSTED:
        backend_.RunPosted();
        return 0;
    case WM_APP_NOTICE:
        if (setup_.main) backend_.NoticeEvent(lParam, events_);
        return 0;
    case WM_CLOSE:
        // The close button, Alt+F4 and the taskbar's Close ask; Close (closeAsked_) doesn't.
        if (!closeAsked_ && events_.onCloseRequested && !events_.onCloseRequested(CloseReason::Window)) {
            return 0;  // kept open
        }
        // A dialog gives its owner input back before it goes, so Windows activates the owner
        // (not some other app's window) next.
        if (setup_.modal && owner_) EnableWindow(owner_, TRUE);
        break;  // DefWindowProc destroys it
    case WM_DESTROY: {
        if (setup_.modal && owner_) EnableWindow(owner_, TRUE);
        const bool silent = silent_;
        if (!silent && events_.onClosed) events_.onClosed();
        if (controller_) controller_->Close();
        controller_.Reset();
        webview17_.Reset();
        webview_.Reset();
        if (setup_.main) backend_.RemoveNoticeIcon();  // it belongs to this window: before its handle goes
        window_ = nullptr;
        if (setup_.main) {
            backend_.SetMainHandle(nullptr);
            backend_.MainDestroyed();
        }
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

// ShellExecute's "open", as a double-click: on a thread of its own with COM set up, as the shell
// asks of its callers (and the caller's thread may be any).
void SystemOpen(const std::string& target) {
    const std::wstring wide = Wide(target);
    INT_PTR result = 0;
    std::thread([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        result = INT_PTR(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (SUCCEEDED(com)) CoUninitialize();
    }).join();
    if (result <= 32) throw std::runtime_error("ShellExecute error " + std::to_string(result));
}

// Explorer with the path selected (explorer /select,"path"; Explorer ends with 1 either way, so
// only starting it counts).
void SystemReveal(const std::string& path) {
    if (path.find('"') != std::string::npos) throw std::runtime_error("a path with a quote in it");
    std::wstring line = L"explorer.exe /select,\"" + Wide(path) + L"\"";
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("explorer didn't start (error " + std::to_string(GetLastError()) + ")");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

std::unique_ptr<Backend> CreateBackend(const AppOptions& options, BackendEvents events) {
    return std::make_unique<WindowsBackend>(options, std::move(events));
}

}  // namespace detail
}  // namespace uniapi
