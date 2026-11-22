// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// The desktop test: what crosses between the app and the rest of the desktop. The clipboard both
// ways (text the app puts there is what other programs paste, and theirs is what the app reads),
// a link and a new window to another site going outside (onExternalURL, which keeps them from
// opening here) while the page stays, a file: navigation refused, files dropped on the window
// reaching onFileDrop (macOS, through a test hook), and a second launch handing its arguments to
// the running app (singleInstance; the test starts itself again). Exits with 0 when every check
// passes. The clipboard's text is put back at the end.
//
//   uniapi_desktop_test          keeps the window open with the results, to try by hand
//   uniapi_desktop_test --exit   ends when done, without taking focus (ctest)

#include <uniapi/coro.hpp>
#include <uniapi/uniapi.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

UNIAPI_ASSETS(desktop_web);

// Inside the library (src/core/desktop.cpp): how a dialog's file types and starting folder are
// read from its options. The dialogs themselves are tried by hand (the checker).
namespace uniapi::detail {
std::vector<FileFilter> DialogFilters(const std::string& name, const std::string& pattern,
                                      const std::vector<FileFilter>& more);
bool IsFolder(const std::string& path);
std::string WebKitVersion(const std::string& bundleVersion);
std::string RuntimeVersion(const std::string& browserVersion);
int ColorValue(const std::string& color);
}  // namespace uniapi::detail

// The platform's side (desktop/linux.cpp, desktop/mac.mm; Windows below). Drop: as if these files
// were dropped on the window with this title at (x, y), false where the test can't (a drag can't
// be made). The system clipboard as other programs see it, from a background thread: false when
// there is no tool for it.
bool Drop(const std::string& title, const std::vector<std::string>& paths, int x, int y);
bool SystemClipboardGet(std::string& text);
bool SystemClipboardSet(const std::string& text);
bool HasSystemClipboard();

namespace {

using uniapi::json;
using namespace std::chrono_literals;

constexpr char kTitle[] = "uniapi desktop test";

#ifdef _WIN32
std::wstring Wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    return wide;
}

// A command line argument as CommandLineToArgvW reads it back.
std::wstring Quote(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += c;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L"\"";
}
#endif

// Runs a program and waits for it: its exit code, or -1.
int Run(const std::vector<std::string>& argv) {
#ifdef _WIN32
    std::wstring line;
    for (const std::string& argument : argv) line += (line.empty() ? L"" : L" ") + Quote(Wide(argument));
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return int(code);
#else
    std::vector<char*> args;
    for (const std::string& argument : argv) args.push_back(const_cast<char*>(argument.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

struct Checks {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> lines;

    void Check(const std::string& name, bool ok, const std::string& detail = {}) {
        ok ? ++passed : ++failed;
        std::string line = std::string(ok ? "  pass  " : "  FAIL  ") + name + (ok || detail.empty() ? "" : ": " + detail);
        Print(std::move(line));
    }
    void NotApplicable(const std::string& name, const std::string& why) { Print("  n/a   " + name + " (" + why + ")"); }
    void Print(std::string line) {
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

uniapi::Task<bool> Call(uniapi::App& app, std::string function, json args) {
    uniapi::Result result = co_await app.callAsync(std::move(function), std::move(args));
    co_return result.ok;
}

// A path as UTF-8 (C++20's u8string is std::u8string).
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
std::string GenericUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string Join(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : " | ") + item;
    return text;
}

}  // namespace

#ifdef _WIN32
bool Drop(const std::string&, const std::vector<std::string>&, int, int) { return false; }  // WebView2's own window

// PowerShell: the clipboard as other programs see it.
bool HasSystemClipboard() { return true; }
bool SystemClipboardGet(std::string& text) {
    FILE* pipe = _popen("powershell -NoProfile -Command \"[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-Clipboard -Raw\"", "rb");
    if (!pipe) return false;
    text.clear();
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) text.append(chunk, n);
    const bool ok = _pclose(pipe) == 0;
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0) text.resize(text.size() - 2);
    return ok;
}
// The program's details, read back from its own .exe (uniapi_icon in tests/CMakeLists.txt): its
// version as numbers, then each text, "key=value" separated by " | ".
std::string OwnDetails() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return "no path";
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return "no version resource";
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return "unreadable";
    std::string text;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) && fixed) {
        text = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "."
               + std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
    }
    for (const char* name : {"ProductName", "FileDescription", "FileVersion", "ProductVersion", "CompanyName",
                             "LegalCopyright", "Comments"}) {
        const std::wstring key(name, name + std::strlen(name));  // ASCII
        wchar_t* value = nullptr;
        const std::wstring query = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
        if (!VerQueryValueW(data.data(), query.c_str(), reinterpret_cast<void**>(&value), &length) || !value) continue;
        const std::wstring wide(value, wcsnlen(value, length));
        std::string utf8(WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
        text += std::string(" | ") + name + "=" + utf8;
    }
    return text;
}

bool SystemClipboardSet(const std::string& text) {
    FILE* pipe = _popen("powershell -NoProfile -Command \"[Console]::InputEncoding=[Text.Encoding]::UTF8; $input | Set-Clipboard\"", "wb");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return _pclose(pipe) == 0;
}
#endif

int main() {
    std::vector<std::string> arguments = uniapi::arguments();
    // The second launch: the same singleInstance hands this launch over and ends in App's constructor.
    if (arguments.size() >= 2 && arguments[0] == "--second-launch") {
        uniapi::AppOptions options;
        options.singleInstance = arguments[1];
        uniapi::App app(options);
        std::printf("desktop: the second launch wasn't handed over\n");
        return 3;
    }
    bool keepOpen = true;
    for (const std::string& argument : arguments) {
        if (argument == "--exit") {
            keepOpen = false;
        } else {
            std::printf("unknown option %s\nusage: uniapi_desktop_test [--exit]\n", argument.c_str());
            return 2;
        }
    }

    const std::string instanceId = "org.uniapi.tests.desktop." + std::to_string(std::time(nullptr)) + "."
                                   + std::to_string(std::rand());
    uniapi::AppOptions options;
    options.title = kTitle;
    options.width = 640;
    options.height = 420;
    options.activate = keepOpen;
    options.singleInstance = instanceId;
    uniapi::App app(options);
    app.serve(desktop_web);
    Checks checks;

    std::vector<std::string> external;
    std::vector<std::vector<std::string>> launchArgs;
    std::vector<std::string> launchDirs;
    int disconnects = 0;
    std::vector<std::string> dropped;
    int dropX = -1, dropY = -1;
    bool started = false;
    app.onExternalURL = [&](const std::string& url) {
        external.push_back(url);
        return false;  // the test keeps them from opening
    };
    app.onDisconnect = [&](const std::string&) { ++disconnects; };
    app.onFileDrop = [&](const std::vector<std::string>& paths, int x, int y) {
        dropped = paths;
        dropX = x;
        dropY = y;
    };
    app.onSecondInstance = [&](const std::vector<std::string>& args, const std::string& dir) {
        launchArgs.push_back(args);
        launchDirs.push_back(dir);
    };
    // By hand (the page's button): the clipboard set after a click in the window.
    app.expose("copyByHand", [&app] {
        char when[16];
        const std::time_t now = std::time(nullptr);
        std::strftime(when, sizeof when, "%H:%M:%S", std::localtime(&now));
        const std::string text = std::string("uniapi: copied by a click at ") + when;
        app.setClipboardText(text);
        std::printf("copyByHand: %s\n", text.c_str());
        return text;
    });

    auto flow = [&]() -> uniapi::Task<> {
        // 1. The clipboard, both ways, as other programs see it.
        const std::optional<std::string> original = co_await app.clipboardTextAsync();
        const std::string text = "uniapi desktop test: \xc3\xbc" "n" "\xc3\xaf" "c" "\xc3\xb6" "d" "\xc3\xa9 \xe2\x9c\x93 1\nsecond line";
        // The clipboard is shared: a person copying, or a program that watches it, may change it
        // between a check's write and its read. Each round trip is tried again (a few times, a
        // moment apart) when what came back isn't what was put there; a check fails only when no
        // try comes back right, so a real fault still shows while a copy made meanwhile doesn't.
        constexpr int kTries = 4;
        bool taken = false;
        std::optional<std::string> back;
        for (int attempt = 0; attempt < kTries; ++attempt) {
            taken = app.setClipboardText(text);
            back = co_await app.clipboardTextAsync();
            if (taken && back && *back == text) break;
            co_await app.sleep(250ms);
        }
        // Wayland takes a clipboard only from an app the person has just used (a click, a key: it
        // wants that input's serial). This window never had any: what it sets may not hold, here
        // or for other programs. Those checks are left out then, not failed.
#ifdef __linux__
        const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
        const char* gdkBackend = std::getenv("GDK_BACKEND");
        const bool wayland = waylandDisplay && *waylandDisplay && !(gdkBackend && std::string(gdkBackend).rfind("x11", 0) == 0);
#else
        const bool wayland = false;
#endif
        const char* waylandWhy = "Wayland takes the clipboard only from a window the person has used";
        if (wayland && !(back && *back == text)) {
            checks.NotApplicable("setClipboardText, then clipboardText", waylandWhy);
        } else {
            checks.Check("setClipboardText, then clipboardText: the same text (unicode, two lines)",
                         taken && back && *back == text, back ? *back : "nothing");
        }
        if (HasSystemClipboard()) {
            std::string seen;
            bool got = false;
            for (int attempt = 0; attempt < kTries; ++attempt) {
                if (attempt > 0) app.setClipboardText(text);  // put it back: someone copied meanwhile
                co_await uniapi::switchToBackground();  // the tools talk to this app's UI thread
                got = SystemClipboardGet(seen);
                co_await app.switchToUi();
                if (got && seen == text) break;
                co_await app.sleep(250ms);
            }
            const std::string theirs = "copied by another program \xe2\x9c\x93";
            bool set = false;
            if (wayland && !(got && seen == text)) {
                checks.NotApplicable("from the app: other programs paste what setClipboardText put there", waylandWhy);
            } else {
                checks.Check("from the app: other programs paste what setClipboardText put there", got && seen == text,
                             seen);
            }
            // The desktop tells the app about another program's clipboard a moment later; another
            // copy meanwhile replaces theirs, so then it is copied again.
            std::optional<std::string> read;
            for (int attempt = 0; attempt < kTries && !(read && *read == theirs); ++attempt) {
                co_await uniapi::switchToBackground();
                set = SystemClipboardSet(theirs);
                co_await app.switchToUi();
                for (int tries = 0; tries < 40; ++tries) {
                    read = co_await app.clipboardTextAsync();
                    if (read && *read == theirs) break;
                    if (read && *read != text && *read != theirs) break;  // someone else's: copy again
                    co_await app.sleep(50ms);
                }
            }
            checks.Check("to the app: clipboardText reads what another program copied", set && read && *read == theirs,
                         read ? *read : "nothing");
        } else {
            checks.NotApplicable("the clipboard as other programs see it", "no wl-copy and wl-paste, or xclip on X11");
        }
        if (original) app.setClipboardText(*original);  // put back

        // Notifications: an empty one is refused (nothing is shown on screen by the test).
        const uniapi::Result empty = co_await app.notifyAsync({});
        const std::string named = app.notify({"", "", "mine"}, {});
        checks.Check("notify refuses a notification with neither title nor body; it names one it isn't given an id for",
                     !empty.ok && empty.error.find("title or a body") != std::string::npos && named == "mine",
                     empty.error);

        // 2. Leaving the app: a link, a new window, a file. Opening things refuses bad input (and
        // nothing is opened here).
        auto refuses = [](const std::function<void()>& open, const std::string& expected) {
            try {
                open();
            } catch (const std::runtime_error& e) {
                return std::string(e.what()).find(expected) != std::string::npos;
            }
            return false;
        };
        checks.Check("openURL, openPath and revealPath refuse what they can't open (std::runtime_error, saying why)",
                     refuses([] { uniapi::openURL("not a url"); }, "scheme")
                         && refuses([] { uniapi::openPath("no such folder/no such file.txt"); }, "no such")
                         && refuses([] { uniapi::revealPath(" "); }, "no path"));

        // The file dialogs' types: filterName/filterPattern's first, then filters; one without a
        // pattern left out, line breaks and edge spaces gone. And the folder they open in.
        {
            using uniapi::FileFilter;
            using List = std::vector<FileFilter>;
            auto same = [](const List& a, const List& b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (a[i].name != b[i].name || a[i].pattern != b[i].pattern) return false;
                }
                return true;
            };
            using uniapi::detail::DialogFilters;
            const bool filtersOk =
                DialogFilters("", "", {}).empty() && same(DialogFilters("CSV file", "*.csv", {}), {{"CSV file", "*.csv"}})
                && same(DialogFilters("", "*.csv", {}), {{"", "*.csv"}}) && DialogFilters("A name alone", "", {}).empty()
                && same(DialogFilters("", "", {{"YAML", "*.yml;*.yaml"}, {"All files", "*"}}),
                        {{"YAML", "*.yml;*.yaml"}, {"All files", "*"}})
                && same(DialogFilters("JSON", "*.json", {{"No pattern", ""}, {"All files", "*"}}),
                        {{"JSON", "*.json"}, {"All files", "*"}})
                && same(DialogFilters("Two\nlines", " *.a\r\n", {}), {{"Two lines", "*.a"}});
            std::error_code error;
            const auto here = std::filesystem::current_path(error).u8string();
            const std::string folder(here.begin(), here.end());
            const bool folderOk = uniapi::detail::IsFolder(folder) && !uniapi::detail::IsFolder("")
                                  && !uniapi::detail::IsFolder(folder + "/no such folder");
            checks.Check("the file dialogs' types (filterName/filterPattern first, then filters) and folder (one that is there)",
                         filtersOk && folderOk, std::string("types ") + (filtersOk ? "ok" : "wrong") + ", folder "
                                                    + (folderOk ? "ok" : "wrong"));
        }

        // The web engine's version as each platform gives it: macOS's WebKit bundle version
        // (the generation first) and WebView2's browser version (a preview channel after it).
        {
            std::string wrong;
            for (const auto& [given, expected] : std::vector<std::pair<std::string, std::string>>{
                     {"20621.1.15.11.10", "621.1.15.11.10"}, {"19618.3.11", "618.3.11"}, {"621.1.15", "621.1.15"},
                     {"605", "605"}, {"20621", "621"}, {"", ""}, {"abc1234.5", "abc1234.5"}}) {
                if (uniapi::detail::WebKitVersion(given) != expected) wrong += " WebKitVersion(" + given + ")";
            }
            for (const auto& [given, expected] : std::vector<std::pair<std::string, std::string>>{
                     {"131.0.2903.86", "131.0.2903.86"}, {"133.0.3065.0 canary", "133.0.3065.0"},
                     {" 132.0.2957.26 beta", "132.0.2957.26"}, {"", ""}}) {
                if (uniapi::detail::RuntimeVersion(given) != expected) wrong += " RuntimeVersion(" + given + ")";
            }
            checks.Check("the web engine's version read from macOS's WebKit and WebView2's version strings", wrong.empty(),
                         wrong.empty() ? "ok" : wrong);
        }

        // backgroundColor: "#rrggbb" or "#rgb", anything else ignored (reported on stderr).
        {
            std::string wrong;
            for (const auto& [given, expected] : std::vector<std::pair<std::string, int>>{
                     {"", -1}, {"#1b2636", 0x1b2636}, {"#1B2636", 0x1b2636}, {"#000000", 0}, {"#fff", 0xffffff},
                     {"#1a2", 0x11aa22}, {"1b2636", -1}, {"#1b26", -1}, {"#1b2636f", -1}, {"#gggggg", -1}, {"red", -1}}) {
                if (uniapi::detail::ColorValue(given) != expected) wrong += " \"" + given + "\"";
            }
            checks.Check("backgroundColor: \"#rrggbb\" and \"#rgb\" read, anything else ignored", wrong.empty(),
                         wrong.empty() ? "ok" : wrong);
        }

        // The program's details in its .exe (Windows: uniapi_icon's NAME, VERSION...).
#ifdef _WIN32
        {
            const std::string details = OwnDetails();
            const std::string expected = "1.2.3.0 | ProductName=uniapi desktop test | FileDescription=uniapi desktop test"
                                         " | FileVersion=1.2.3 | ProductVersion=1.2.3 | CompanyName=uniapi"
                                         " | LegalCopyright=\xC2\xA9 uniapi | Comments=The desktop test's \"details\"";
            checks.Check("the program's details in its .exe (uniapi_icon: Properties > Details)", details == expected, details);
        }
#else
        checks.NotApplicable("the program's details in its .exe", "Windows only; the icon test checks what is written");
#endif

        const bool clicked = co_await Call(app, "click", {"link"});
        const bool gotLink = co_await WaitFor(app, [&] { return external.size() >= 1; });
        co_await app.sleep(300ms);  // a navigation that wasn't refused would have begun
        checks.Check("a link to another site goes outside (onExternalURL); the page stays",
                     clicked && gotLink && external[0] == "https://example.com/from-a-link" && disconnects == 0
                         && app.connected(),
                     Join(external) + " disconnects " + std::to_string(disconnects));
        co_await Call(app, "click", {"blank"});
        const bool gotBlank = co_await WaitFor(app, [&] { return external.size() >= 2; });
        checks.Check("a link to a new window goes outside too; no window opens",
                     gotBlank && external[1] == "https://example.com/from-a-new-window" && app.windows().empty(),
                     Join(external));
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / ("uniapi-desktop-" + std::to_string(std::time(nullptr)));
        std::filesystem::create_directories(temp);
        std::string fileUrl = "file://" + GenericUtf8(temp) + "/";
#ifdef _WIN32
        fileUrl = "file:///" + GenericUtf8(temp) + "/";
#endif
        co_await Call(app, "goTo", {fileUrl});
        co_await app.sleep(500ms);
        checks.Check("a file: navigation is refused: the page stays, nothing goes outside",
                     disconnects == 0 && app.connected() && external.size() == 2,
                     Join(external) + " disconnects " + std::to_string(disconnects));

        // 3. Files dropped on the window (macOS: through a test hook).
        const std::vector<std::string> paths = {Utf8(temp / "a song.wav"), Utf8(temp / "b.txt")};
        if (Drop(kTitle, paths, 120, 80)) {
            const bool got = co_await WaitFor(app, [&] { return !dropped.empty(); });
            checks.Check("files dropped on the window reach onFileDrop: their paths, and where",
                         got && dropped == paths && dropX == 120 && dropY == 80,
                         Join(dropped) + " at " + std::to_string(dropX) + "," + std::to_string(dropY));
        } else {
            checks.NotApplicable("files dropped on the window", "the test can't make a drop here; try by hand");
        }
        std::error_code ignored;
        std::filesystem::remove_all(temp, ignored);

        // 4. A second launch: handed over, and it ends.
        const std::vector<std::string> second = {uniapi::executablePath(), "--second-launch", instanceId, "open",
                                                 "this file.wav"};
        const auto start = std::chrono::steady_clock::now();
        co_await uniapi::switchToBackground();
        const int code = Run(second);
        co_await app.switchToUi();
        const bool handed = co_await WaitFor(app, [&] { return launchArgs.size() == 1; });
        std::error_code error;
        const std::string here = Utf8(std::filesystem::current_path(error));
        checks.Check("a second launch hands its arguments and folder to the running app (onSecondInstance), and ends",
                     code == 0 && handed
                         && launchArgs[0] == std::vector<std::string>(second.begin() + 1, second.end())
                         && launchDirs[0] == here && std::chrono::steady_clock::now() - start < 10s,
                     "exit " + std::to_string(code) + ", " + (handed ? Join(launchArgs[0]) + " in " + launchDirs[0] : "not handed"));

        std::printf("\ndesktop: %d passed, %d failed\n", checks.passed, checks.failed);
        std::fflush(stdout);
        if (!keepOpen) {
            app.quit();
            co_return;
        }
        app.call("showResults", json::array({checks.lines}));
        // By hand: say what would have opened, and what was dropped.
        app.onExternalURL = [&](const std::string& url) {
            std::vector<std::string> lines = checks.lines;
            lines.push_back("");
            lines.push_back("would open: " + url);
            app.call("showResults", json::array({lines}));
            return false;
        };
        app.onFileDrop = [&](const std::vector<std::string>& files, int x, int y) {
            std::vector<std::string> lines = checks.lines;
            lines.push_back("");
            lines.push_back("dropped at " + std::to_string(x) + "," + std::to_string(y) + ": " + Join(files));
            app.call("showResults", json::array({lines}));
        };
    };

    app.onConnect = [&](const std::string&) {
        if (started) return;
        started = true;
        flow();
    };
    int ticks = 0;
    app.every(60s, [&] {
        if (ticks++ == 0 || keepOpen) return;
        checks.Check("finished within 60 s", false);
        app.quit();
    });
    app.navigate("/index.html");
    app.run();
    return checks.failed == 0 && started ? 0 : 1;
}
