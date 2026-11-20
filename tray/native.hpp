#pragma once

// A platform's tray icon (native_linux.cpp, native_windows.cpp, native_macos.mm), and the Tray's
// internals. Not installed: tray.cpp and uniapi's own tests (tests/trayicon) use it.

#include <uniapi/tray.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uniapi::tray {

// Menu item flags, as every platform takes them.
enum : int { kSeparator = 1, kChecked = 2, kDisabled = 4 };

// One tray icon on the platform: settings kept until Create, applied from then on. Used on the UI
// thread. clicked (given to NewNative) gets a menu item's index, or -1 for a click on the icon,
// and answers whether it took it (false: the platform opens the menu).
class Native {
public:
    virtual ~Native() = default;
    virtual bool Create(std::string& error) = 0;  // shows it, with what was set so far
    virtual bool SetIcon(const unsigned char* png, size_t size, bool templateImage) = 0;
    virtual void SetTooltip(const std::string& text) = 0;
    virtual void SetClickOpensMenu(bool on) = 0;  // Linux: hosts read it when the item appears
    virtual void SetMenu(const std::vector<std::string>& labels, const std::vector<int>& flags) = 0;
    virtual void Remove() = 0;  // for good

    // For tests. Placed: the platform shows it (macOS: in the menu bar; Windows: Windows knows
    // where it is; Linux: the host's watcher lists it). Choose: as if the person chose that menu
    // item (through the platform where it can: on Linux over D-Bus, arriving later). Item: the
    // menu item as the platform shows it; false where it can't tell.
    virtual bool TestPlaced() = 0;
    virtual void TestChoose(int index) = 0;
    virtual bool TestItem(int index, std::string& label, int& flags) {
        (void)index;
        (void)label;
        (void)flags;
        return false;
    }
};

bool NativeAvailable();
std::unique_ptr<Native> NewNative(std::function<bool(int index)> clicked);

bool IsPng(const unsigned char* data, size_t size);

struct Tray::Impl {
    App* app = nullptr;  // nullptr once removed or the App has ended
    Tray* owner = nullptr;
    std::unique_ptr<Native> native;
    std::vector<Item> items;
    bool removed = false;
    int endingStep = 0;

    bool Clicked(int index);
    void Remove();
};

}  // namespace uniapi::tray

// The C API's handle (tray_c.cpp).
struct uniapi_tray {
    std::unique_ptr<uniapi::tray::Tray> tray;
};
