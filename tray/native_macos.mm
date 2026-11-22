// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The macOS tray (Objective-C++, ARC): an NSStatusItem in the menu bar, with an image, a tooltip
// and a menu. Everything here runs on the main thread.

#include "native.hpp"

#import <Cocoa/Cocoa.h>

#include <cmath>
#include <string>
#include <vector>

#if !__has_feature(objc_arc)
#error "native_macos.mm needs ARC (-fobjc-arc)"
#endif

namespace uniapi::tray {
class MacNative;
}

// The status item's target: clicks on the icon and choices in the menu.
@interface UniapiTrayTarget : NSObject
@property(nonatomic, assign) uniapi::tray::MacNative* host;
@end

namespace uniapi::tray {

namespace {
const CGFloat kIconHeight = 18;  // the icon's height in the menu bar, in points: Apple's guideline
}  // namespace

class MacNative final : public Native {
public:
    explicit MacNative(std::function<bool(int)> clicked) : clicked_(std::move(clicked)) {
        [NSApplication sharedApplication];
        target_ = [UniapiTrayTarget new];
        target_.host = this;
        menu_ = [NSMenu new];
        menu_.autoenablesItems = NO;  // items are enabled as the app says
    }
    ~MacNative() override {
        Remove();
        target_.host = nullptr;
    }

    bool Create(std::string&) override {
        if (item_ || removed_) return true;
        item_ = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
        NSStatusBarButton* button = item_.button;
        button.target = target_;
        button.action = @selector(clicked:);
        [button sendActionOn:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown];
        ApplyIcon();
        button.toolTip = tooltip_.empty() ? nil : [NSString stringWithUTF8String:tooltip_.c_str()];
        return true;
    }

    // Its pixels drawn at the menu bar's icon height, the width in proportion: a 36-pixel image is
    // sharp on Retina screens. A template is drawn by its shape (alpha) alone, in the menu bar's
    // colour: dark on a light menu bar, light on a dark one, as the system's icons.
    bool SetIcon(const unsigned char* png, size_t size, bool templateImage) override {
        NSImage* image = [[NSImage alloc] initWithData:[NSData dataWithBytes:png length:size]];
        if (!image || !image.isValid || image.size.height <= 0) return false;
        image.size = NSMakeSize(std::round(kIconHeight * image.size.width / image.size.height), kIconHeight);
        [image setTemplate:templateImage];  // not image.template: "template" is a C++ keyword in Objective-C++
        image_ = image;
        ApplyIcon();
        return true;
    }

    void SetTooltip(const std::string& text) override {
        tooltip_ = text;
        if (item_) item_.button.toolTip = text.empty() ? nil : [NSString stringWithUTF8String:text.c_str()];
    }

    void SetClickOpensMenu(bool) override {}  // asked of the Tray at each click

    void SetMenu(const std::vector<std::string>& labels, const std::vector<int>& flags) override {
        [menu_ removeAllItems];
        for (size_t i = 0; i < labels.size(); ++i) {
            if (flags[i] & kSeparator) {
                [menu_ addItem:[NSMenuItem separatorItem]];
                continue;
            }
            NSString* label = [NSString stringWithUTF8String:labels[i].c_str()] ?: @"";
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:label action:@selector(chosen:) keyEquivalent:@""];
            item.target = target_;
            item.tag = NSInteger(i);
            item.state = (flags[i] & kChecked) ? NSControlStateValueOn : NSControlStateValueOff;
            item.enabled = (flags[i] & kDisabled) == 0;
            [menu_ addItem:item];
        }
    }

    void Remove() override {
        removed_ = true;
        if (!item_) return;
        item_.button.target = nil;
        [NSStatusBar.systemStatusBar removeStatusItem:item_];
        item_ = nil;
    }

    bool TestPlaced() override { return item_ && item_.button.window != nil; }

    bool TestItem(int index, std::string& label, int& flags) override {
        if (index < 0 || index >= menu_.numberOfItems) return false;
        NSMenuItem* item = [menu_ itemAtIndex:index];
        flags = 0;
        label.clear();
        if (item.isSeparatorItem) {
            flags = kSeparator;
            return true;
        }
        label = item.title.UTF8String ?: "";
        if (item.state == NSControlStateValueOn) flags |= kChecked;
        if (!item.enabled) flags |= kDisabled;
        return true;
    }

    void TestChoose(int index) override {
        if (index < 0 || index >= menu_.numberOfItems) return;
        NSMenuItem* item = [menu_ itemAtIndex:index];
        if (item.action) [NSApp sendAction:item.action to:item.target from:item];
    }

    // A click on the icon: the Tray's onClick for a plain (left) click, if it has one; the menu
    // otherwise, and always for a right click or Control-click.
    void Clicked() {
        NSEvent* event = NSApp.currentEvent;
        const bool secondary = event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeRightMouseUp ||
                               (event.modifierFlags & NSEventModifierFlagControl) != 0;
        if (!secondary && clicked_(-1)) return;
        if (menu_.numberOfItems == 0 || !item_) return;
        // The status item opens its menu itself, placed and highlighted as the system's are, when it
        // has one: given it for this click only, so the next click comes here again.
        item_.menu = menu_;
        [item_.button performClick:nil];
        item_.menu = nil;
    }

    void Chosen(int index) { clicked_(index); }

private:
    void ApplyIcon() {
        if (item_ && image_) item_.button.image = image_;
    }

    std::function<bool(int)> clicked_;
    UniapiTrayTarget* target_ = nil;
    NSStatusItem* item_ = nil;
    NSMenu* menu_ = nil;
    NSImage* image_ = nil;
    std::string tooltip_;
    bool removed_ = false;
};

bool NativeAvailable() { return true; }

std::unique_ptr<Native> NewNative(std::function<bool(int)> clicked) { return std::make_unique<MacNative>(std::move(clicked)); }

}  // namespace uniapi::tray

@implementation UniapiTrayTarget
- (void)clicked:(id)sender {
    if (uniapi::tray::MacNative* host = self.host) host->Clicked();
}
- (void)chosen:(NSMenuItem*)sender {
    if (uniapi::tray::MacNative* host = self.host) host->Chosen(int(sender.tag));
}
@end
