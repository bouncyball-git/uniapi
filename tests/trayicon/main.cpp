// The tray test: icons in the real tray. A Tray refuses an icon that isn't a PNG; an icon appears
// (the platform shows it: macOS's menu bar, Windows' notification area, the Linux host's watcher);
// its menu is what C++ set (macOS, where the test can read it back); choosing an item runs its
// onClick, through the platform where it can be driven (macOS: the menu item's action; Linux: the
// dbusmenu "clicked" event over D-Bus; Windows: the tray's side only, as Windows builds the menu
// when it opens); a separator or a disabled item runs nothing; a menu set again is what counts;
// remove() takes an icon away, and so does the App ending; the C API's tray works too. Exits with
// 0 when every check passes, and when the desktop has no tray (inconclusive: nothing to check).
//
//   uniapi_tray_test          keeps the window and the icon, to try the menu by hand
//   uniapi_tray_test --exit   ends when done, the window hidden throughout (ctest)

#include <uniapi/coro.hpp>
#include <uniapi/tray.h>
#include <uniapi/tray.hpp>
#include <uniapi/uniapi.hpp>

#include "c/c_internal.hpp"  // uniapi_app: the C handle's App
#include "native.hpp"          // the tray's platform side, to look at

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

UNIAPI_ASSETS(trayicon_web);

namespace {

using uniapi::json;
using namespace std::chrono_literals;

struct Checks {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> lines;

    void Check(const std::string& name, bool ok, const std::string& detail = {}) {
        ok ? ++passed : ++failed;
        std::string line = std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok || detail.empty() ? "" : ": " + detail);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        lines.push_back(std::move(line));
    }
};

uniapi::Task<bool> WaitFor(uniapi::App& app, std::function<bool()> done, std::chrono::milliseconds timeout = 10s) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::steady_clock::now() - start > timeout) co_return false;
        co_await app.sleep(20ms);
    }
    co_return true;
}

std::vector<unsigned char> Icon(const char* name) {
    const uniapi::Asset* asset = trayicon_web.find(std::string("icons/") + name);
    if (!asset) return {};
    const uniapi::AssetData data = uniapi::assetData(*asset);
    return std::vector<unsigned char>(data.data, data.data + data.size);
}

std::string Join(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
    return "[" + text + "]";
}

int cClicks = 0;
void CountClick(void*) { ++cClicks; }

}  // namespace

int main() {
    bool keepOpen = true;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--exit") {
            keepOpen = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_tray_test [--exit]\n", argument.c_str());
            return 2;
        }
    }
    if (!uniapi::tray::available()) {
        std::printf("trayicon: this desktop has no tray (no StatusNotifierWatcher): inconclusive, nothing to check\n");
        return 0;
    }

    // Made through the C API, so the C tray can use it too.
    uniapi_options options;
    uniapi_options_init(&options);
    options.title = "uniapi tray test";
    options.width = 640;
    options.height = 380;
    options.activate = keepOpen;
    options.start_hidden = !keepOpen;  // only the tray matters
    uniapi_app* capp = uniapi_app_new(&options);
    uniapi::App* app = &capp->app;
    app->serve(trayicon_web);
    Checks checks;

    bool refused = false;
    try {
        const std::string text = "not a png";
        uniapi::tray::Tray bad(*app, {std::vector<unsigned char>(text.begin(), text.end()), false, ""});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    checks.Check("a Tray refuses an icon that isn't a PNG", refused);

    // The icon, made before run() (it appears once run() runs), and its menu. On the heap and
    // left for the App's end to take away (checked after run()).
    auto first = std::make_unique<uniapi::tray::Tray>(*app, uniapi::tray::Options{Icon("circle-black-36.png"), true, "uniapi tray test"});
    std::vector<std::string> ran;
    auto run = [&ran](std::string name) { return [&ran, name] { ran.push_back(name); }; };
    first->setMenu({
        {"First", run("first")},
        uniapi::tray::Item::separator(),
        {"Checked", run("checked"), true},
        {"Disabled", run("disabled"), false, true},
    });
    uniapi::tray::Native* native = first->internal()->native.get();

    // The C API's tray: made, its menu chosen, freed.
    uniapi_tray* cTray = nullptr;
    bool started = false;

    auto flow = [&]() -> uniapi::Task<> {
        uniapi::App& a = *app;
        const bool placed = co_await WaitFor(a, [&] { return native->TestPlaced(); });
        checks.Check("the icon appears in the tray (made before run())", placed);

        // The menu as the platform has it, where the test can read it back.
        std::string labels[4];
        int flags[4] = {};
        bool readable = true;
        for (int i = 0; i < 4; ++i) readable = native->TestItem(i, labels[i], flags[i]) && readable;
        if (readable) {
            checks.Check("its menu is what C++ set: labels, a separator, a check mark, a disabled item",
                         labels[0] == "First" && flags[0] == 0 && flags[1] == uniapi::tray::kSeparator && labels[2] == "Checked"
                             && flags[2] == uniapi::tray::kChecked && labels[3] == "Disabled" && flags[3] == uniapi::tray::kDisabled);
        }

        // Choosing items: through the platform where it can be driven.
        native->TestChoose(0);
        native->TestChoose(1);  // the separator
        native->TestChoose(3);  // disabled
        native->TestChoose(2);
        const bool chosen = co_await WaitFor(a, [&] { return ran.size() >= 2; });
        co_await a.sleep(200ms);  // anything else would have arrived by now
        checks.Check("choosing an item runs its onClick; a separator or a disabled item runs nothing",
                     chosen && ran == std::vector<std::string>{"first", "checked"}, Join(ran));

        // The menu set again: what counts is the new one.
        ran.clear();
        first->setMenu({{"Renamed", run("renamed")}});
        std::string label;
        int flag = 0;
        const bool canRead = native->TestItem(0, label, flag);
        native->TestChoose(0);
        const bool again = co_await WaitFor(a, [&] { return ran.size() == 1; });
        checks.Check("a menu set again replaces the old one", again && ran[0] == "renamed" && (!canRead || label == "Renamed"),
                     Join(ran) + " " + label);

        // A second icon, then remove(): it leaves the tray.
        try {
            uniapi::tray::Tray second(a, {Icon("circle-blue-32.png"), false, "second"});
            uniapi::tray::Native* secondNative = second.internal()->native.get();
            const bool appeared = co_await WaitFor(a, [&] { return secondNative->TestPlaced(); });
            second.remove();
            const bool gone = !secondNative->TestPlaced() || co_await WaitFor(a, [&] { return !secondNative->TestPlaced(); });
            checks.Check("a second icon appears; remove() takes it away", appeared && gone && second.removed(),
                         std::string(appeared ? "appeared" : "never appeared") + ", " + (gone ? "gone" : "still there"));
        } catch (const std::exception& e) {
            checks.Check("a second icon", false, e.what());
        }

        // The C API.
        const std::vector<unsigned char> png = Icon("circle-blue-32.png");
        cTray = uniapi_tray_new(capp, png.data(), png.size(), 0, "C tray");
        if (cTray) {
            const uniapi_tray_item items[] = {{"Count", 0, CountClick, nullptr}, {nullptr, UNIAPI_TRAY_SEPARATOR, nullptr, nullptr}};
            uniapi_tray_set_menu(cTray, items, 2);
            uniapi::tray::Native* cNative = cTray->tray->internal()->native.get();
            const bool cPlaced = co_await WaitFor(a, [&] { return cNative->TestPlaced(); });
            cNative->TestChoose(0);
            const bool counted = co_await WaitFor(a, [&] { return cClicks == 1; });
            uniapi_tray_free(cTray);
            cTray = nullptr;
            checks.Check("the C API: uniapi_tray_new, _set_menu (an item's callback runs), _free", cPlaced && counted,
                         std::string(cPlaced ? "placed" : "not placed") + ", clicks " + std::to_string(cClicks));
        } else {
            checks.Check("the C API: uniapi_tray_new", false, uniapi_last_error());
        }
        const std::string text = "not a png";
        checks.Check("uniapi_tray_new refuses an icon that isn't a PNG, saying why",
                     !uniapi_tray_new(capp, reinterpret_cast<const unsigned char*>(text.data()), text.size(), 0, nullptr)
                         && std::string(uniapi_last_error()).find("PNG") != std::string::npos,
                     uniapi_last_error());

        std::printf("\ntrayicon: %d passed, %d failed (and one after the app ends)\n", checks.passed, checks.failed);
        std::fflush(stdout);
        if (!keepOpen) {
            a.quit();
            co_return;
        }
        a.call("showResults", json::array({checks.lines}));
    };

    app->onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();
    };
    int ticks = 0;
    app->every(60s, [&] {
        if (ticks++ == 0 || keepOpen) return;
        checks.Check("finished within 60 s", false);
        app->quit();
    });
    app->navigate("/index.html");
    app->run();
    uniapi_app_free(capp);  // the App ends: its end takes the first icon away
    app = nullptr;
    checks.Check("the App ending takes the icon away (the Tray, still there, does nothing after)",
                 first->removed() && !native->TestPlaced());
    first->setMenu({{"after the end", {}}});  // no effect, no harm
    first.reset();
    std::printf("\ntrayicon: %d passed, %d failed\n", checks.passed, checks.failed);
    return checks.failed == 0 && started ? 0 : 1;
}
