// uniapi checker: the checks that need a person at the screen, one button each. Each result
// is shown on the page. Not run by ctest.
//
//   uniapi_checker                 alert, dialogs, window options, icon, full screen
//   uniapi_checker --dialog-first  an alert before run() first: on Linux the app must then
//                                  start with shared memory off (and not crash)

#include <uniapi/uniapi.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

UNIAPI_ASSETS(checker_web);

namespace {

// The system's temporary folder, where the dialogs open (UTF-8).
std::string TempFolder() {
    std::error_code error;
    const auto u8 = std::filesystem::temp_directory_path(error).u8string();
    return std::string(u8.begin(), u8.end());
}

}  // namespace

int main() {
    bool dialogFirst = false;
    for (const std::string& argument : uniapi::arguments()) {
        if (argument == "--dialog-first") dialogFirst = true;
    }
    if (dialogFirst) {
        uniapi::alert("uniapi checker", "This alert ran before run(). Close it; the app then starts "
                                        "(on Linux expect shared memory OFF on the page, and no crash).");
    }

    uniapi::AppOptions options;
    options.title = "uniapi checker";
    options.width = 760;
    options.height = 560;
    options.minWidth = 500;  // the page asks you to shrink the window below this
    options.minHeight = 400;
    options.devTools = true;  // the "Developer tools" button; F12 / "Inspect Element" too
    uniapi::App app(options);
    app.serve(checker_web);

    // Dialogs run their own event loop: open them from post(), outside the web view's event.
    auto openWith = [&app](bool multiple, bool folder) {
        return [&app, multiple, folder](const uniapi::json&, const uniapi::Reply& reply) {
            app.post([&app, multiple, folder, reply] {
                uniapi::OpenDialogOptions o;
                o.title = folder ? "Pick folder(s)" : "Pick file(s)";
                // Opens in the temporary folder, with three file types to choose from.
                o.filterName = "Text or images";
                o.filterPattern = "*.txt;*.png;*.jpg";
                o.filters = {{"C++ files", "*.cpp;*.hpp"}, {"All files", "*"}};
                o.directory = TempFolder();
                o.multiple = multiple;
                o.folder = folder;
                reply.resolve(app.openFileDialog(o));
            });
        };
    };
    app.expose("openOne", openWith(false, false));
    app.expose("openSeveral", openWith(true, false));
    app.expose("openFolder", openWith(false, true));
    app.expose("openFolders", openWith(true, true));
    app.expose("save", [&app](const uniapi::json&, const uniapi::Reply& reply) {
        app.post([&app, reply] {
            uniapi::SaveDialogOptions o;
            o.defaultName = "results.csv";
            o.filterName = "CSV file";
            o.filterPattern = "*.csv";
            o.filters = {{"Text file", "*.txt"}};
            o.directory = TempFolder();
            const auto path = app.saveFileDialog(o);
            reply.resolve(path ? uniapi::json(*path) : uniapi::json(nullptr));
        });
    });
    app.expose("alert", [&app](const uniapi::json&, const uniapi::Reply& reply) {
        app.post([reply] {
            uniapi::alert("uniapi checker", "uniapi::alert from C++. Close it: the page then says \"closed\".");
            reply.resolve("closed");
        });
    });
    app.expose("toggleFullscreen", [&app] {
        app.setFullscreen(!app.fullscreen());
        return app.fullscreen();
    });
    // The title bar: the system's light or dark, then forced dark, then forced light.
    app.expose("cycleTitleBar", [&app] {
        const uniapi::TitleBar next = app.titleBar() == uniapi::TitleBar::System ? uniapi::TitleBar::Dark
                                    : app.titleBar() == uniapi::TitleBar::Dark   ? uniapi::TitleBar::Light
                                                                                 : uniapi::TitleBar::System;
        app.setTitleBar(next);
        return std::string(next == uniapi::TitleBar::Dark ? "dark" : next == uniapi::TitleBar::Light ? "light" : "system");
    });
    // Picks a PNG with the Open dialog (filtered to *.png) and makes it the window icon.
    app.expose("pickIcon", [&app](const uniapi::json&, const uniapi::Reply& reply) {
        app.post([&app, reply] {
            uniapi::OpenDialogOptions o;
            o.title = "Pick a PNG for the window icon";
            o.filterName = "PNG images";
            o.filterPattern = "*.png";
            const std::vector<std::string> picked = app.openFileDialog(o);
            if (picked.empty()) {
                reply.resolve({{"cancelled", true}});
                return;
            }
            std::ifstream in(picked[0], std::ios::binary);
            std::vector<unsigned char> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            reply.resolve({{"path", picked[0]}, {"bytes", png.size()}, {"setIcon", app.setIcon(png.data(), png.size())}});
        });
    });
    // Opens the engine's developer tools (Linux, Windows); macOS: Safari's Develop menu.
    app.expose("devTools", [&app] {
        app.openDevTools();
#ifdef __APPLE__
        return std::string("macOS opens none: Safari > Develop lists this page");
#else
        return std::string("opened in a window of their own");
#endif
    });
    // A further window that stops growing at 640 x 480 and has a dark background, which shows
    // through its page (limits.html sets none).
    app.expose("limits", [&app] {
        uniapi::WindowOptions o;
        o.title = "uniapi checker: limits and background";
        o.width = 520;
        o.height = 360;
        o.maxWidth = 640;
        o.maxHeight = 480;
        o.backgroundColor = "#1b2636";
        app.openWindow(o).navigate("/limits.html");
        return std::string("opened: grow it past 640 x 480");
    });
    app.expose("sharedMemory", [&app] { return app.sharedMemory(); });

    app.navigate("/index.html");
    return app.run();
}
