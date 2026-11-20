// macOS helpers for the windows test: what Win32 calls do there (IsWindowEnabled, WM_CLOSE),
// on uniapi's NSWindows, found by title. Objective-C++ so main.cpp stays C++.

#import <Cocoa/Cocoa.h>

#include <string>

namespace {

NSWindow* FindWindow(const std::string& title) {
    NSString* wanted = [NSString stringWithUTF8String:title.c_str()];
    for (NSWindow* window in NSApp.windows) {
        if ([window.title isEqualToString:wanted]) return window;
    }
    return nil;
}

}  // namespace

// 1: the window can take input (it may become the key window), 0: it can't (a modal window holds
// it), -1: no window with that title.
int MacWindowTakesInput(const std::string& title) {
    NSWindow* window = FindWindow(title);
    if (!window) return -1;
    return window.canBecomeKeyWindow ? 1 : 0;
}

// As a click on the window's close button. False if there is no such window.
bool MacCloseAsUser(const std::string& title) {
    NSWindow* window = FindWindow(title);
    if (!window) return false;
    [window performClose:nil];
    return true;
}
