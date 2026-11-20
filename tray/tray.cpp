// The tray's platform-independent part (see uniapi/tray.hpp): its menu, its clicks, and its end
// with the App. Each platform's icon is a Native (native.hpp).

#include "native.hpp"

#include "core/ending.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace uniapi::tray {

namespace {

// Runs fn; an exception escaping it is printed, as uniapi does for handlers.
template <typename F>
void Guarded(const std::string& what, F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uniapi: %s failed: %s\n", what.c_str(), e.what());
    } catch (...) {
        std::fprintf(stderr, "uniapi: %s failed\n", what.c_str());
    }
}

}  // namespace

bool IsPng(const unsigned char* data, size_t size) {
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return data && size >= sizeof kSignature && std::memcmp(data, kSignature, sizeof kSignature) == 0;
}

bool available() { return NativeAvailable(); }

Tray::Tray(App& app, Options options) : impl_(std::make_shared<Impl>()) {
    if (!NativeAvailable()) throw std::runtime_error("this desktop has no tray (no StatusNotifierWatcher on the session bus)");
    if (!IsPng(options.icon.data(), options.icon.size())) throw std::runtime_error("the tray icon must be a PNG");
    Impl& impl = *impl_;
    impl.app = &app;
    impl.owner = this;
    std::weak_ptr<Impl> weak = impl_;
    impl.native = NewNative([weak](int index) {
        const std::shared_ptr<Impl> self = weak.lock();
        return !self || self->Clicked(index);
    });
    if (!impl.native->SetIcon(options.icon.data(), options.icon.size(), options.templateImage)) {
        throw std::runtime_error("the tray icon is a PNG the platform can't read");
    }
    impl.native->SetTooltip(options.tooltip);
    // The App ending takes the icon away (and this Tray does nothing after).
    impl.endingStep = detail::AddEndingStep(app, [weak] {
        if (const std::shared_ptr<Impl> self = weak.lock()) {
            self->endingStep = 0;  // being run: nothing to unregister
            self->Remove();
        }
    });
    // On the UI thread once the event loop runs: onClick is set by then.
    app.post([weak] {
        const std::shared_ptr<Impl> self = weak.lock();
        if (!self || self->removed) return;
        self->native->SetClickOpensMenu(!self->owner->onClick);
        std::string error;
        if (!self->native->Create(error)) std::fprintf(stderr, "uniapi: the tray icon couldn't be shown: %s\n", error.c_str());
    });
}

Tray::~Tray() { remove(); }

void Tray::setMenu(std::vector<Item> items) {
    Impl& impl = *impl_;
    if (impl.removed) return;
    impl.items = std::move(items);
    std::vector<std::string> labels;
    std::vector<int> flags;
    for (const Item& item : impl.items) {
        labels.push_back(item.label);
        flags.push_back(item.isSeparator ? kSeparator : (item.checked ? kChecked : 0) | (item.disabled ? kDisabled : 0));
    }
    impl.native->SetMenu(labels, flags);
}

bool Tray::setIcon(const unsigned char* png, size_t size, bool templateImage) {
    return !impl_->removed && IsPng(png, size) && impl_->native->SetIcon(png, size, templateImage);
}

void Tray::setTooltip(std::string_view text) {
    if (!impl_->removed) impl_->native->SetTooltip(std::string(text));
}

void Tray::remove() { impl_->Remove(); }
bool Tray::removed() const { return impl_->removed; }

void Tray::Impl::Remove() {
    if (removed) return;
    removed = true;
    native->Remove();
    if (app && endingStep) detail::RemoveEndingStep(*app, endingStep);
    endingStep = 0;
    app = nullptr;
}

// From the platform, on the UI thread. index: a menu item, or -1 for the icon (true: onClick took
// it; false: the platform opens the menu).
bool Tray::Impl::Clicked(int index) {
    if (removed) return true;
    if (index < 0) {
        if (!owner->onClick) return false;
        auto handler = owner->onClick;  // a copy: it may replace itself
        Guarded("the tray's onClick", handler);
        return true;
    }
    if (index < int(items.size())) {
        const Item item = items[size_t(index)];  // a copy: its onClick may set the menu again
        if (!item.isSeparator && !item.disabled && item.onClick) Guarded("the tray menu's \"" + item.label + "\"", item.onClick);
    }
    return true;
}

}  // namespace uniapi::tray
