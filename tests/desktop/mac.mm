// macOS side of the desktop test: a drop through the web view's test hook (a drag can't be made
// here), and the clipboard as other programs see it (pbcopy, pbpaste). Objective-C++ so main.cpp
// stays C++.

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <string>
#include <vector>

// As if the files were dropped on the window with this title at (x, y): uniapi's web view
// reports them as from a real drop. False if there is no such window.
bool Drop(const std::string& title, const std::vector<std::string>& paths, int x, int y) {
    NSString* wanted = [NSString stringWithUTF8String:title.c_str()];
    for (NSWindow* window in NSApp.windows) {
        if (![window.title isEqualToString:wanted]) continue;
        NSView* view = window.contentView;
        SEL hook = NSSelectorFromString(@"uniapiTestDropPaths:x:y:");
        if (![view respondsToSelector:hook]) return false;
        NSMutableArray<NSString*>* list = [NSMutableArray array];
        for (const std::string& path : paths) [list addObject:[NSString stringWithUTF8String:path.c_str()]];
        using Hook = void (*)(id, SEL, NSArray<NSString*>*, NSInteger, NSInteger);
        reinterpret_cast<Hook>([view methodForSelector:hook])(view, hook, list, x, y);
        return true;
    }
    return false;
}

bool HasSystemClipboard() { return true; }

bool SystemClipboardGet(std::string& text) {
    FILE* pipe = popen("/usr/bin/pbpaste", "r");
    if (!pipe) return false;
    text.clear();
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) text.append(chunk, n);
    return pclose(pipe) == 0;
}

bool SystemClipboardSet(const std::string& text) {
    FILE* pipe = popen("/usr/bin/pbcopy", "w");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
}
