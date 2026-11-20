// macOS helpers for the lifecycle test: uniapi's NSWindows, found by title. Objective-C++ so
// main.cpp stays C++. Called on the main thread.

#import <Cocoa/Cocoa.h>

#include <string>

namespace {

NSWindow* Find(const std::string& title) {
    NSString* wanted = [NSString stringWithUTF8String:title.c_str()];
    for (NSWindow* window in NSApp.windows) {
        if ([window.title isEqualToString:wanted]) return window;
    }
    return nil;
}

}  // namespace

// Whether the window is on screen (ordered in; minimised counts): 1, 0, or -1 if there is none.
int OnScreen(const std::string& title) {
    NSWindow* window = Find(title);
    if (!window) return -1;
    return window.isVisible || window.isMiniaturized ? 1 : 0;
}

// As a click on its close button (performClose:), which asks onCloseRequested.
bool CloseAsUser(const std::string& title) {
    NSWindow* window = Find(title);
    if (!window) return false;
    [window performClose:nil];
    return true;
}

// As Cmd+Q (-terminate:): the main window's onCloseRequested is asked with CloseReason::Quit.
// Soon, from the main queue, not inside the caller.
bool QuitAsUser() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
    return true;
}

// macOS opens no developer tools from the program (Safari's Develop menu inspects the page).
int DevToolsWindows(bool) { return -1; }

// 1: the window can take input (it may become the key window), 0: a modal window holds it, -1:
// no such window.
int TakesInput(const std::string& title) {
    NSWindow* window = Find(title);
    if (!window) return -1;
    return window.canBecomeKeyWindow ? 1 : 0;
}
