// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// macOS backend: a Cocoa window hosting WKWebView. Objective-C++ with ARC; every other file of
// the library is C++. Needs macOS 14 (WKWebsiteDataStore by identifier, for dataDirectory).
// The main window and further windows (App::openWindow) are both MacWindow; MacBackend is the
// app around them (event loop, timers, posting, dialogs).
//
//   page -> app messages    window.__uniapiPost(text), bridged to a script message handler in
//                           its own content world (top frame only; see Bridge scripts below)
//   app -> page messages    evaluateJavaScript("window.__uniapi.receive(...)") in the page's world
//   page files and fetches  a WKURLSchemeHandler for uniapi://<host>/
//   shared memory           none: WKWebView has no public way to give a page outside memory
//                           (no web process extensions, as WebKitGTK has), so streams always use
//                           the fetch fallback. CreateSharedRegion returns nullptr.
//
// Request bodies: WKWebView drops a Blob POST body on the way to a scheme handler (HTTPBody and
// HTTPBodyStream both nil; measured on macOS 26.5, 2026-11-10). uniapi.js sends ArrayBuffers,
// which arrive whole in HTTPBody. A POST arriving without a body is logged (UNIAPI_DEBUG).
//
// Set UNIAPI_DEBUG=1 to see the backend's debug messages on stderr.

#include "core/backend.hpp"

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>

#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

extern char** environ;

#if !__has_feature(objc_arc)
#error "backend_macos.mm needs ARC (-fobjc-arc)"
#endif

namespace uniapi {

std::string executablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    char resolved[PATH_MAX];
    return realpath(buffer.data(), resolved) ? std::string(resolved) : std::string(buffer.data());
}

std::vector<std::string> arguments() {
    std::vector<std::string> result;
    NSArray<NSString*>* all = [NSProcessInfo processInfo].arguments;  // UTF-8 already; [0] is the program
    for (NSUInteger i = 1; i < all.count; ++i) result.emplace_back(all[i].UTF8String ?: "");
    return result;
}

namespace detail {
namespace {

void Debug(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Debug(const char* format, ...) {
    static const bool on = [] {
        const char* value = getenv("UNIAPI_DEBUG");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (!on) return;
    va_list args;
    va_start(args, format);
    std::fputs("uniapi: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

NSString* Ns(const std::string& text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
}

std::string Utf8(NSString* text) { return text ? std::string(text.UTF8String ?: "") : std::string(); }

// A JavaScript string literal holding `text`. JSON strings are valid JavaScript; invalid UTF-8
// is replaced rather than thrown on.
std::string JsString(const std::string& text) {
    return json(text).dump(-1, ' ', false, json::error_handler_t::replace);
}

constexpr char kScheme[] = "uniapi";

// The page is hidden (window alpha 0) and appears once it has loaded and drawn a frame
// (kRevealAfterDrawMs after that frame, for it to reach the screen), or after the timeout;
// longer when the page shows it itself (showWhenReady). As on Linux.
constexpr int kRevealAfterDrawMs = 20;
constexpr int kRevealTimeoutMs = 3000;
constexpr int kRevealTimeoutReadyMs = 5000;

class MacBackend;
class MacWindow;
MacBackend* gRunning = nullptr;  // the backend whose Run() is in the event loop (one at a time)
MacBackend* gNotifying = nullptr;  // the backend notification clicks go to (set by Run())

}  // namespace
}  // namespace detail
}  // namespace uniapi

using uniapi::detail::MacWindow;

// --- Objective-C classes: each forwards to its C++ owner, whose pointer is cleared before the
// owner goes (so a late callback finds nil and does nothing). -----------------------------------

// A window that can be held by a modal window (WindowOptions::modal): no input, never key, a
// click brings the modal window forward instead.
@interface UniapiNSWindow : NSWindow
@property(nonatomic, weak) NSWindow* heldBy;
@end

@implementation UniapiNSWindow
- (BOOL)canBecomeKeyWindow {
    return self.heldBy == nil && [super canBecomeKeyWindow];
}
- (BOOL)canBecomeMainWindow {
    return self.heldBy == nil && [super canBecomeMainWindow];
}
- (void)sendEvent:(NSEvent*)event {
    if (NSWindow* holder = self.heldBy) {
        switch (event.type) {
            case NSEventTypeLeftMouseDown:
            case NSEventTypeRightMouseDown:
            case NSEventTypeOtherMouseDown:
                [holder makeKeyAndOrderFront:nil];
                NSBeep();
                return;
            case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseUp:
            case NSEventTypeOtherMouseUp:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged:
            case NSEventTypeScrollWheel:
            case NSEventTypeKeyDown:
            case NSEventTypeKeyUp:
            case NSEventTypeFlagsChanged:
            case NSEventTypeMagnify:
            case NSEventTypeRotate:
            case NSEventTypeSwipe:
                return;
            default:
                break;
        }
    }
    [super sendEvent:event];
}
@end

@interface UniapiWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) MacWindow* host;
@end

@interface UniapiViewDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(nonatomic, assign) MacWindow* host;
@end

@interface UniapiSchemeHandler : NSObject <WKURLSchemeHandler>
@property(nonatomic, assign) MacWindow* host;
@end

// The web view: as WKWebView, and files dropped on it are reported (their paths, which the page's
// File objects don't have) before WebKit hands the page its drop event.
@interface UniapiWebView : WKWebView
@property(nonatomic, assign) MacWindow* host;
// AppOptions::contextMenu off: its right-click menu keeps the text editing items only.
@property(nonatomic, assign) BOOL editingMenuOnly;
// For tests (tests/desktop): as if these files had been dropped at (x, y) in CSS pixels.
- (void)uniapiTestDropPaths:(NSArray<NSString*>*)paths x:(NSInteger)x y:(NSInteger)y;
@end

// Cmd+Q, the app menu's Quit and the Dock's ask the main window's onCloseRequested
// (CloseReason::Quit); if it lets them, they close the main window (as App::quit), so run()
// returns to main() instead of the process exiting in -terminate: (a logout still ends it there,
// without asking). Clicking the Dock icon with no window on screen shows a hidden main window.
@interface UniapiAppDelegate : NSObject <NSApplicationDelegate>
@end

// Follows the system's light/dark appearance for web views whose window has a forced one.
@interface UniapiAppearanceObserver : NSObject
@end

// Shows notifications while the app is in front too (otherwise only in the list), and reports
// clicks on them (to the running backend: gNotifying).
@interface UniapiNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

namespace uniapi {
namespace detail {
namespace {

UniapiAppDelegate* gAppDelegate = nil;

NSMenuItem* AddItem(NSMenu* menu, NSString* title, SEL action, NSString* key,
                    NSEventModifierFlags modifiers = NSEventModifierFlagCommand) {
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    return item;
}

// The menu bar every Mac app needs: without an Edit menu, Cmd+C, V, X, A and Z do nothing in the
// page's text fields (WKWebView gets them through the menu's key equivalents); without the app
// menu there is no Cmd+Q. AppKit adds "Enter Full Screen" to the View menu itself.
void BuildMenu() {
    NSString* name = [NSBundle mainBundle].infoDictionary[@"CFBundleName"] ?: [NSProcessInfo processInfo].processName;
    NSMenu* bar = [NSMenu new];

    NSMenu* app = [[NSMenu alloc] initWithTitle:name];
    AddItem(app, [@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    NSMenu* services = [[NSMenu alloc] initWithTitle:@"Services"];
    [app addItemWithTitle:@"Services" action:nil keyEquivalent:@""].submenu = services;
    NSApp.servicesMenu = services;
    [app addItem:[NSMenuItem separatorItem]];
    AddItem(app, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h");
    AddItem(app, @"Hide Others", @selector(hideOtherApplications:), @"h",
            NSEventModifierFlagCommand | NSEventModifierFlagOption);
    AddItem(app, @"Show All", @selector(unhideAllApplications:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    AddItem(app, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q");
    [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = app;

    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    AddItem(edit, @"Undo", @selector(undo:), @"z");
    AddItem(edit, @"Redo", @selector(redo:), @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [edit addItem:[NSMenuItem separatorItem]];
    AddItem(edit, @"Cut", @selector(cut:), @"x");
    AddItem(edit, @"Copy", @selector(copy:), @"c");
    AddItem(edit, @"Paste", @selector(paste:), @"v");
    AddItem(edit, @"Select All", @selector(selectAll:), @"a");
    [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""].submenu = edit;

    NSMenu* view = [[NSMenu alloc] initWithTitle:@"View"];
    [bar addItemWithTitle:@"View" action:nil keyEquivalent:@""].submenu = view;

    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    AddItem(window, @"Minimize", @selector(performMiniaturize:), @"m");
    AddItem(window, @"Zoom", @selector(performZoom:), @"", 0);
    [window addItem:[NSMenuItem separatorItem]];
    AddItem(window, @"Close", @selector(performClose:), @"w");
    [window addItem:[NSMenuItem separatorItem]];
    AddItem(window, @"Bring All to Front", @selector(arrangeInFront:), @"", 0);
    [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""].submenu = window;
    NSApp.windowsMenu = window;

    NSApp.mainMenu = bar;
}

// NSApplication, once per process: by run(), or by a dialog, alert() or setIcon() before it. A
// plain executable (not in an .app bundle, e.g. started from the build folder) is a background
// program until told otherwise: no Dock icon, no menu bar, windows behind the terminal.
void InitApp() {
    static bool started = false;
    if (started) return;
    started = true;
    // The program's arguments stay its own: AppKit would otherwise take any that isn't an option
    // for a file to open, and report it again through application:openURLs: (App::onOpenFiles)
    // as well as in argv.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"NSTreatUnknownArgumentsAsOpen" : @"NO"}];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    gAppDelegate = [UniapiAppDelegate new];
    NSApp.delegate = gAppDelegate;
    BuildMenu();
}

// Brings the app to the front. macOS 14's cooperative [NSApp activate] only works when the
// active app yields, and a terminal never does: a program started from one opened behind it.
// So the older call, which still takes focus (deprecated in 14, not removed).
void ActivateApp() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

// A dialog shown before or outside a window should come up in front, also from a terminal.
void ActivateForDialog() {
    InitApp();
    ActivateApp();
}

// Profiles: AppOptions::dataDirectory names one. WebKit keeps every profile in its own folder
// and finds it by a UUID (macOS 14), so the path is turned into one: the same path, the same
// profile. Empty: the default profile (~/Library/WebKit/<bundle id or executable name>).
WKWebsiteDataStore* DataStoreFor(const std::string& directory) {
    if (directory.empty()) return [WKWebsiteDataStore defaultDataStore];
    // A name-based UUID (version 5 layout) from SHA-1 of the path.
    const std::string name = "uniapi.dataDirectory:" + directory;
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(name.data(), CC_LONG(name.size()), digest);
    digest[6] = (digest[6] & 0x0F) | 0x50;
    digest[8] = (digest[8] & 0x3F) | 0x80;
    NSUUID* uuid = [[NSUUID alloc] initWithUUIDBytes:digest];
    Debug("dataDirectory %s is WebKit profile %s", directory.c_str(), uuid.UUIDString.UTF8String);
    return [WKWebsiteDataStore dataStoreForIdentifier:uuid];
}

// Content types for "*.png;*.jpg"; empty (anything) for an empty pattern or one with * or *.*.
NSArray<UTType*>* ContentTypes(const std::string& patterns) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    size_t start = 0;
    while (start <= patterns.size()) {
        const size_t end = std::min(patterns.find(';', start), patterns.size());
        std::string pattern = patterns.substr(start, end - start);
        start = end + 1;
        pattern.erase(0, pattern.find_first_not_of(" \t"));
        pattern.erase(pattern.find_last_not_of(" \t") + 1);
        if (pattern.empty()) continue;
        if (pattern == "*" || pattern == "*.*") return @[];
        if (pattern.rfind("*.", 0) == 0) pattern = pattern.substr(2);
        if (UTType* type = [UTType typeWithFilenameExtension:Ns(pattern)]) [types addObject:type];
    }
    return types;
}

// Every file type's patterns in one: macOS panels have no list of types to choose from and no
// filter names, so a panel allows what any of them does.
template <typename Options>
std::string AllPatterns(const Options& options) {
    std::string patterns;
    for (const FileFilter& f : DialogFilters(options)) {
        if (!patterns.empty()) patterns += ';';
        patterns += f.pattern;
    }
    return patterns;
}

// Makes a panel open in directory, if that is a folder that is there.
void SetDirectory(NSSavePanel* panel, const std::string& directory) {
    if (!IsFolder(directory)) return;
    panel.directoryURL = [NSURL fileURLWithPath:Ns(directory) isDirectory:YES];
}

// The bytes of a POST: HTTPBody, else HTTPBodyStream (not seen in practice; see the top).
void ReadBody(NSURLRequest* request, std::vector<uint8_t>& body) {
    if (NSData* data = request.HTTPBody) {
        const auto* bytes = static_cast<const uint8_t*>(data.bytes);
        body.assign(bytes, bytes + data.length);
        return;
    }
    if (NSInputStream* stream = request.HTTPBodyStream) {
        [stream open];
        uint8_t chunk[1 << 16];
        NSInteger read;
        while ((read = [stream read:chunk maxLength:sizeof chunk]) > 0) body.insert(body.end(), chunk, chunk + read);
        [stream close];
        return;
    }
    Debug("a POST to %s arrived without its body (a Blob body? WKWebView drops those)",
          request.URL.absoluteString.UTF8String);
}

// The JavaScript of the bridge (as on Linux, see AddBridge in backend_linux.cpp): the top frame
// gets window.__uniapiPost in the page's world, which dispatches a DOM event on its own document;
// a script in uniapi's content world, also in the top frame only, forwards it to the handler,
// which only that world can see. An iframe from another origin can't touch the top document,
// so it has no way in. uniapi.js uses __uniapiPost.
NSString* const kBridgePage =
    @"window.__uniapiPost = (text) => "
     "document.dispatchEvent(new CustomEvent('uniapi:post', { detail: String(text) }));";
NSString* const kBridgeForward =
    @"document.addEventListener('uniapi:post', (event) => {"
     "  if (typeof event.detail === 'string') window.webkit.messageHandlers.uniapi.postMessage(event.detail);"
     "});";
// Resolves once the page's load event has come and the frame after it has been drawn (two
// animation frames), as on Linux (AfterPageDrawn there).
NSString* const kAfterDrawn =
    @"return new Promise(r => {"
     "  const frames = () => requestAnimationFrame(() => requestAnimationFrame(() => r(true)));"
     "  if (document.readyState === 'complete') frames(); else addEventListener('load', frames, { once: true });"
     "});";

WKContentWorld* UniapiWorld() { return [WKContentWorld worldWithName:@"uniapi"]; }

// uniapi's coordinates are top-left: from the primary screen's top-left corner, y down. Cocoa's
// are from its bottom-left corner, y up.
CGFloat PrimaryHeight() {
    NSScreen* primary = NSScreen.screens.firstObject;
    return primary ? NSHeight(primary.frame) : 0;
}
NSPoint TopLeft(NSRect frame) { return NSMakePoint(NSMinX(frame), PrimaryHeight() - NSMaxY(frame)); }
NSPoint CocoaPoint(NSPoint topLeft) { return NSMakePoint(topLeft.x, PrimaryHeight() - topLeft.y); }

// --- A window and its web view: the main window and every further one ---------------------------

class MacWindow final : public BackendWindow {
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
        bool hidden = false; // hide(), startHidden: ordered out until SetVisible
        bool main = false;   // the app's main window: closing it ends the app
        bool owned = false;  // opens centred over the main window
        bool modal = false;  // the main window takes no input while it is open
        bool devTools = false;  // AppOptions::devTools: Safari's Develop menu lists the page
        bool contextMenu = true;  // AppOptions::contextMenu: the right-click menu in full
        int maxWidth = 0;       // 0: no maximum
        int maxHeight = 0;
        int background = -1;    // 0xRRGGBB behind the page, or -1 (ColorValue)
    };

    MacWindow(MacBackend& backend, Setup setup, BackendEvents events)
        : backend_(backend), setup_(std::move(setup)), events_(std::move(events)) {}
    ~MacWindow() override;

    // Makes the window and its view: at once, or (opened before run()) with the main window.
    void Create(const std::string& origin, WKWebsiteDataStore* store, NSWindow* owner);
    // The main window closed: this one closes too, with its events.
    void Destroy() {
        if (window_) [window_ close];  // windowWillClose: OnClosing
    }
    bool Created() const { return created_; }
    const Setup& GetSetup() const { return setup_; }
    NSWindow* Handle() const { return window_; }
    WKWebView* View() const { return view_; }

    void ReportReady() {
        if (events_.onReady) events_.onReady(false);  // no shared memory on macOS
    }

    // BackendWindow
    void Navigate(const std::string& path) override {
        if (view_) [view_ loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:Ns(origin_ + path)]]];
    }
    void PostToPage(const std::string& text) override {
        if (!view_) return;
        // Messages run in order in the page. Before uniapi.js has loaded there is nobody to
        // receive them, as on the other platforms: the page starts with "hello".
        NSString* script = Ns("window.__uniapi&&window.__uniapi.receive(" + JsString(text) + ")");
        [view_ evaluateJavaScript:script inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:nil];
    }
    void ShareRegion(SharedRegion&, const std::string&, bool) override {}  // never made on macOS

    void SetTitle(const std::string& title) override {
        setup_.title = title;
        if (window_) window_.title = Ns(title);
    }

    void SetSize(int width, int height) override {
        setup_.width = width;  // also the shape keepAspectRatio keeps
        setup_.height = height;
        if (!window_ || IsFullscreen()) return;  // before it exists: it opens at this size
        ApplyAspectRatio();
        // The content area resized, the window's top-left corner kept (Cocoa's origin is the
        // bottom-left).
        NSRect content = [window_ contentRectForFrameRect:window_.frame];
        content.origin.y += content.size.height - height;
        content.size = NSMakeSize(width, height);
        [window_ setFrame:[window_ frameRectForContentRect:content] display:YES animate:NO];
    }

    void SetFullscreen(bool on) override {
        setup_.fullscreen = on;
        wantFullscreen_ = on;
        ApplyFullscreen();
    }

    bool IsFullscreen() const override {
        if (!window_) return setup_.fullscreen;
        return wantFullscreen_ ? *wantFullscreen_ : ActuallyFullscreen();
    }

    void SetTitleBar(TitleBar mode) override {
        setup_.titleBar = mode;
        ApplyTitleBar();
    }
    TitleBar GetTitleBar() const override { return setup_.titleBar; }

    void Show() override { Reveal(); }

    // Brings the window on screen in front (restored if minimised) or orders it out. The page
    // runs either way. A modal window holds the main window only while it is shown. Before the
    // window exists: how it opens.
    void SetVisible(bool visible) override;

    bool IsVisible() const override { return !setup_.hidden; }

    // --- Placement ---
    Size CurrentSize() const override {
        if (!window_) return {setup_.width, setup_.height};
        const NSRect content = [window_ contentRectForFrameRect:window_.frame];
        return {int(lround(NSWidth(content))), int(lround(NSHeight(content)))};
    }

    std::optional<Point> Position() const override {
        NSPoint point = position_;
        if (window_) {
            point = TopLeft(window_.frame);
        } else if (!hasPosition_) {
            return std::nullopt;  // it opens centred: where isn't known yet
        }
        return Point{int(lround(point.x)), int(lround(point.y))};
    }

    bool SetPosition(int x, int y) override {
        position_ = NSMakePoint(x, y);
        hasPosition_ = true;
        if (window_ && !IsFullscreen()) [window_ setFrameTopLeftPoint:CocoaPoint(position_)];
        return true;
    }

    void Center() override {
        hasPosition_ = false;  // before it exists: it opens centred anyway
        if (window_ && !IsFullscreen()) [window_ center];
    }

    WindowState State() const override {
        if (!window_ || setup_.hidden) return statePending_ ? state_ : WindowState::Normal;
        if (window_.isMiniaturized) return WindowState::Minimized;
        return window_.isZoomed ? WindowState::Maximized : WindowState::Normal;
    }

    // On a window on screen; a hidden one (or one not yet made) takes it when shown.
    void SetState(WindowState state) override {
        state_ = state;
        if (!window_ || setup_.hidden) {
            statePending_ = true;
            return;
        }
        statePending_ = false;
        ApplyState();
    }

    // The normal frame: its top-left corner, and its content's size.
    Bounds NormalBounds() const override {
        if (!window_ || !hasNormalFrame_) {
            const Size size = CurrentSize();
            const std::optional<Point> point = Position();
            return {point ? point->x : 0, point ? point->y : 0, size.width, size.height, true};
        }
        const NSRect content = [window_ contentRectForFrameRect:normalFrame_];
        const NSPoint topLeft = TopLeft(normalFrame_);
        return {int(lround(topLeft.x)), int(lround(topLeft.y)), int(lround(NSWidth(content))),
                int(lround(NSHeight(content))), true};
    }

    // Above other apps' windows: the floating level, as utility panels.
    bool SetAlwaysOnTop(bool on) override {
        onTop_ = on;
        if (window_) window_.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
        return true;
    }

    bool IsAlwaysOnTop() const override { return window_ ? window_.level >= NSFloatingWindowLevel : onTop_; }

    // The screens' whole frames, in top-left coordinates. The first is the primary one (the menu
    // bar's, at 0,0); current is the one most of the window is on (none before it exists).
    std::vector<Screen> Screens() const override {
        InitApp();
        NSScreen* current = window_ ? window_.screen : nil;
        NSScreen* primary = NSScreen.screens.firstObject;
        std::vector<Screen> screens;
        for (NSScreen* s in NSScreen.screens) {
            const NSRect frame = s.frame;
            const NSPoint topLeft = TopLeft(frame);
            Screen screen;
            screen.x = int(lround(topLeft.x));
            screen.y = int(lround(topLeft.y));
            screen.width = int(lround(NSWidth(frame)));
            screen.height = int(lround(NSHeight(frame)));
            screen.scale = s.backingScaleFactor;
            screen.primary = s == primary;
            screen.current = current != nil && s == current;
            screens.push_back(screen);
        }
        return screens;
    }

    // Moved, resized, minimised...: the normal frame kept (while normal), and the core told.
    void OnPlacementChanged() {
        if (!window_) return;
        if (!window_.isMiniaturized && !window_.isZoomed && !ActuallyFullscreen() &&
            CFAbsoluteTimeGetCurrent() > zoomingUntil_) {
            normalFrame_ = window_.frame;
            hasNormalFrame_ = true;
        }
        if (!closing_ && events_.onPlacementChanged) events_.onPlacementChanged();
    }

    // Zooming (the title bar double-clicked, maximize()): the frame before is the normal one,
    // and the frames of its animation aren't.
    void OnZooming(NSWindow* window) {
        if (!window.isZoomed) {
            normalFrame_ = window.frame;
            hasNormalFrame_ = true;
        }
        zoomingUntil_ = CFAbsoluteTimeGetCurrent() + 1.0;
    }

    // The close button, Cmd+W (performClose:), or for the main window the app's Quit: the core
    // decides. Close() ([window close]) doesn't ask.
    bool AskClose(CloseReason reason) {
        if (closing_ || !window_) return true;
        return !events_.onCloseRequested || events_.onCloseRequested(reason);
    }

    // Mac windows have no icons of their own; the main window's sets the app's (MacBackend).
    bool SetIcon(const unsigned char*, size_t) override { return false; }

    // Nothing: WKWebView has no public way to open the Web Inspector; with AppOptions::devTools
    // the page is inspectable, so Safari's Develop menu opens it.
    void OpenDevTools() override {}

    void Close() override {
        if (window_ && !closing_) [window_ close];  // as its close button: onClosed follows
    }

    // The forced appearance (Dark, Light) is the window's title bar and frame; the page keeps
    // the system's, as on Windows: its prefers-color-scheme doesn't change with setTitleBar.
    void FollowSystemAppearance() {
        if (view_) view_.appearance = setup_.titleBar == TitleBar::System ? nil : NSApp.effectiveAppearance;
    }

    // From the Objective-C delegates.
    void OnClosing();
    void OnFullscreenChanged() {
        Debug("full screen: transition done, %s", ActuallyFullscreen() ? "in" : "out");
        transitioning_ = false;
        if (wantFullscreen_ && *wantFullscreen_ == ActuallyFullscreen()) wantFullscreen_.reset();
        if (!wantFullscreen_) return;
        // A request that came during the transition. Not from inside AppKit's notification: a
        // toggleFullScreen: there is ignored (seen on macOS 26).
        std::weak_ptr<int> alive = alive_;
        MacWindow* self = this;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!alive.expired()) self->ApplyFullscreen();
        });
    }
    void OnFullscreenStarting() { transitioning_ = true; }
    void OnScriptMessage(WKScriptMessage* message) {
        // Only the top frame of pages the app serves itself may talk to it.
        if (!message.frameInfo.isMainFrame || ![message.body isKindOfClass:[NSString class]]) return;
        const std::string url = Utf8(view_.URL.absoluteString);
        if (url.compare(0, origin_.size() + 1, origin_ + "/") != 0) return;
        if (events_.onMessage) events_.onMessage(Utf8(message.body));
    }
    void OnNavigationStarting() {
        if (events_.onNavigationStarting) events_.onNavigationStarting();
    }
    void OnPageLoaded() {
        if (!invisible_ || setup_.showWhenReady) return;
        std::weak_ptr<int> alive = alive_;
        MacWindow* self = this;
        [view_ callAsyncJavaScript:kAfterDrawn arguments:nil inFrame:nil inContentWorld:UniapiWorld()
                 completionHandler:^(id, NSError* error) {
                     if (alive.expired()) return;  // the window is gone
                     if (error) Debug("waiting for the page's first frame: %s", error.localizedDescription.UTF8String);
                     self->StartRevealTimer(kRevealAfterDrawMs);
                 }];
    }
    void OnSchemeTask(id<WKURLSchemeTask> task);

    // Where a navigation goes (the app's pages, a file, outside), and one outside reported.
    Destination DestinationOfUrl(NSURL* url) const { return DestinationOf(origin_, Utf8(url.absoluteString)); }
    void OnExternal(NSURL* url) {
        if (events_.onExternalNavigation) events_.onExternalNavigation(Utf8(url.absoluteString));
    }
    // Files dropped: their paths, and where, in CSS pixels from the page's top-left corner.
    void OnDrop(const std::vector<std::string>& paths, int x, int y) {
        if (!paths.empty() && events_.onFileDrop) events_.onFileDrop(paths, x, y);
    }

    void Hold(NSWindow* holder) {
        if (auto* window = static_cast<UniapiNSWindow*>(window_)) window.heldBy = holder;
    }

    // A notification clicked (the main window's events).
    void NotificationClicked(const std::string& id) {
        if (events_.onNotificationClicked) events_.onNotificationClicked(id);
    }

    // Files the system asked the app to open (the main window's events).
    void OpenFiles(const std::vector<std::string>& paths) {
        if (!paths.empty() && events_.onOpenFiles) events_.onOpenFiles(paths);
    }

private:
    void ReleaseHold();  // a modal window lets the main window take input again

    // The state asked for (minimize, maximize, restore), on a window on screen.
    void ApplyState() {
        if (!window_ || setup_.hidden) return;
        switch (state_) {
        case WindowState::Minimized:
            if (!window_.isMiniaturized) [window_ miniaturize:nil];
            break;
        case WindowState::Maximized:
            if (window_.isMiniaturized) [window_ deminiaturize:nil];
            if (!window_.isZoomed) {
                normalFrame_ = window_.frame;  // what restore gives back
                hasNormalFrame_ = true;
                [window_ zoom:nil];
            }
            break;
        case WindowState::Normal:
            if (window_.isMiniaturized) [window_ deminiaturize:nil];
            if (window_.isZoomed) [window_ zoom:nil];
            break;
        }
    }

    // Hides the page until it has drawn again, as when the window opened: for a window started
    // hidden whose page was revealed while it was off screen (by the timeout, or app.show())
    // but hasn't been drawn there.
    void RevealWhenDrawn() {
        invisible_ = true;
        window_.alphaValue = 0.0;
        StartRevealTimer(kRevealTimeoutMs);
        std::weak_ptr<int> alive = alive_;
        MacWindow* self = this;
        [view_ callAsyncJavaScript:kAfterDrawn arguments:nil inFrame:nil inContentWorld:UniapiWorld()
                 completionHandler:^(id, NSError* error) {
                     if (alive.expired()) return;
                     if (error) Debug("waiting for the page's first frame: %s", error.localizedDescription.UTF8String);
                     self->StartRevealTimer(kRevealAfterDrawMs);
                 }];
    }

    bool ActuallyFullscreen() const { return window_ && (window_.styleMask & NSWindowStyleMaskFullScreen) != 0; }

    // One transition at a time: AppKit ignores toggleFullScreen: during one. A window not yet on
    // screen (startHidden) enters full screen when it is first shown.
    void ApplyFullscreen() {
        if (!window_ || transitioning_ || !wantFullscreen_ || !everShown_) return;
        if (*wantFullscreen_ == ActuallyFullscreen()) {
            wantFullscreen_.reset();
            return;
        }
        Debug("full screen: %s", *wantFullscreen_ ? "entering" : "leaving");
        transitioning_ = true;
        [window_ toggleFullScreen:nil];
    }

    void ApplyAspectRatio() {
        if (window_ && setup_.keepAspectRatio && setup_.width > 0 && setup_.height > 0) {
            window_.contentAspectRatio = NSMakeSize(setup_.width, setup_.height);
        }
    }

    void ApplyTitleBar() {
        if (!window_) return;  // before it exists: how it opens
        switch (setup_.titleBar) {
            case TitleBar::Dark: window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; break;
            case TitleBar::Light: window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua]; break;
            case TitleBar::System: window_.appearance = nil; break;
        }
        FollowSystemAppearance();
    }

    void StartRevealTimer(int ms) {
        const int generation = ++revealGeneration_;
        std::weak_ptr<int> alive = alive_;
        MacWindow* self = this;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(ms) * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (!alive.expired() && self->revealGeneration_ == generation) self->Reveal();
        });
    }

    // Shows the page: once, after its first page has drawn (or the page said so), or the timeout.
    void Reveal() {
        ++revealGeneration_;  // cancels a reveal timer
        if (!invisible_ || !window_) return;
        invisible_ = false;
        window_.alphaValue = 1.0;
    }

    MacBackend& backend_;
    Setup setup_;
    BackendEvents events_;
    std::string origin_;
    UniapiNSWindow* window_ = nil;
    WKWebView* view_ = nil;
    UniapiWindowDelegate* windowDelegate_ = nil;
    UniapiViewDelegate* viewDelegate_ = nil;
    UniapiSchemeHandler* scheme_ = nil;
    __weak NSWindow* owner_ = nil;
    bool created_ = false;
    bool closing_ = false;
    bool invisible_ = false;
    bool everShown_ = false;  // it has been on screen (a window started hidden: once shown)
    // Placement: before the window exists (and for a state while hidden), what it takes when
    // made or shown; then its normal frame (not zoomed, minimised, full screen).
    bool hasPosition_ = false;
    NSPoint position_ = NSZeroPoint;  // top-left coordinates (TopLeft)
    WindowState state_ = WindowState::Normal;
    bool statePending_ = false;
    bool onTop_ = false;
    NSRect normalFrame_ = NSZeroRect;
    bool hasNormalFrame_ = false;
    CFAbsoluteTime zoomingUntil_ = 0;  // an animated zoom: its frames aren't normal ones
    int revealGeneration_ = 0;
    bool transitioning_ = false;
    std::optional<bool> wantFullscreen_;  // asked for and not reached yet
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // async callbacks check it
};

// --- Notifications ---------------------------------------------------------------------------------
// Notification Center takes notifications only from an app it can name: one in an .app bundle
// with a bundle identifier. (Asked for its notification centre, a plain executable raises an
// exception.)
NSString* NotificationsUnavailable() {
    NSBundle* bundle = NSBundle.mainBundle;
    if (bundle.bundleIdentifier.length == 0 || ![bundle.bundlePath.pathExtension isEqualToString:@"app"]) {
        return @"macOS shows notifications only from an app in an .app bundle (uniapi_install() makes one)";
    }
    return nil;
}

// Each notification goes to Notification Center as "<id>#<uuid>", so every one is new: a request
// with the identifier of one still in the list would only update that entry, without a banner.
NSString* const kIdSeparator = @"#";

NSString* NotificationId(NSString* identifier) {
    NSRange last = [identifier rangeOfString:kIdSeparator options:NSBackwardsSearch];
    return last.location == NSNotFound ? identifier : [identifier substringToIndex:last.location];
}

NSString* const kNotAllowed = @"notifications are turned off for this app: allow them in System Settings, Notifications";

// Permission: the app's setting, or the person's answer the first time. While macOS is asking (its
// prompt waits until the person answers, however long that takes), it refuses every further
// request at once; so one question at a time, and the notifications sent meanwhile wait for its
// answer. Main thread only.
bool gAsking = false;
NSMutableArray<void (^)(NSString*)>* gWaiting = nil;

void Answered(NSString* refusal) {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSArray<void (^)(NSString*)>* waiting = [gWaiting copy];
        [gWaiting removeAllObjects];
        gAsking = false;
        for (void (^then)(NSString*) in waiting) then(refusal);
    });
}

// then(nil) once notifications are allowed, then(why not) otherwise.
void WhenAllowed(void (^then)(NSString* refusal)) {
    if (!gWaiting) gWaiting = [NSMutableArray array];
    [gWaiting addObject:then];
    if (gAsking) return;
    gAsking = true;
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
        if (settings.authorizationStatus != UNAuthorizationStatusNotDetermined) {
            Answered(settings.authorizationStatus == UNAuthorizationStatusDenied ? kNotAllowed : nil);
            return;
        }
        Debug("asking the person to allow notifications");
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                              completionHandler:^(BOOL granted, NSError* error) {
                                  if (granted) {
                                      Answered(nil);
                                  } else if (!error || ([error.domain isEqualToString:UNErrorDomain] &&
                                                        error.code == UNErrorCodeNotificationsNotAllowed)) {
                                      Answered(kNotAllowed);
                                  } else {
                                      Answered(error.localizedDescription);
                                  }
                              }];
    }];
}

UniapiNotificationDelegate* gNotificationDelegate = nil;

// --- The app: event loop, timers, posting, dialogs, and its windows ------------------------------

class MacBackend final : public Backend {
public:
    MacBackend(const AppOptions& options, BackendEvents events)
        : options_(options), origin_(std::string(kScheme) + "://" + options.host), uiThread_(std::this_thread::get_id()),
          posted_(std::make_shared<PostQueue>()) {
        posted_->backend = this;
        MacWindow::Setup setup;
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
        main_ = std::make_unique<MacWindow>(*this, std::move(setup), std::move(events));
    }

    ~MacBackend() override {
        for (auto& entry : timers_) [entry.second.timer invalidate];
        {
            std::lock_guard<std::mutex> lock(posted_->mutex);
            posted_->backend = nullptr;  // a posted block still queued finds nobody
        }
        if (gRunning == this) gRunning = nullptr;
        if (gNotifying == this) gNotifying = nullptr;
        main_.reset();
    }

    int Run() override {
        InitApp();
        // Clicks on notifications come to this backend, also the one that launched the app:
        // before the event loop.
        gNotifying = this;
        if (!NotificationsUnavailable() && !gNotificationDelegate) {
            gNotificationDelegate = [UniapiNotificationDelegate new];
            UNUserNotificationCenter.currentNotificationCenter.delegate = gNotificationDelegate;
        }
        store_ = DataStoreFor(options_.dataDirectory);
        main_->Create(origin_, store_, nil);
        CreateWindows();  // the further windows opened before run()
        running_ = true;
        main_->ReportReady();  // the core navigates now
        if (!main_->Handle()) return 0;  // closed from onReady
        gRunning = this;
        [NSApp run];  // until the main window closes (OnMainClosed)
        if (gRunning == this) gRunning = nullptr;
        running_ = false;
        return 0;
    }

    void Quit() override {
        if (main_) main_->Close();
    }

    void Navigate(const std::string& path) override { main_->Navigate(path); }
    void PostToPage(const std::string& text) override { main_->PostToPage(text); }
    std::unique_ptr<SharedRegion> CreateSharedRegion(size_t) override { return nullptr; }
    void ShareRegion(SharedRegion& region, const std::string& infoJson, bool writable) override {
        main_->ShareRegion(region, infoJson, writable);
    }

    // Posted functions run in order, on the main thread, also while a dialog or a window drag
    // runs its own loop (common modes).
    void Post(std::function<void()> fn) override {
        std::shared_ptr<PostQueue> queue = posted_;
        bool schedule;
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->items.push_back(std::move(fn));
            schedule = !queue->scheduled;
            queue->scheduled = true;
        }
        if (!schedule) return;
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
            std::vector<std::function<void()>> items;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                if (!queue->backend) return;
                items.swap(queue->items);
                queue->scheduled = false;
            }
            for (auto& item : items) item();
        });
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }

    bool OnUiThread() const override { return std::this_thread::get_id() == uiThread_; }

    void SetTitle(const std::string& title) override { main_->SetTitle(title); }
    void SetSize(int width, int height) override { main_->SetSize(width, height); }
    void SetFullscreen(bool on) override { main_->SetFullscreen(on); }
    bool IsFullscreen() const override { return main_->IsFullscreen(); }
    void SetTitleBar(TitleBar mode) override { main_->SetTitleBar(mode); }
    TitleBar GetTitleBar() const override { return main_->GetTitleBar(); }
    void Show() override { main_->Show(); }
    void SetVisible(bool visible) override { main_->SetVisible(visible); }
    bool IsVisible() const override { return main_->IsVisible(); }
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
    // The system's WebKit, which WKWebView is. Its bundle version carries the macOS generation
    // before the WebKit version ("20621.1.15.11.10" is WebKit 621.1.15.11.10).
    WebEngine Engine() const override {
        WebEngine engine;
        engine.name = "WebKit";
        @autoreleasepool {
            id version = [NSBundle bundleForClass:[WKWebView class]].infoDictionary[@"CFBundleVersion"];
            if ([version isKindOfClass:[NSString class]] && [version length]) {
                engine.version = WebKitVersion(std::string([version UTF8String]));
            }
        }
        return engine;
    }
    // The screens' visible frames (without the menu bar and Dock), in top-left coordinates.
    std::vector<Rect> ScreenAreas() const override {
        InitApp();
        std::vector<Rect> areas;
        for (NSScreen* screen in NSScreen.screens) {
            const NSRect visible = screen.visibleFrame;
            const NSPoint topLeft = TopLeft(visible);
            areas.push_back({int(lround(topLeft.x)), int(lround(topLeft.y)), int(lround(NSWidth(visible))),
                             int(lround(NSHeight(visible)))});
        }
        return areas;
    }

    void OpenDevTools() override { main_->OpenDevTools(); }

    // The app's icon in the Dock and the app switcher (Mac windows have none of their own). An
    // .app bundle has one already, from uniapi_install(); this replaces it while the app runs.
    bool SetIcon(const unsigned char* png, size_t size) override {
        static const unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        if (!png || size < sizeof kPngSignature || std::memcmp(png, kPngSignature, sizeof kPngSignature) != 0) return false;
        InitApp();
        NSImage* image = [[NSImage alloc] initWithData:[NSData dataWithBytes:png length:size]];
        if (!image || !image.isValid || image.representations.count == 0) return false;
        NSApp.applicationIconImage = image;
        return true;
    }

    int AddTimer(std::chrono::milliseconds interval, std::function<void()> fn) override {
        const int id = nextTimer_++;
        Timer& timer = timers_[id];
        // Held by a shared_ptr: the callback may cancel its own timer, and its state (a mutable
        // lambda's captures) lasts from tick to tick.
        timer.fn = std::make_shared<std::function<void()>>(std::move(fn));
        const NSTimeInterval seconds = double(std::max<int64_t>(1, interval.count())) / 1000.0;
        MacBackend* self = this;
        // Like on Windows and Linux, the first tick comes right away, then one per interval.
        timer.timer = [[NSTimer alloc] initWithFireDate:[NSDate date] interval:seconds repeats:YES
                                                  block:^(NSTimer*) { self->OnTimer(id); }];
        [[NSRunLoop mainRunLoop] addTimer:timer.timer forMode:NSRunLoopCommonModes];
        return id;
    }

    void RemoveTimer(int id) override {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        [it->second.timer invalidate];  // fine from inside its own callback
        timers_.erase(it);
    }

    void ClipboardText(std::function<void(std::optional<std::string>)> done) override {
        NSString* text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
        done(text ? std::optional<std::string>(Utf8(text)) : std::nullopt);
    }

    bool SetClipboardText(const std::string& text) override {
        NSString* string = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
        if (!string) return false;
        NSPasteboard* board = NSPasteboard.generalPasteboard;
        [board clearContents];
        return [board setString:string forType:NSPasteboardTypeString];
    }

    // Notification Center, from an .app bundle only (NotificationsUnavailable says why otherwise).
    // done comes on the main thread, unless the backend has gone by then.
    void Notify(const Notification& n, std::function<void(const std::string&)> done) override {
        std::weak_ptr<int> alive = alive_;
        auto finish = [alive, done](NSString* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!alive.expired()) done(error ? Utf8(error) : std::string());
            });
        };
        if (NSString* unavailable = NotificationsUnavailable()) {
            finish(unavailable);
            return;
        }
        UNMutableNotificationContent* content = [UNMutableNotificationContent new];
        content.title = Ns(n.title);
        content.body = Ns(n.body);
        content.sound = UNNotificationSound.defaultSound;
        NSString* identifier = [NSString stringWithFormat:@"%@%@%@", Ns(n.id), kIdSeparator, NSUUID.UUID.UUIDString];
        UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:nil];
        WhenAllowed(^(NSString* refusal) {
            if (refusal) {
                finish(refusal);
                return;
            }
            [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request
                                                                 withCompletionHandler:^(NSError* error) {
                                                                     finish(error.localizedDescription);
                                                                 }];
        });
    }

    // A click on one of this app's notifications (UniapiNotificationDelegate), on the main thread.
    void NotificationClicked(const std::string& id) { main_->NotificationClicked(id); }

    // Files to open, from the Finder or the Dock (UniapiAppDelegate), on the main thread.
    void OpenFiles(const std::vector<std::string>& paths) { main_->OpenFiles(paths); }

    std::optional<std::string> SaveFileDialog(const SaveDialogOptions& options) override {
        ActivateForDialog();  // also before run()
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = Ns(options.title);
        if (options.title != "Save") panel.message = Ns(options.title);
        if (!options.defaultName.empty()) panel.nameFieldStringValue = Ns(options.defaultName);
        NSArray<UTType*>* types = ContentTypes(AllPatterns(options));
        if (types.count) panel.allowedContentTypes = types;
        SetDirectory(panel, options.directory);
        // The panel asks before overwriting.
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return std::nullopt;
        return std::string(panel.URL.fileSystemRepresentation);
    }

    std::vector<std::string> OpenFileDialog(const OpenDialogOptions& options) override {
        ActivateForDialog();  // also before run()
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = Ns(options.title);
        if (options.title != "Open") panel.message = Ns(options.title);
        panel.canChooseFiles = !options.folder;
        panel.canChooseDirectories = options.folder;
        panel.allowsMultipleSelection = options.multiple;
        panel.canCreateDirectories = options.folder;
        if (!options.folder) {
            NSArray<UTType*>* types = ContentTypes(AllPatterns(options));
            if (types.count) panel.allowedContentTypes = types;
        }
        SetDirectory(panel, options.directory);
        std::vector<std::string> paths;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in panel.URLs) paths.emplace_back(url.fileSystemRepresentation);
        }
        return paths;
    }

    std::unique_ptr<BackendWindow> OpenWindow(const WindowOptions& options, BackendEvents events) override {
        MacWindow::Setup setup;
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
        auto window = std::make_unique<MacWindow>(*this, std::move(setup), std::move(events));
        windows_.push_back(window.get());
        if (running_) {  // otherwise Run() makes it, with the main window
            window->Create(origin_, store_, main_->Handle());
            window->ReportReady();
        }
        return window;
    }

    // From MacWindow.
    void Forget(MacWindow* window) { windows_.erase(std::remove(windows_.begin(), windows_.end(), window), windows_.end()); }
    MacWindow* Main() const { return main_.get(); }
    // Every further window closes with the main one (their onClosed first), then the loop ends.
    void CloseWindows() {
        for (MacWindow* window : std::vector<MacWindow*>(windows_)) window->Destroy();
    }
    void StopLoop() {
        if (!running_) return;
        [NSApp stop:nil];
        // -stop: takes effect after the next event: send one, so run() returns now.
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                           timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0]
                 atStart:YES];
    }
    void FollowSystemAppearance() {
        main_->FollowSystemAppearance();
        for (MacWindow* window : windows_) window->FollowSystemAppearance();
    }

private:
    void CreateWindows() {
        for (MacWindow* window : std::vector<MacWindow*>(windows_)) {
            if (window->Created()) continue;
            window->Create(origin_, store_, window->GetSetup().owned ? main_->Handle() : nil);
            window->ReportReady();
        }
    }

    void OnTimer(int id) {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        std::shared_ptr<std::function<void()>> fn = it->second.fn;
        (*fn)();
    }

    struct Timer {
        NSTimer* timer = nil;
        std::shared_ptr<std::function<void()>> fn;
    };

    // Shared with the blocks that drain it, which may run after the backend has gone.
    struct PostQueue {
        std::mutex mutex;
        std::vector<std::function<void()>> items;
        bool scheduled = false;
        MacBackend* backend = nullptr;  // nullptr once it has gone
    };

    AppOptions options_;
    std::string origin_;  // "uniapi://<host>"
    std::thread::id uiThread_;
    std::shared_ptr<PostQueue> posted_;
    std::unique_ptr<MacWindow> main_;
    std::vector<MacWindow*> windows_;  // further windows, until they are destroyed
    WKWebsiteDataStore* store_ = nil;  // one profile for every window
    std::map<int, Timer> timers_;
    int nextTimer_ = 1;
    bool running_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // notification answers check it
};

// --- MacWindow, the parts that need MacBackend ----------------------------------------------------

MacWindow::~MacWindow() {
    if (!setup_.main) backend_.Forget(this);
    alive_.reset();
    if (scheme_) scheme_.host = nullptr;
    if (view_) ((UniapiWebView*)view_).host = nullptr;
    if (viewDelegate_) viewDelegate_.host = nullptr;
    if (windowDelegate_) windowDelegate_.host = nullptr;
    if (setup_.modal) {
        if (MacWindow* main = backend_.Main(); main && main != this) main->Hold(nil);
    }
    if (window_) {
        // Closed without events (the App ending while it is open).
        UniapiNSWindow* window = window_;
        [view_ stopLoading];
        [view_.configuration.userContentController removeAllScriptMessageHandlers];
        window_ = nil;
        view_ = nil;
        [window close];
    }
}

void MacWindow::Create(const std::string& origin, WKWebsiteDataStore* store, NSWindow* owner) {
    if (created_) return;
    created_ = true;
    origin_ = origin;
    owner_ = owner;

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (setup_.resizable) style |= NSWindowStyleMaskResizable;
    window_ = [[UniapiNSWindow alloc] initWithContentRect:NSMakeRect(0, 0, setup_.width, setup_.height)
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    window_.releasedWhenClosed = NO;
    window_.title = Ns(setup_.title);
    window_.tabbingMode = NSWindowTabbingModeDisallowed;
    window_.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    if (!setup_.resizable) [window_ standardWindowButton:NSWindowZoomButton].enabled = NO;
    if (setup_.minWidth > 0 || setup_.minHeight > 0) {
        window_.contentMinSize = NSMakeSize(std::max(setup_.minWidth, 1), std::max(setup_.minHeight, 1));
    }
    if (setup_.maxWidth > 0 || setup_.maxHeight > 0) {
        window_.contentMaxSize = NSMakeSize(setup_.maxWidth > 0 ? setup_.maxWidth : CGFLOAT_MAX,
                                            setup_.maxHeight > 0 ? setup_.maxHeight : CGFLOAT_MAX);
    }
    // AppOptions::backgroundColor: the window's, which shows until the page has drawn.
    if (const int rgb = setup_.background; rgb >= 0) {
        window_.backgroundColor = [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                                                      green:((rgb >> 8) & 0xff) / 255.0
                                                       blue:(rgb & 0xff) / 255.0
                                                      alpha:1.0];
    }
    ApplyAspectRatio();
    windowDelegate_ = [UniapiWindowDelegate new];
    windowDelegate_.host = this;
    window_.delegate = windowDelegate_;

    // Where it opens: where the core said (setPosition, a remembered placement), else centred over
    // the main window when owned, else centred on the screen.
    if (hasPosition_) {
        [window_ setFrameTopLeftPoint:CocoaPoint(position_)];
    } else if (owner) {
        const NSRect over = owner.frame;
        NSRect frame = window_.frame;
        frame.origin.x = NSMidX(over) - frame.size.width / 2;
        frame.origin.y = NSMidY(over) - frame.size.height / 2;
        [window_ setFrame:frame display:NO];
    } else {
        [window_ center];
    }

    WKWebViewConfiguration* config = [WKWebViewConfiguration new];
    config.websiteDataStore = store;
    scheme_ = [UniapiSchemeHandler new];
    scheme_.host = this;
    [config setURLSchemeHandler:scheme_ forURLScheme:@(kScheme)];
    config.preferences.elementFullscreenEnabled = YES;  // the page's requestFullscreen()
    viewDelegate_ = [UniapiViewDelegate new];
    viewDelegate_.host = this;
    WKUserContentController* content = config.userContentController;
    [content addScriptMessageHandler:viewDelegate_ contentWorld:UniapiWorld() name:@"uniapi"];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kBridgePage
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:WKContentWorld.pageWorld]];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kBridgeForward
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:UniapiWorld()]];

    UniapiWebView* webView = [[UniapiWebView alloc] initWithFrame:NSMakeRect(0, 0, setup_.width, setup_.height)
                                                    configuration:config];
    webView.host = this;
    view_ = webView;
    view_.navigationDelegate = viewDelegate_;
    view_.UIDelegate = viewDelegate_;
    // AppOptions::devTools: Safari's Develop menu lists the page (as WebView2's DevTools); off,
    // it doesn't.
    view_.inspectable = setup_.devTools;
    // AppOptions::contextMenu off: the right-click menu keeps its text editing items only.
    webView.editingMenuOnly = !setup_.contextMenu;
    // AppOptions::backgroundColor: the web view draws no background of its own, so the window's
    // colour shows through a page with none, and beyond the page when it is scrolled past its
    // ends.
    if (setup_.background >= 0) {
        [view_ setValue:@NO forKey:@"drawsBackground"];
        view_.underPageBackgroundColor = window_.backgroundColor;
    }
    // No Force Touch link previews: with them WKWebView puts a gesture recognizer (immediate
    // actions: Look Up, previews) in front of the page's mouse events, which holds a drag's
    // first moves back while it decides, so a control started moving late, then followed.
    view_.allowsLinkPreview = NO;
    view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window_.contentView = view_;
    ApplyTitleBar();

    // Never an empty or white window, or a page half built: the window is on screen but fully
    // transparent until its page has drawn (Reveal), as Windows cloaks it. The page loads and
    // runs as if shown.
    window_.alphaValue = 0.0;
    invisible_ = true;
    StartRevealTimer(setup_.showWhenReady ? kRevealTimeoutReadyMs : kRevealTimeoutMs);
    // Started hidden (or hidden before it was made): made, its page loading, but ordered in
    // only by SetVisible.
    everShown_ = !setup_.hidden;
    if (setup_.hidden) {
        Debug("window \"%s\" opens hidden", setup_.title.c_str());
    } else if (setup_.activate) {
        ActivateApp();
        [window_ makeKeyAndOrderFront:nil];
    } else {
        [window_ orderFront:nil];  // without taking focus from the app you're in
    }
    [window_ makeFirstResponder:view_];  // keyboard shortcuts work without a click
    if (setup_.modal && owner && !setup_.hidden) {
        if (MacWindow* main = backend_.Main()) main->Hold(window_);
    }
    if (onTop_) window_.level = NSFloatingWindowLevel;
    normalFrame_ = window_.frame;
    hasNormalFrame_ = true;
    if (!setup_.hidden && statePending_) {  // minimize(), maximize() before it existed
        statePending_ = false;
        ApplyState();
    }
    if (setup_.fullscreen) SetFullscreen(true);  // a hidden window: once shown
}

void MacWindow::SetVisible(bool visible) {
    setup_.hidden = !visible;
    if (!window_ || closing_) return;
    if (!visible) {
        if (setup_.modal) ReleaseHold();
        [window_ orderOut:nil];
        return;
    }
    const bool first = !everShown_;
    if (first) {
        everShown_ = true;
        // A page still hidden waits for its frame (or app.show()) as usual.
        if (!invisible_) RevealWhenDrawn();
    }
    if (window_.isMiniaturized) [window_ deminiaturize:nil];
    if (setup_.activate) {
        ActivateApp();
        [window_ makeKeyAndOrderFront:nil];
    } else {
        [window_ orderFront:nil];
    }
    if (setup_.modal && owner_) {
        if (MacWindow* main = backend_.Main(); main && main != this) main->Hold(window_);
    }
    if (statePending_) {  // minimize(), maximize() while it was hidden
        statePending_ = false;
        ApplyState();
    }
    if (first) ApplyFullscreen();  // asked for while it was hidden
}

void MacWindow::ReleaseHold() {
    if (MacWindow* main = backend_.Main(); main && main != this) main->Hold(nil);
}

void MacWindow::OnClosing() {
    if (closing_) return;
    closing_ = true;
    if (setup_.modal) {
        if (MacWindow* main = backend_.Main(); main && main != this) main->Hold(nil);
        if (NSWindow* owner = owner_) [owner makeKeyAndOrderFront:nil];
    }
    if (setup_.main) backend_.CloseWindows();  // every further window closes with the main one
    if (events_.onClosed) events_.onClosed();  // a further window: the core destroys this later (posted)
    // Released after the close has finished (AppKit is still inside -close).
    UniapiNSWindow* window = window_;
    WKWebView* view = view_;
    [view stopLoading];
    [view.configuration.userContentController removeAllScriptMessageHandlers];
    scheme_.host = nullptr;
    ((UniapiWebView*)view).host = nullptr;
    viewDelegate_.host = nullptr;
    windowDelegate_.host = nullptr;
    window_ = nil;
    view_ = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        (void)window;
        (void)view;
    });
    if (setup_.main) backend_.StopLoop();
}

// Answers a request for uniapi://<host>/...: the core's response, or 404. At once, so the task
// can't have been stopped meanwhile.
void MacWindow::OnSchemeTask(id<WKURLSchemeTask> task) {
    NSURLRequest* request = task.request;
    const std::string url = Utf8(request.URL.absoluteString);
    Response response;
    if (url.size() > origin_.size() && url.compare(0, origin_.size(), origin_) == 0 && url[origin_.size()] == '/' &&
        events_.onRequest) {
        Request info;
        info.path = url.substr(origin_.size());
        if (NSString* method = request.HTTPMethod) info.method = Utf8(method);
        if (info.method == "POST") ReadBody(request, info.body);
        events_.onRequest(info, response);
    }

    NSData* body;
    if (response.status == 200) {
        if (response.size == 0) {
            body = [NSData data];
        } else {
            // No copy: the bytes stay owned by keepAlive until WebKit is done with them.
            std::shared_ptr<const void> keepAlive = std::move(response.keepAlive);
            body = [[NSData alloc] initWithBytesNoCopy:const_cast<uint8_t*>(response.data)
                                                length:response.size
                                           deallocator:^(void*, NSUInteger) { (void)keepAlive; }];
        }
    } else {
        body = [@"Not Found" dataUsingEncoding:NSUTF8StringEncoding];
        response.mimeType = "text/plain";
    }
    NSDictionary* headers = @{
        @"Content-Type" : Ns(response.mimeType),
        @"Content-Length" : [NSString stringWithFormat:@"%lu", (unsigned long)body.length],
        @"Cache-Control" : @"no-store",
    };
    NSHTTPURLResponse* reply = [[NSHTTPURLResponse alloc] initWithURL:request.URL
                                                           statusCode:response.status == 200 ? 200 : 404
                                                          HTTPVersion:@"HTTP/1.1"
                                                         headerFields:headers];
    [task didReceiveResponse:reply];
    [task didReceiveData:body];
    [task didFinish];
}

}  // namespace
}  // namespace detail
}  // namespace uniapi

using uniapi::detail::gRunning;

// --- Objective-C implementations -----------------------------------------------------------------

@implementation UniapiWindowDelegate
// The close button and Cmd+W (performClose:) ask; Close() ([window close]) doesn't.
- (BOOL)windowShouldClose:(NSWindow*)sender {
    MacWindow* host = self.host;
    return !host || host->AskClose(uniapi::CloseReason::Window);
}
- (void)windowWillClose:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnClosing();
}
- (void)windowDidMove:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnPlacementChanged();
}
- (void)windowDidResize:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnPlacementChanged();
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnPlacementChanged();
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnPlacementChanged();
}
- (BOOL)windowShouldZoom:(NSWindow*)window toFrame:(NSRect)frame {
    if (MacWindow* host = self.host) host->OnZooming(window);
    return YES;
}
- (void)windowWillEnterFullScreen:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnFullscreenStarting();
}
- (void)windowWillExitFullScreen:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnFullscreenStarting();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnFullscreenChanged();
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    if (MacWindow* host = self.host) host->OnFullscreenChanged();
}
- (void)windowDidFailToEnterFullScreen:(NSWindow*)window {
    if (MacWindow* host = self.host) host->OnFullscreenChanged();
}
- (void)windowDidFailToExitFullScreen:(NSWindow*)window {
    if (MacWindow* host = self.host) host->OnFullscreenChanged();
}
@end

@implementation UniapiViewDelegate

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (MacWindow* host = self.host) host->OnScriptMessage(message);
}

- (void)webView:(WKWebView*)webView didStartProvisionalNavigation:(WKNavigation*)navigation {
    if (MacWindow* host = self.host) host->OnNavigationStarting();
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (MacWindow* host = self.host) host->OnPageLoaded();
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    uniapi::detail::Debug("the page's web process ended; reloading");
    [webView reload];
}

// window.open() and target=_blank links: no window of the page's own (as WebView2's default would,
// a bare browser window outside the app). One to another site opens outside (the core decides).
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
    MacWindow* host = self.host;
    if (host && host->DestinationOfUrl(action.request.URL) == uniapi::detail::Destination::Outside) {
        host->OnExternal(action.request.URL);
    }
    return nil;
}

// Where the top frame may go: the app's pages; not to a file (a file dropped on a page that
// doesn't take it would replace the page); anywhere else, outside the app (the core decides).
// Iframes load what they like.
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
    MacWindow* host = self.host;
    const BOOL newWindow = action.targetFrame == nil;  // target=_blank: refused here, createWebView isn't asked
    if (!host || (!newWindow && !action.targetFrame.isMainFrame)) {
        decision(WKNavigationActionPolicyAllow);
        return;
    }
    const uniapi::detail::Destination where = host->DestinationOfUrl(action.request.URL);
    if (where == uniapi::detail::Destination::App && !newWindow) {
        decision(WKNavigationActionPolicyAllow);
        return;
    }
    decision(WKNavigationActionPolicyCancel);
    if (where == uniapi::detail::Destination::Outside) host->OnExternal(action.request.URL);
}

// The page's alert(), confirm() and prompt(): WKWebView shows nothing without these.
- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))done {
    NSAlert* alert = [NSAlert new];
    alert.messageText = message;
    [alert addButtonWithTitle:@"OK"];
    if (!webView.window) {
        [alert runModal];
        done();
        return;
    }
    [alert beginSheetModalForWindow:webView.window completionHandler:^(NSModalResponse) { done(); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))done {
    NSAlert* alert = [NSAlert new];
    alert.messageText = message;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    if (!webView.window) {
        done([alert runModal] == NSAlertFirstButtonReturn);
        return;
    }
    [alert beginSheetModalForWindow:webView.window
                  completionHandler:^(NSModalResponse response) { done(response == NSAlertFirstButtonReturn); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString*))done {
    NSAlert* alert = [NSAlert new];
    alert.messageText = prompt;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = defaultText ?: @"";
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;
    void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
        done(response == NSAlertFirstButtonReturn ? field.stringValue : nil);
    };
    if (!webView.window) {
        finish([alert runModal]);
        return;
    }
    [alert beginSheetModalForWindow:webView.window completionHandler:finish];
}

// <input type="file">: WKWebView opens nothing without this.
- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>*))done {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.canChooseDirectories = parameters.allowsDirectories;
    panel.canChooseFiles = YES;
    void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
        done(response == NSModalResponseOK ? panel.URLs : nil);
    };
    if (!webView.window) {
        finish([panel runModal]);
        return;
    }
    [panel beginSheetModalForWindow:webView.window completionHandler:finish];
}

@end

@implementation UniapiWebView
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [sender.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                     options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    MacWindow* host = self.host;
    if (urls.count && host) {
        NSPoint point = [self convertPoint:sender.draggingLocation fromView:nil];
        if (!self.isFlipped) point.y = NSHeight(self.bounds) - point.y;
        std::vector<std::string> paths;
        for (NSURL* url in urls) {
            if (url.isFileURL && url.path) paths.emplace_back(url.path.fileSystemRepresentation);
        }
        host->OnDrop(paths, int(point.x), int(point.y));
    }
    return [super performDragOperation:sender];  // the page's drop event (a navigation to the file is refused)
}
// AppOptions::contextMenu off: WebKit's menu keeps its text editing items (by their action, or
// WebKit's identifier for Cut, Copy and Paste), with separators only between them; an empty menu
// isn't shown.
- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event {
    [super willOpenMenu:menu withEvent:event];
    if (!self.editingMenuOnly) return;
    static const SEL kEditing[] = {@selector(cut:),    @selector(copy:),      @selector(paste:), @selector(pasteAsPlainText:),
                                   @selector(delete:), @selector(selectAll:), @selector(undo:),  @selector(redo:)};
    auto editing = [](NSMenuItem* item) {
        for (SEL action : kEditing)
            if (item.action == action) return true;
        NSString* name = item.identifier ?: @"";
        return [name isEqualToString:@"WKMenuItemIdentifierCut"] || [name isEqualToString:@"WKMenuItemIdentifierCopy"] ||
               [name isEqualToString:@"WKMenuItemIdentifierPaste"];
    };
    BOOL afterSeparator = YES;  // (or at the start): no separator may follow
    for (NSInteger i = 0; i < menu.numberOfItems;) {
        NSMenuItem* item = [menu itemAtIndex:i];
        const BOOL keep = item.isSeparatorItem ? !afterSeparator : editing(item);
        if (!keep) {
            [menu removeItemAtIndex:i];
            continue;
        }
        afterSeparator = item.isSeparatorItem;
        ++i;
    }
    if (menu.numberOfItems && [menu itemAtIndex:menu.numberOfItems - 1].isSeparatorItem)
        [menu removeItemAtIndex:menu.numberOfItems - 1];
}
- (void)uniapiTestDropPaths:(NSArray<NSString*>*)paths x:(NSInteger)x y:(NSInteger)y {
    MacWindow* host = self.host;
    if (!host) return;
    std::vector<std::string> list;
    for (NSString* path in paths) list.emplace_back(path.fileSystemRepresentation);
    host->OnDrop(list, int(x), int(y));
}
@end

@implementation UniapiSchemeHandler
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    if (MacWindow* host = self.host) {
        host->OnSchemeTask(task);
        return;
    }
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorResourceUnavailable userInfo:nil]];
}
- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    // Every task is answered as it starts: nothing to stop.
}
@end

@implementation UniapiAppDelegate {
    UniapiAppearanceObserver* _appearance;
}
- (void)applicationWillFinishLaunching:(NSNotification*)notification {
    _appearance = [UniapiAppearanceObserver new];
    [NSApp addObserver:_appearance forKeyPath:@"effectiveAppearance" options:0 context:nullptr];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (!gRunning) return NSTerminateNow;
    NSAppleEventDescriptor* event = [NSAppleEventManager sharedAppleEventManager].currentAppleEvent;
    const OSType reason = [event attributeDescriptorForKeyword:kAEQuitReason].typeCodeValue;
    if (reason == kAELogOut || reason == kAEReallyLogOut || reason == kAEShowRestartDialog || reason == kAERestart ||
        reason == kAEShowShutdownDialog || reason == kAEShutDown) {
        // Logging out, restarting or shutting down: the system waits for an answer, and Cancel
        // would stop it. So the windows close without asking (onClosed for each, the pages go
        // away) and the process ends now, in exit(); the C++ after run() doesn't run.
        gRunning->Quit();
        return NSTerminateNow;
    }
    // Any other Quit (Cmd+Q, the Dock, a script): the main window's onCloseRequested decides
    // (CloseReason::Quit). If it lets it, the main window closes (and every other with it), so
    // run() returns and main() goes on: what an app does after run() (saving, cleaning up)
    // happens. AppKit's own ending (exit()) is declined.
    MacWindow* main = gRunning->Main();
    if (main && main->AskClose(uniapi::CloseReason::Quit)) gRunning->Quit();
    return NSTerminateCancel;
}

// Files to open (a double-click in the Finder on a declared type, Open With, a drop on the Dock
// icon, `open -a`): to the running backend (gNotifying, set by Run(), so also the launch's own
// files, which come while the event loop starts), then App::onOpenFiles. Later, from the loop:
// the handler may open dialogs, which AppKit's event handling shouldn't be inside.
- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    std::vector<std::string> paths;
    for (NSURL* url in urls) {
        if (url.isFileURL && url.path) paths.push_back(uniapi::detail::Utf8(url.path));
    }
    if (paths.empty()) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (uniapi::detail::gNotifying) uniapi::detail::gNotifying->OpenFiles(paths);
    });
}

// The app clicked in the Dock (or opened again) with no window on screen: a hidden main window
// comes back, as Mac apps that keep running without windows do.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)visible {
    MacWindow* main = gRunning ? gRunning->Main() : nullptr;
    if (!visible && main && main->Handle() && !main->IsVisible()) {
        main->SetVisible(true);
        return NO;
    }
    return YES;
}
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    return YES;
}
@end

@implementation UniapiNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))done {
    done(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))done {
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        const std::string identifier = uniapi::detail::Utf8(uniapi::detail::NotificationId(response.notification.request.identifier));
        dispatch_async(dispatch_get_main_queue(), ^{
            if (uniapi::detail::gNotifying) uniapi::detail::gNotifying->NotificationClicked(identifier);
        });
    }
    done();
}
@end

@implementation UniapiAppearanceObserver
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    if (gRunning) gRunning->FollowSystemAppearance();
}
@end

namespace uniapi {

void alert(std::string_view title, std::string_view message) {
    detail::ActivateForDialog();
    NSAlert* alert = [NSAlert new];
    alert.messageText = detail::Ns(std::string(title));
    alert.informativeText = detail::Ns(std::string(message));
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

namespace detail {

namespace {
// /usr/bin/open (Launch Services, as a double-click in the Finder), waited for.
void RunOpen(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "/usr/bin/open");
    std::vector<char*> argv;
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (const int error = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ); error != 0) {
        throw std::runtime_error(std::string("open didn't start: ") + std::strerror(error));
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("open failed (exit status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
    }
}
}  // namespace

void SystemOpen(const std::string& target) { RunOpen({target}); }
void SystemReveal(const std::string& path) { RunOpen({"-R", path}); }

std::unique_ptr<Backend> CreateBackend(const AppOptions& options, BackendEvents events) {
    return std::make_unique<MacBackend>(options, std::move(events));
}

}  // namespace detail
}  // namespace uniapi
