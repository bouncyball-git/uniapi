<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/logo/uniapi-name.svg">
    <img src="doc/logo/uniapi-name-dark.svg" width="441" height="120" alt="uniapi: the logo, a tile half ink, half blue, whose halves are [ and ] round a box in the crossed colours (C and J), and the name, uni in ink and api in blue">
  </picture>
</p>

# uniapi
API layer for C\C++ and JS interaction

A C++ library for desktop apps whose UI is a web page and whose work is done in C++:
JavaScript calls C++ functions, C++ calls JavaScript functions, and large data (video
frames, images, buffers) travels through zero-copy shared memory where the platform allows it.

```cpp
uniapi::AppOptions options;
options.title = "My app";
uniapi::App app(options);
app.serve(my_web);                                       // HTML/CSS/JS compiled into the program
app.expose("add", [](int a, int b) { return a + b; });  // JS: await app.native.add(1, 2)
app.onConnect = [&](const std::string&) {
    app.call("greet", uniapi::json::array({"from C++"}));  // runs a JS function
};
app.navigate("/index.html");
return app.run();
```

```js
const app = uniapi.connect({ page: 'main' });
console.log(await app.native.add(1, 2));        // 3
app.expose({ greet: (who) => console.log('hello ' + who) });
```

**Platforms:** Windows (WebView2) and Linux (WebKitGTK), both with zero-copy shared memory
for streams, and macOS 14+ (WKWebView), where streams always use the fetch fallback (which is
fast there: see Performance).

**Documentation:** [`doc/api.html`](doc/api.html) is the full API guide and reference, with
diagrams of how connections, calls and streams work. Open it in a browser.

## Requirements (Windows)

- Windows 10 or 11 with the [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
  (preinstalled on Windows 11). Shared memory needs Runtime 1.0.1661 or newer; older
  runtimes use the fetch fallback automatically.
- Visual Studio 2022 or Build Tools 2022 with the "Desktop development with C++" workload
  (MSVC, Windows SDK, and the bundled CMake and Ninja).
- Internet access the first time you configure: CMake downloads nlohmann/json and the
  WebView2 SDK.

## Requirements (Linux)

- A C++17 compiler, CMake, Ninja (or make), and with development files either
  **GTK 4.10+ and `webkitgtk-6.0`** (used when found) or **GTK 3 and `webkit2gtk-4.1`**
  (the fallback, for older distributions). `UNIAPI_GTK` picks one explicitly (see below).
  - Debian/Ubuntu: `sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev libwebkitgtk-6.0-dev`
    (or `libgtk-3-dev libwebkit2gtk-4.1-dev`)
  - Fedora: `sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config gtk4-devel webkitgtk6.0-devel`
    (or `gtk3-devel webkit2gtk4.1-devel`)
  - Arch/Manjaro: `sudo pacman -S base-devel cmake ninja gtk4 webkitgtk-6.0` (or `gtk3 webkit2gtk-4.1`)
- Internet access the first time you configure: CMake downloads nlohmann/json.
- X11 or Wayland. With GTK 3 on NVIDIA and Wayland, uniapi sets `__NV_DISABLE_EXPLICIT_SYNC=1`
  for itself before GTK starts: GTK 3 otherwise dies with "Error 71 (Protocol error)". Set the
  variable yourself to override it. GTK 3 builds also set `GTK_USE_PORTAL=1` (the desktop's own
  file dialogs, as GTK 4 uses) and, under X11, `WEBKIT_DISABLE_DMABUF_RENDERER=1` (WebKit's
  DMA-BUF renderer can leave the page blank there, e.g. on NVIDIA). GTK 4 doesn't need these; there uniapi sets
  `GSK_RENDERER=gl` instead (GTK's OpenGL renderer, called `ngl` before GTK 4.18; see Performance), again only if unset.
- WebKitGTK 2.40 or newer. Shared memory also needs uniapi's web process extension,
  `uniapi-webext/libuniapi_webext.so`, which the build puts in `build/`. Ship that folder next
  to your executable (or point `UNIAPI_WEBEXT_DIR` at it); without it, streams use the fetch
  fallback.
- Zero-copy on WebKitGTK means switching off JavaScriptCore's **Gigacage** in the app's web
  process: a hardening layer against JavaScript engine exploits, which aborts the process when
  a page is given outside memory. When shared memory is on, uniapi sets `GIGACAGE_ENABLED=no`
  before WebKit starts, and WebKit prints `Warning: disabling gigacage` on stderr once per
  process. uniapi pages are the app's own, which keeps the risk low;
  `AppOptions::sharedMemory = false` leaves Gigacage on. `G_MESSAGES_DEBUG=uniapi` shows which
  mode was chosen and why.

## Requirements (macOS)

- macOS 14 (Sonoma) or newer, on Apple silicon or Intel. WKWebView is part of the system.
- Xcode's Command Line Tools (`xcode-select --install`; the full Xcode isn't needed), CMake and
  Ninja (e.g. `brew install cmake ninja`).
- Internet access the first time you configure: CMake downloads nlohmann/json.
- No shared memory: WKWebView has no public way to hand a page outside memory, so streams
  always fetch their slots from the app. The fetch path is fast on macOS (about 4.7 GB/s, more
  than the other platforms' shared memory at 1080p).
- Programs in `build/` are plain executables; run from a terminal they come to the front with
  a Dock icon and menu bar of their own. `uniapi_install()` makes `.app` bundles (see
  [Installing an app](#installing-an-app)). Set `UNIAPI_DEBUG=1` to see the backend's debug
  messages.
- Each app gets the standard menu bar (app, Edit, View, Window): the Edit menu is what makes
  Cmd+C, V, X, A and Z work in the page's text fields. Cmd+Q, the Dock's Quit or a script's
  `quit` asks the main window's `onCloseRequested` (`CloseReason::Quit`), then closes it as
  `App::quit()` does, so `run()` returns and `main()` goes on (a script asking it to quit is
  told "User canceled": AppKit's own ending is declined). A logout, restart or shutdown closes
  the windows without asking but then ends the process at once, as the system waits for it:
  code after `run()` doesn't run then. Clicking the app in the Dock with no window on screen
  shows a hidden main window again.

## Building

The scripts in the repository root build everything and can also clean, test, install and
uninstall. They run the actions you give, in order; with none, they build:

```
.\build.ps1 [action...] [-Prefix <dir>] [-Config Release|Debug] [-Yes]                    # Windows
./build.sh  [action...] [--prefix=<dir>] [--config=Release|Debug] [--gtk=auto|4|3] [--yes]   # Linux, macOS
```

| Action | What it does |
|---|---|
| `clean` | Deletes the build folders: `build/` and every `build-*/` holding a CMake build (`build-gtk3/`, or one made by hand such as `build-asan/`). With `--gtk`/`--build-dir` (`-BuildDir` on Windows), only that build's folder. |
| `build` | Configures if needed and builds the library, examples and tests (Release by default). |
| `test` | Runs the tests with `ctest` (builds first). |
| `install` | Installs the example apps to `<prefix>/bin` (on Linux also the shared-memory extension and the apps' desktop entries and icons; on macOS each as `<prefix>/<app>.app` instead, see [Installing an app](#installing-an-app); builds first). |
| `uninstall` | Removes what `install` put in `<prefix>`, then the folders left empty. |
| `clean-profiles` | Deletes the web engine profiles of the programs this project builds, also under old program names (Windows: `%LOCALAPPDATA%\<program>` and old `<program>.exe.WebView2` folders in the build folder; Linux: `~/.local/share/<program>` and `~/.cache/<program>`; macOS: `~/Library/WebKit/<program>` and `~/Library/Caches/<program>`, and the same for `org.uniapi.<program>`, the installed apps'). It lists them and asks first (`-Yes` / `--yes` doesn't ask), and skips a folder holding anything but a profile, which may be another app's. Each program makes a fresh one when it next starts. |
| `help` | Shows the full help (also `-Help`, `-h`, `--help`). |

The prefix defaults to `install/` in the repository. `install` keeps the list of files it
installed in `<prefix>/uniapi-install-manifest.txt`, so `uninstall` (same prefix) removes
exactly those, even after `clean`. `build.ps1` sets up Visual Studio's build environment
itself, so any PowerShell window works; a prefix like `C:\Program Files\uniapi` needs an
elevated one. `build.sh` uses Ninja when installed, otherwise make, and runs only the
`install` and `uninstall` steps with `sudo`, when the prefix (e.g. `/usr/local`) isn't
writable. Examples:

```
.\build.ps1 clean build test
./build.sh install --prefix=/usr/local
./build.sh uninstall --prefix=/usr/local
```

Without the scripts: on Windows, open an **x64 Native Tools Command Prompt for VS 2022** in
the repository root; on Linux and macOS, any terminal. Then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the `uniapi` static library and, in `build/` (names without `.exe` on Linux and macOS):

- `hello.exe`: the smallest example (`examples/hello`): the page calls a C++ function and C++
  calls a JavaScript function. Start reading here.
- `hello_c.exe`: the same app in C (`examples/hello_c`), through the C API.
- `uniapi_stream.exe`: the demo (`examples/uniapi_stream`), a frame streaming benchmark.
- `uniapi_logo.exe`: the uniapi logo (`examples/uniapi_logo`): the light, dark, gray and embossed
  versions at 16 to 128 px (the PNGs made for each size, at real screen pixels), larger as SVG
  and with the name, on a background that changes colour every three seconds; pause it (Space),
  step (arrow keys), pick a swatch or any colour, or a random one (R). The logo's files
  (`doc/logo`) are compiled in as a second table and reach the page from C++ as binary data.
- `uniapi_notify.exe`: a notification from C++ (`examples/uniapi_notify`); see
  [Notifications](#notifications).
- `uniapi_tray.exe`: an icon in the tray with a menu (`examples/uniapi_tray`); closing the
  window hides it; see [The tray](#the-tray-uniapi_tray-trayincludeuniapitrayhpp).
- `uniapi_test.exe` and `uniapi_test_c.exe`: the tests; `uniapi_checker.exe` and
  `uniapi_checker_c.exe`: the checks by hand; `uniapi_latency.exe`: how long messages take
  between C++ and the page, and C++'s answer to a click (see [Testing](#testing)).

Each program is a single file with its pages compiled in (and on Windows, the WebView2
loader linked statically).

| CMake option | Default | Effect |
|---|---|---|
| `UNIAPI_BUILD_EXAMPLES` | `ON` | Build the examples: `hello`, `hello_c`, `uniapi_stream`, `uniapi_logo`, `uniapi_notify` and `uniapi_tray`. |
| `UNIAPI_BUILD_TESTS` | `ON` | Build the tests (registered with CTest), the two checker apps and the latency probe. |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | `14.0` | macOS only: the oldest macOS the programs run on (14 or later). |
| `UNIAPI_GTK` | `auto` | Linux only: `auto` uses GTK 4 + `webkitgtk-6.0` if installed, GTK 3 + `webkit2gtk-4.1` otherwise; `4` or `3` insists on one. CMake prints which it took. |
| `UNIAPI_STREAM_EMBED_WEB` | `ON` | Compile uniapi_stream's `web/` folder into the program. `OFF` reads it from the source folder instead, so you can edit HTML/JS and press F5 without rebuilding. |
| `UNIAPI_EMBED_MINIFY` | `ON` | `uniapi_embed()` minifies `.js` and `.css` files with esbuild, and `.html`, `.svg` and `.json` with tdewolff/minify (both fetched, pinned) before embedding them; `UNIAPI_ESBUILD` and `UNIAPI_MINIFY` name other copies. |
| `UNIAPI_EMBED_COMPRESS` | `OFF` | `uniapi_embed()` stores files that shrink by a tenth or more zlib-compressed, for a smaller program file; they are inflated each time they are served. `COMPRESS` does it for one call. |
| `UNIAPI_EMBED_INCBIN` | `ON` with GCC/Clang on Linux (if a test program using it works), else `OFF` | `uniapi_embed()` brings the bytes in with the assembler's `.incbin` instead of compiling C++ arrays: no compile time for the data. |
| `UNIAPI_GC_SECTIONS` | `ON` | GCC/Clang on Linux: apps that link uniapi drop the functions and data nothing uses (`-ffunction-sections -fdata-sections`, `-Wl,--gc-sections`), as MSVC's Release linker does by itself. A larger app: 1004 KB → 725 KB stripped. macOS: `-Wl,-dead_strip` (Apple's linker needs no compiler flags for it); the same app 1015 KB → 763 KB. |
| `UNIAPI_STRIP` | `ON` | GCC/Clang on Linux and macOS: Release and MinSizeRel apps (and the web process extension) are linked stripped (`-s`): no symbol table, as MSVC's exes (theirs goes to a `.pdb`). Debug and RelWithDebInfo keep theirs; `OFF` keeps it in Release too, e.g. for crash reports. macOS: `-Wl,-x`, the local symbols (nearly all; Apple's linker has no full strip, and `strip` afterwards saved only 10 KB more); a larger app 763 KB → 563 KB. |

On Linux and macOS this project builds Release at `-O2` (GCC/Clang; on macOS also the
Objective-C++ backend), as MSVC's Release does (`/O2`), instead of CMake's `-O3`: a larger
app 631 KB instead of 725 KB on Linux, 547 KB instead of 563 KB on macOS, no speed lost in
these apps (macOS stream demo, 1080p: 584 fps, 573 at `-O3`). Only
CMake's default is replaced, at a build folder's first configure; other flags, or `-O3` set again
afterwards (`-DCMAKE_CXX_FLAGS_RELEASE=...`), stay, and a project that adds uniapi keeps its own.

To use the library in your own CMake project, add this repository with `add_subdirectory`
or `FetchContent` (set `UNIAPI_BUILD_EXAMPLES` and `UNIAPI_BUILD_TESTS` to `OFF`), link
`uniapi`, and embed your pages. It builds along with your app; there is no separate SDK
install, since the library is only a few source files:

```cmake
add_executable(myapp WIN32 main.cpp)
target_link_libraries(myapp PRIVATE uniapi)
uniapi_embed(myapp my_web "${CMAKE_CURRENT_SOURCE_DIR}/web")   # then: UNIAPI_ASSETS(my_web);
uniapi_icon(myapp "${CMAKE_CURRENT_SOURCE_DIR}/myapp.ico")     # Windows: its icon (default: the uniapi logo)
uniapi_install(myapp)                                          # for cmake --install
```

### The program's icon

`uniapi_icon(<target> [<icon.ico>])` gives a program its icon on Windows: Explorer shows it on
the `.exe`, the taskbar and Alt+Tab on the running program, and every uniapi window in its
title bar (the window class takes the program's first icon resource; `App::setIcon` and
`Window::setIcon` still give a window one of its own). Without `<icon.ico>` it is the uniapi
logo (`doc/logo/uniapi.ico`: 16, 32, 48 and 256 px; `uniapi_logo.exe` shows it); every example,
test and tool here uses it. A program without an icon resource shows Windows' generic one.

**Windows: the program's details.** The same call can give the program its details, which
Explorer shows under Properties > Details and Task Manager as the program's name. Each is
optional; with none of them the program gets its icon alone:

```cmake
uniapi_icon(myapp "${CMAKE_CURRENT_SOURCE_DIR}/myapp.ico"
            NAME "My App" VERSION ${PROJECT_VERSION} COMPANY "My Company" COPYRIGHT "© 2026 Me")
```

| Keyword | Shown as | |
|---|---|---|
| `NAME` | Product name | |
| `DESCRIPTION` | File description, and Task Manager's name for the program | `NAME` if not given |
| `VERSION` | File version and Product version | one to four numbers, `1.2.3` |
| `COMPANY` | Company | |
| `COPYRIGHT` | Copyright | |
| `COMMENTS` | Comments | |

A `VERSION` that isn't one stops the configure on every platform, so a mistake shows wherever
you build; one given as `""` is left out. The texts are UTF-8 and stored as US English, which
Explorer shows under every display language.

On Linux the call does nothing: an executable has no icon of its own there. Under GTK 3 (X11)
`setIcon(png)` puts one in the title bar and the taskbar; GTK 4, and Wayland under either, take
a window's icon only from the app's `.desktop` file and the icon theme, found through the
window's app id, which is the executable's name. So there the icon belongs to the installed
app: `uniapi_install()` installs the desktop entry and its icon (the uniapi logo unless `ICON`
names another), see [Installing an app](#installing-an-app).

On macOS the call does nothing either: an app's icon is its bundle's, which `uniapi_install()`
makes (`ICON`, default the uniapi logo). Mac windows have no icons of their own;
`App::setIcon(png)` sets the app's icon in the Dock and the app switcher while it runs
(further windows' `setIcon` returns false).

### Embedding pages

`uniapi_embed(<target> <name> <directory> [BUNDLE <entry>...] [KEEP <pattern>...] [NO_MINIFY] [COMPRESS] [NO_COMPRESS])`
compiles every file under `<directory>` into the program. A small tool built with uniapi (`uniapi_embed_tool`)
writes the generated file at build time, in milliseconds per megabyte, and only files that
changed are embedded again:

- Files are minified first, so the web engine parses less and less is stored: `.js` and `.css`
  with esbuild (comments and whitespace go, local names get short, nothing else changes: no
  bundling, no new syntax), `.html`/`.htm`, `.svg` and `.json` with tdewolff/minify (comments
  and whitespace go; HTML keeps its document and end tags; strings and values stay). CMake
  fetches esbuild 0.24.0 and minify 2.24.21 for the machine that builds, each checked against
  its published hash; `UNIAPI_ESBUILD` and `UNIAPI_MINIFY` name other copies. minify has no
  build for Windows on ARM64: there `.html`, `.svg` and `.json` are embedded as written.
  `NO_MINIFY` (one call) or `UNIAPI_EMBED_MINIFY=OFF` (all) embeds everything as written, e.g.
  to read it in the browser's developer tools. A syntax error stops the build with the tool's
  message.
- `BUNDLE` names scripts and style sheets to bundle (`.js`, `.mjs`, `.css`; paths inside
  `<directory>`): esbuild puts each entry and everything it imports into one file, embedded
  under the entry's path (a script as an ES module), so the page refers to the same path
  before and after and the web engine fetches one file where it fetched many:

  ```cmake
  uniapi_embed(myapp my_web "${CMAKE_CURRENT_SOURCE_DIR}/web" BUNDLE js/main.js css/style.css)
  ```

  The files that went into a bundle are not embedded by themselves; every other file is, as
  without `BUNDLE`. Bundles are minified like the rest; with `NO_MINIFY` they are bundled and
  left readable. It needs the embed tool and an esbuild: it is refused when cross-compiling,
  and with `UNIAPI_EMBED_MINIFY=OFF` unless `UNIAPI_ESBUILD` names an esbuild.
- `KEEP` names files to embed as they are, not minified, by patterns for the file's name
  (`*` any characters, `?` one, `[a-z]` a set): `KEEP "*.min.js"` for scripts that are
  minified already, or files that must stay byte for byte what they are.
- Compression is an option, off by default: it makes the program file smaller and nothing
  faster. With `COMPRESS` (one call) or `UNIAPI_EMBED_COMPRESS=ON` (all; `NO_COMPRESS` then
  leaves one call out) files that shrink by a tenth or more (text: HTML, JS, CSS, JSON,
  SVG...) are stored zlib-compressed; formats that are compressed already (images, fonts,
  media, archives) are stored as they are. A compressed file is inflated each time a page asks
  for it, into memory that is freed once the file is sent: nothing inflated is kept. Code that
  reads an embedded file itself uses `uniapi::assetData(asset)` for its bytes
  (`Asset::packedSize` says whether it is compressed); that one inflates once and keeps the
  bytes, so its pointer stays good.
- With MSVC (and elsewhere without `.incbin`) the bytes are C++ arrays, which compile at about
  a megabyte a second; with GCC or Clang on Linux the assembler's `.incbin` brings them in,
  with no compile time for the data (`UNIAPI_EMBED_INCBIN`).

Measured on Windows (MSVC), with `COMPRESS`: a 10 MB JavaScript bundle is embedded in 0.7 s,
compiles in about 3 s and adds 2.5 MB to the program (10 s and 10 MB uncompressed). Minifying shrinks this
repository's scripts by about half before compression: a 313 KB script library becomes
138 KB, stored as 50 KB (91 KB compressed alone); `uniapi.js` 28.5 KB becomes 8.4 KB; its HTML
pages shrink by about a third (a fifth once compressed). Both minifiers were compared on these
files first: on JavaScript within 0.5 % of each other once compressed, both passing every
test; esbuild does the scripts for its far wider use on JavaScript of every kind, minify the
types esbuild doesn't handle.

A compressed file is inflated by the app, not the web engine: when the page asks for it,
uniapi's request handler inflates it (`src/core/assets.cpp`, miniz) on the thread that serves
the page's requests, which is the UI thread, and hands the engine the plain bytes; they are
freed when sent. Inflating runs at about 250 MB a second (zlib's speed, measured on Linux; a
660 KB script takes about 2 ms, 10 MB about 40 ms), each time the file is asked for. The
embedding used to be a
CMake script that took about 40 s for 400 KB and grew with the square of the size; it is still
the fallback when cross-compiling, where the tool can't run on the build machine, now linear
but slower (about 3 s per megabyte) and uncompressed.

### Installing an app

`uniapi_install(<target> [DESTINATION <dir>] [NAME <name>] [ICON <file.svg|file.png>] [COMMENT <text>] [CATEGORIES <name>...] [BUNDLE_ID <id>] [VERSION <v>] [COPYRIGHT <text>])`
installs an app so it runs from wherever it is installed (`cmake --install build --prefix
<prefix>`):

| Platform | Installed files |
|---|---|
| Windows | `<prefix>/bin/<app>.exe`, nothing else: pages and the WebView2 loader are compiled in |
| Linux | `<prefix>/bin/<app>`, plus the shared-memory extension in `<prefix>/lib/<app>/uniapi-webext/` (`<dir>/../lib/...` for another `DESTINATION`), where the app looks for it; and the desktop entry `<prefix>/share/applications/<app>.desktop` with its icon in `<prefix>/share/icons/hicolor/` (`scalable/apps/<app>.svg` or `256x256/apps/<app>.png`) |
| macOS | `<prefix>/<NAME>.app` (`DESTINATION` defaults to `.` here): the program in `Contents/MacOS/<app>`, an `Info.plist` (`BUNDLE_ID`, default `org.uniapi.<app>`; `VERSION`, default 1.0) and the icon, `Contents/Resources/<app>.icns`, made from `ICON` during the build. The bundle is signed ad hoc (`codesign -`) after installing |

`DESTINATION .` makes a self-contained folder instead (on Linux the extension goes to
`uniapi-webext/` next to the executable, and there is no desktop entry).

On Linux the desktop entry gives the app its menu entry, and its icon in the title bar and
the taskbar: the desktop matches it to the window's app id, the executable's name. `NAME` is
the name the menu shows (default: the executable's), `ICON` an SVG or PNG (default: the uniapi
logo). `COMMENT` says in a few words what the app is (the entry's `Comment`: the menu's
second line or tooltip; on macOS Finder's Get Info text). `CATEGORIES` are the menu's
categories, the Desktop Menu Specification's names (`CATEGORIES System TerminalEmulator`;
`Utility` if not given). `COPYRIGHT` is the copyright line Finder shows on macOS. Its `Exec`
is the installed program's full path, written when installing. The desktop
reads entries from `~/.local/share` and `/usr/local/share` (`--prefix ~/.local` or
`/usr/local`), not from another prefix. On macOS, `--prefix ~/Applications` (or
`/Applications`) puts the apps where Launchpad and Spotlight find them; the build folder keeps
the plain programs. The examples use the default, so `cmake --install build --prefix <dir>`
(or `build.ps1` / `build.sh install`) puts them in `<dir>/bin`.

The web engine's profile (cache, storage) goes to a per-user folder named after the
executable, which is writable wherever the app is installed: `%LOCALAPPDATA%\<app>` on
Windows, `~/.local/share/<app>` and `~/.cache/<app>` on Linux, `~/Library/WebKit/<bundle id>`
(a plain program: `<app>`) and `~/Library/Caches/<same>` on macOS. `AppOptions::dataDirectory`
puts it somewhere else; on macOS WebKit keeps every profile in its own folders and finds it by
an identifier, so there the path names a separate profile (the same path, the same profile)
but its files stay under `~/Library/WebKit/<app>/WebsiteDataStore/`. On Windows,
`AppOptions::browserArguments` passes extra switches to the web engine (WebView2), such as
`--proxy-server=host:8080`; Linux and macOS ignore it.

After a click or key, Chromium normally holds the page's other tasks until the next frame is
drawn, C++'s messages among them, so C++'s answer to a click (a lit key, a value it sets) shows
a frame later. uniapi switches that off on Windows (`--disable-features=DeferRendererTasksAfterInput`,
joined with any `--disable-features` of your own): measured in a page of controls, a key's
answer from C++ was drawn 13 ms after the press instead of 26 ms. `AppOptions::deferTasksAfterInput
= true` (C: `defer_tasks_after_input`) keeps Chromium's way.

## Testing

```
ctest --test-dir build --output-on-failure
```

This runs `uniapi_test --exit` and `uniapi_test_c --exit`, each with shared memory and
with the fetch fallback, `uniapi_windows_test --exit` (Windows; both modes too: further
windows, their pages, calls, events, streams, a modal window, closing), `uniapi_lifecycle_test
--exit` (hiding and showing windows with their pages working, closing by the user asking
`onCloseRequested`, `close()` and `quit()` not asking; on Windows and macOS a hidden modal
window not holding the main one; on macOS Quit asking), `uniapi_desktop_test --exit` (the
clipboard both ways, as other programs see it; links and new windows going outside; a `file:`
navigation refused; on macOS dropped files; a second launch handed to the running app),
`uniapi_placement_test --exit` (size, position, centring, minimised, maximised and the normal
size, on top, placements put back and remembered, for a further window and across runs for the
main window; the position checks n/a where windows can't be placed), `uniapi_modal_test` (Linux: a modal window keeping clicks from the main window, with clicks made
inside the app on GTK 3; skipped on GTK 4, which can't make events: `--by-hand` there),
`uniapi_strict_test --exit` and `--loose --exit` (`strictCalls` refusing an extra argument and
an unknown field, and the default letting them through) and `uniapi_tray_test --exit` (icons in the real tray, their menus chosen through the platform
where it can be driven, `remove()` and the App's end taking them away, the C API; it passes
saying so where the desktop has no tray). Each run opens a window
(without taking keyboard focus), prints
every check, and exits with 0 only if all of them pass. The API test (C++20) checks calls in
both directions, errors, events, binary data both ways, coroutines, streams with
backpressure, replacing a stream, DOM command lists, a page reload and the fallback when
shared memory doesn't reach the page. The C API test is a C program that calls every function
in `uniapi.h` that can run unattended (all but `uniapi_alert` and the two file dialogs). A run
takes about a second (the API test about 3 s on Windows; about 1.5 s on Linux). On a brand-new
WebView2 profile (a new machine, a deleted or renamed profile folder), quick runs take a few
seconds longer: the engine keeps downloading its components after each run until one run
lasts long enough, up to a minute, to finish.

The checks that need a person are in two checker apps, one button each, not run by ctest:
`build/uniapi_checker` (C++: an alert, the dialogs, window options, the icon, full screen;
with `--dialog-first`, an alert before `run()`) and `build/uniapi_checker_c` (the same through
the C API).

### Measuring latency

`build/uniapi_latency` measures how long things take between C++ and the page, on this machine
and this web engine (not run by ctest; there is no pass or fail):

1. **Messages** (runs by itself): the page calls C++ 100 times idle, with the page busy (10 ms
   of work every frame) and with C++ busy (8 ms every 33 ms), and shows each way (on a clock
   synced to C++'s, its error bound shown) and the round trip.
2. **After a click** (by hand): each click on the pad calls C++, timed from the click itself
   (the input event's own time) until the answer is back; clicks alternate between changing the
   screen and not, and a checkbox animates the page.

The results show on the page and print to the console. `--exit` runs the first part only, prints
it and closes; `--keep-defer` keeps Chromium's deferral of tasks after an input (see below), to
compare; `--browser-arguments="..."` passes switches to the web engine.

Measured on Windows 11 (WebView2, Release): a message takes 0.2-0.4 ms each way, about 0.7 ms
there and back, idle or with either side busy (a busy side only delays a message that arrives
during its work). After a click, C++'s answer is back in 1-2 ms; with `--keep-defer`, 8-13 ms:
Chromium holds the page's other tasks after an input until the next frame is drawn, which is
why uniapi switches that off by default (`AppOptions::deferTasksAfterInput`). In a real page
the rest is the page's own load: a page busy every frame handles a click late, up to a frame
(the probe's second part measures exactly that).

Started directly, `build/uniapi_test` and `build/uniapi_test_c` keep their window
open afterwards and list every check there, grouped by step with the value it got, until you
close it; the title shows the summary. Add `--no-shared-memory` for the fallback or `--exit`
to close by itself when done; the API test also takes `--data-dir=<path>` to keep the web
engine's profile there (`AppOptions::dataDirectory`). Its checks are in
`tests/selftest/main.cpp` (C++ side) and `tests/selftest/web/selftest.js` (page side); the C
API test's in `tests/c_api/main.c` and `tests/c_api/web/index.html`.

## Using the library

### C++ (`include/uniapi/uniapi.hpp`)

```cpp
UNIAPI_ASSETS(my_web);                       // generated by uniapi_embed()

uniapi::AppOptions options;                  // title, width, height, sharedMemory, activate, host, dataDirectory
options.title = "My app";
uniapi::App app(options);
app.serve(my_web);                           // or app.serveDirectory("path/to/web") while editing

// JavaScript calls C++. Each JS argument becomes one parameter; the return value goes back
// as JSON; an exception becomes a rejected promise. Too few arguments or a wrong type reject the
// call ("expected 2 argument(s), got 1", "argument 1: type must be number..."); an extra
// argument or an unknown object field is let through, unless options.strictCalls = true
// ("expected 2 argument(s), got 3", "argument 1: unknown field \"volum\"").
app.expose("add", [](int a, int b) { return a + b; });

// Work that finishes later (another thread, a dialog): answer through the Reply.
app.expose("load", [&](const uniapi::json& args, const uniapi::Reply& reply) {
    std::string path = args[0].get<std::string>();
    std::thread([path, reply] { reply.resolve(Load(path)); }).detach();  // Reply works from any thread
});

// Binary data from the page (ArrayBuffer, typed array, Blob/File) arrives as uniapi::Bytes.
app.expose("thumbnail", [](const uniapi::Bytes& image, int width) {
    return MakeThumbnail(image.data(), image.size(), width);
});

// Events from the page.
app.on("clicked", [](const uniapi::json& data) { /* ... */ });
app.off("clicked");                          // no longer handled
app.unexpose("add");                         // no longer callable

// C++ calls JavaScript, after a page has connected.
app.onConnect = [&](const std::string& page) {
    app.call("confirm", uniapi::json::array({"Overwrite?"}), [](const uniapi::Result& r) {
        if (r.ok && r.value.get<bool>()) { /* ... */ }
    });
    app.emit("progress", {{"done", 3}, {"total", 10}});
};

app.navigate("/index.html");
return app.run();                            // returns when the window is closed
```

Also on `App`: `every(interval, fn)` / `cancel(id)` for high-resolution timers on the UI
thread, `post(fn)` to run code on the UI thread from any thread, `openFileDialog(...)` (files
or folders, one or several) and `saveFileDialog(...)`, `setTitle(...)`, `setFullscreen(on)`,
`setTitleBar(mode)`, `setIcon(png, size)`, `onDisconnect`, and `sharedMemory()`. `AppOptions`
also sets `minWidth`/`minHeight`, `maxWidth`/`maxHeight` (not on Linux with GTK 4, which cannot
limit a window's size), `backgroundColor` (`"#rrggbb"`: what the window shows before the page
has drawn, at its edges while resizing and through a page without a background), `resizable`,
`fullscreen` and `titleBar`; `width`/`height` are the page's area, without title bar and
borders. `WindowOptions` takes the same for a further window.

**Developer tools.** `AppOptions::devTools` (off by default, for a program in people's hands;
on while developing) lets every page be inspected with the web engine's own tools: on Linux
WebKit's Web Inspector ("Inspect Element" in the page's right-click menu, always in a window of
its own), on Windows WebView2's DevTools (F12, "Inspect"), on macOS Safari's Develop menu, which
lists the page (Safari > Settings > Advanced > Show features for web developers).
`window.openDevTools()` opens them from the program on Linux and Windows; macOS gives no way.
C: `dev_tools`, `uniapi_window_open_dev_tools`.

```cpp
uniapi::OpenDialogOptions o;
o.multiple = true;
o.directory = lastFolder;                                   // the folder it opens in
o.filters = {{"Images", "*.png;*.jpg"}, {"All files", "*"}};  // the file types to choose from
app.post([&app, o] { auto paths = app.openFileDialog(o); /* ... */ });
```

The title bar and frame follow the system's light or dark setting, also when it changes while
the app runs (`TitleBar::System`, the default); `TitleBar::Dark` or `TitleBar::Light` forces
one, e.g. to match the page's theme (C: `title_bar` in the options, `uniapi_set_title_bar`).
Windows 10 20H1 and later, Windows 11. On Linux, System follows the desktop's preference from
the XDG desktop portal (GNOME 42+, KDE Plasma 5.24+), including changes; where the window
manager draws the title bar (KWin on Plasma, for GTK 3 and GTK 4 windows) it follows the
desktop by itself, and Dark and Light don't change it; where GTK draws it (GNOME), Dark and
Light ask the GTK theme for its dark or light variant. The setting is GTK's dark-theme
preference, which WebKitGTK also gives the page: on Linux the page's `prefers-color-scheme`
follows the mode too (Light makes it light on a dark desktop), where on Windows and macOS it
stays the system's. On macOS the mode is the window's appearance (Aqua or Dark Aqua), per
window.

**Threads.** `App`, `Window` and `Stream` belong to the UI thread: the one that made the `App`,
where every handler, timer and posted function runs.

- Methods that only do something (`emit`, `call`, `setTitle`, `show`, `hide`, `close`,
  `navigate`, `expose`, `on`, `quit`, `cancel`, `serve`, `setPosition`, `minimize`...) work
  from any thread: called elsewhere they post themselves to the UI thread and return at once, so
  they happen a moment later, in order with other posted work (`call`'s `done` runs on the UI
  thread).
- Methods that answer something or must act at once (`connected`, `page`, `visible`, `size`,
  `placement`, `webEngine`, `stream`, `sendDom`, `openWindow`, `windows`, `every`, the dialogs, `run`,
  `Stream::acquire` and `publish`...) throw `std::logic_error` when called elsewhere, naming
  themselves, instead of racing with the UI thread. A worker may fill a stream slot's memory
  between `acquire` and `publish`, both on the UI thread.
- `post()`, `notify()`, `Reply` and `onUiThread()` work from anywhere; `co_await
  app.switchToUi()` brings a coroutine back to the UI thread. C: the functions that answer return
  0 or NULL off the UI thread, `uniapi_last_error()` naming them; `uniapi_on_ui_thread`.

### More windows

`app.openWindow(options)` opens a further window with its own page, connected to C++ the same
way: it is a `uniapi::Window`, which has everything above that concerns a page and its window
(`navigate`, `expose`, `on`, `call`, `emit`, `callAsync`, `stream`, `sendDom`, `onConnect`,
`onDisconnect`, `connected`, `setTitle`, `setSize`, `show`, `setIcon`...), plus `close()` and
`onClosed`. `App` is the main window and adds what belongs to the program (files, timers,
`post`, dialogs, `run`).

```cpp
uniapi::WindowOptions options;                   // title, width, height, owned, modal, ...
options.title = "Mixer";
uniapi::Window& mixer = app.openWindow(options);
mixer.expose("fader", [](int channel, double db) { /* ... */ });   // its own functions
mixer.onConnect = [&](const std::string&) { mixer.emit("levels", Levels()); };
mixer.onClosed = [&] { /* drop references to it: it is destroyed once this returns */ };
mixer.navigate("/mixer.html");
```

- **Owned** (the default): it opens centred over the main window, stays above it, is minimised
  and closed with it. `owned = false`: a window of its own in the taskbar, still closed with the
  app. **Modal** (`modal = true`): the main window takes no input until it closes, as a dialog.
- A further window's page calls its own window's functions, and the app's (`app.expose`) where
  its window has none of that name; the same for events. C++'s `call` and `emit` on a window
  reach that window's page only.
- Each window has its own streams (`mixer.stream(...)`) and DOM lists; shared memory as in the
  main window.
- One stream can feed several windows: `spectrum.addWindow(mixer)` sends the same frames to that
  window's page too, under the same name. C++ writes each slot once (with shared memory every
  page reads the same memory); the slot comes back once every page it went to has handed it
  back, so the slowest page sets the pace. `removeWindow` stops it; replacing the stream keeps
  its windows.
- Opened before `run()`, it opens with the main window. The App owns it: it lives until it has
  closed (after `onClosed` returns), or the App ends; `app.windows()` lists the open ones.
- `app.sender()`: inside an exposed function or event handler, the window whose page sent it
  (the further window when its page reached the app's function): a handler can answer only
  its own window's page with it.
- macOS: an `NSWindow` and web view sharing the main window's profile; owned windows open
  centred over the main window, and a modal one keeps clicks and keys from it until it closes.
- Linux: a GTK window and web view in the main window's web context. GTK's dark title bar is
  app-wide, so further windows follow the main window's `titleBar` setting.

### Hiding, showing and closing

```cpp
options.startHidden = true;         // opens hidden: the page loads and works, nothing on screen
app.show();                         // in front again (restored if minimised); app.visible()
app.hide();                         // off the screen, not closed: the page keeps running

app.onCloseRequested = [&](uniapi::CloseReason reason) {
    if (reason == uniapi::CloseReason::Window) {  // close button, Cmd+W, Alt+F4, the window manager
        app.hide();                                // keep running instead
        return false;
    }
    return true;                                   // CloseReason::Quit: macOS Cmd+Q, the Dock's Quit
};
```

Every window has these (`WindowOptions::startHidden` too; C: `start_hidden`, `uniapi_hide`,
`uniapi_visible`, `uniapi_on_close_requested` and their `uniapi_window_*` forms).
`onCloseRequested` is asked only when the user closes a window; `close()`, `quit()` and the App
ending don't ask, so a handler can say no, ask the page ("unsaved changes?") and `close()` once
answered. An empty handler, or one that throws, lets the window close. A page's `app.show()`
(`showWhenReady`) doesn't show a window C++ keeps hidden, and a window first shown after
starting hidden waits for its page to draw. A modal window holds the main window only while it
is shown. On macOS, clicking the app in the Dock with no window on screen shows a hidden main
window again.

### The tray (`uniapi_tray`, `tray/include/uniapi/tray.hpp`)

```cpp
#include <uniapi/tray.hpp>                   // target_link_libraries(myapp PRIVATE uniapi_tray)

uniapi::tray::Tray tray(app, {logoPng, false, "Mixer"});  // icon (PNG), macOS template, tooltip
tray.setMenu({
    {"Show", [&] { app.show(); }},
    {"Muted", [&] { ToggleMute(); }, muted},           // checked
    {"Recording...", {}, false, true},                  // disabled
    uniapi::tray::Item::separator(),
    {"Quit", [&] { app.quit(); }},
});
tray.onClick = [&] { app.show(); };          // empty: a click opens the menu
app.onCloseRequested = [&](uniapi::CloseReason reason) {
    if (reason == uniapi::CloseReason::Quit) return true;
    app.hide();                              // closing the window keeps the app in the tray
    return false;
};
```

- A part of its own: only apps that link `uniapi_tray` carry it. The icon appears once `run()`
  runs, and goes when the `Tray` is destroyed, with `remove()`, or when the `App` ends
  (whichever comes first). Change the menu, a check mark or a label, by setting it again.
- `uniapi::tray::available()`: always on macOS and Windows; on Linux when the desktop has a tray
  host (a StatusNotifierWatcher: KDE Plasma, XFCE, Cinnamon, LXQt, waybar...), which stock GNOME
  hasn't without the AppIndicator extension. Without one, `Tray` throws `std::runtime_error`, and
  an app shouldn't hide its window to the tray.
- macOS: an `NSStatusItem` in the menu bar, the icon 18 points high (a 36-pixel PNG is sharp on
  Retina), a template drawn in the menu bar's colour. Windows: a notification-area icon and
  popup menu (`Shell_NotifyIcon`), put back when Explorer restarts. Linux: a StatusNotifierItem and
  dbusmenu over GDBus, each icon on a session bus connection of its own.
- The right button (macOS: also Control-click) always opens the menu; `onClick` takes the plain
  click. On Linux set `onClick` before the icon appears (in `main()`): hosts read then whether a
  click opens the menu.
- `examples/uniapi_tray` (`--hidden`: in the tray only, as an app started at login). C:
  `uniapi/tray.h` (`uniapi_tray_new`, `_set_menu`, `_set_icon`, `_set_tooltip`, `_on_click`,
  `_free`, `uniapi_tray_available`).

### Notifications

```cpp
std::string id = app.notify({"Export done", "mix.wav is ready."}, [](const uniapi::Result& r) {
    if (!r.ok) std::printf("not shown: %s\n", r.error.c_str());  // r.value: its id
});
uniapi::Result shown = co_await app.notifyAsync({"Export done", "mix.wav is ready."});
app.onNotificationClicked = [&](const std::string& id) { app.show(); };  // the default
```

- Every `notify` shows a new notification; its `id` (made unique across runs when left empty)
  only names it for `onNotificationClicked`. Safe from any thread; before `run()` it waits for
  `run()`. The person may never see one (do not disturb, notifications turned off).
- Linux: the desktop's notification server (`org.freedesktop.Notifications` over D-Bus); the
  app's name and icon come from its `.desktop` file (`uniapi_install()`).
- macOS: Notification Center, from an `.app` bundle only (`uniapi_install()`; a plain executable
  gets an error saying so). The first notification asks the person to allow them; those sent
  meanwhile wait for the answer.
- Windows: a notification-area balloon, which Windows 10 and 11 show as a notification (a toast
  proper needs a Start menu shortcut with an AppUserModelID, which a program not installed
  lacks); the program's icon is in the notification area while it is shown.
- `examples/uniapi_notify` tries them (install it first: see Installing an app). C:
  `uniapi_notify`, `uniapi_on_notification_clicked`.

### Where a window is: placement

```cpp
uniapi::Size size = app.size();                     // the page area now
std::optional<uniapi::Point> where = app.position(); // the frame's top-left corner; empty: unknown here
for (const uniapi::Screen& s : app.screens()) { /* s.x, s.y, s.width, s.height, s.scale, s.primary, s.current */ }
app.setPosition(100, 120);                           // before run(): where it opens
app.center();
app.minimize();  app.maximize();  app.restore();     // app.minimized(), app.maximized()
app.setAlwaysOnTop(true);                            // false where the platform can't
app.onPlacementChanged = [] { /* moved, resized, minimised, maximised, restored */ };

std::string kept = app.placement();                  // normal position and size, maximised
app.setPlacement(kept);                              // back on a screen that still exists
options.rememberPlacement = "main";                  // saved on close, restored on open
```

- Logical pixels from the primary screen's top-left corner; a position is the window frame's
  top-left corner, a size the page area's. Every window has these (`WindowOptions::
  rememberPlacement` too). Minimised and maximised asked of a hidden window apply when it is
  shown.
- A placement keeps the normal position and size (while maximised, the size before) and whether
  it was maximised. `setPlacement` keeps it on the screens there are now: a position off every
  screen (a laptop undocked) gives a centred window, no larger than its screen.
- `rememberPlacement` keeps placements in `<config>/<program>/uniapi-placement.json` (`~/.config`
  on Linux, `~/Library/Application Support` on macOS, `%AppData%` on Windows), saved when the
  window closes or the App ends.
- Linux: size, minimised and maximised everywhere; the position, centring and always on top with
  GTK 3 on X11 only (Wayland doesn't let apps place their windows or know where they are, and GTK 4
  has no API for it): there `position()` is empty and a placement keeps size and maximised.
  Wayland tells an app nothing of minimised windows: with GTK 4, a window `minimize()` minimised
  counts as minimised once the desktop stops showing it, and `restore()` brings it back; GTK 3
  on Wayland can minimise but neither tell nor undo it.
- C: `uniapi_window_size`, `_position`, `_set_position`, `_center`, `_minimize`, `_maximize`,
  `_restore`, `_minimized`, `_maximized`, `_set_always_on_top`, `_always_on_top`, `_placement`,
  `_set_placement`, `_on_placement_changed` (the main window: `uniapi_main_window(app)`), and
  `remember_placement` in both option structs.

### Links, dropped files, the clipboard, one instance

```cpp
// The page can't leave the app's pages: links, forms and window.open to other sites come here,
// cancelled (and no popup window ever opens). true opens the URL with the system.
app.onExternalURL = [](const std::string& url) { return url.rfind("https://", 0) == 0; };
uniapi::openURL("https://example.com");     // the default browser; mailto: the mail app...
uniapi::openPath("notes.txt");              // its default app; a folder: the file manager
uniapi::revealPath("mix.wav");              // the file manager, the file selected

app.onFileDrop = [](const std::vector<std::string>& paths, int x, int y) { /* ... */ };  // any window

app.setClipboardText("copied");             // other apps paste it
app.clipboardText([](std::optional<std::string> text) { /* ... */ });  // later, on the UI thread
std::optional<std::string> text = co_await app.clipboardTextAsync();  // in a coroutine

options.singleInstance = "org.example.mixer";  // a second launch hands over and ends
app.onSecondInstance = [&](const std::vector<std::string>& args, const std::string& dir) { /* ... */ };
```

- A link, a form or `window.open` to another site, or a script going there after a click,
  doesn't load in the window: `onExternalURL` gets it (empty: it opens with the system). A
  `file:` URL never loads (a file dropped on a page that doesn't take it would replace it), and
  iframes load what they like.
- `onFileDrop` gives the paths of files dropped on a window and where, in CSS pixels. The page
  gets its own drop event too (with `File` objects, which have no paths), except on GTK 4, which
  gives the files to one of the two.
- The clipboard on Wayland: the compositor may ignore an app the person hasn't just used (a
  click or key in it), so `setClipboardText` from a timer, say, may not reach other programs.
- `singleInstance`: a launch while the app runs hands its arguments and working folder to it
  (`onSecondInstance`; empty: the main window is shown) and ends in the `App` constructor
  (`std::exit(0)`), before any window opens. Per user: a Unix-domain socket in the user's
  runtime folder (Linux) or temporary folder (macOS), a named pipe on Windows.
- **The web engine.** `app.webEngine()` names the engine the pages run in and its version, for
  an About box or a bug report: `WebKitGTK` 2.50.1 on Linux (the library loaded at run time),
  `WebView2` 131.0.2903.86 on Windows (the installed runtime, so the Chromium version), `WebKit`
  621.1.15 on macOS (the system's). On Linux its `variant` tells which WebKitGTK the program is
  built for, `API 6.0, GTK 4` or `API 4.1, GTK 3` (the same engine version comes in both); it is
  empty elsewhere. UI thread only. On Windows the version is known once `run()` has started
  WebView2: in any handler, and from `onConnect` on.
- C: `single_instance`, `uniapi_on_external_url`, `uniapi_on_file_drop`,
  `uniapi_window_on_file_drop`, `uniapi_on_second_instance`, `uniapi_clipboard_text`,
  `uniapi_set_clipboard_text`, `uniapi_open_url`, `uniapi_open_path`, `uniapi_reveal_path`,
  `uniapi_web_engine`.

### Coroutines (C++20, `include/uniapi/coro.hpp`)

Sequences of asynchronous calls read top to bottom with `co_await`; the library stays C++17,
only code including `coro.hpp` needs C++20. The same flow both ways (ask for a name, confirm,
delete files on a worker thread, tell the page). With callbacks:

```cpp
app.call("askName", json::array(), [&](const uniapi::Result& name) {
    if (!name.ok) return;
    app.call("confirm", json::array({"Delete " + name.value.get<std::string>() + "?"}),
             [&](const uniapi::Result& ok) {
        if (!ok.ok || !ok.value.get<bool>()) return;
        std::thread([&] {
            DeleteFiles();
            app.post([&] { app.call("showMessage", json::array({"Deleted"})); });
        }).detach();
    });
});
```

With a coroutine:

```cpp
#include <uniapi/coro.hpp>

uniapi::Task<> deleteFlow(uniapi::App& app) {
    auto name = co_await app.callAsync("askName");
    if (!name.ok) co_return;
    auto ok = co_await app.callAsync("confirm", {"Delete " + name.value.get<std::string>() + "?"});
    if (!ok.ok || !ok.value.get<bool>()) co_return;
    co_await uniapi::switchToBackground();   // the rest runs on a worker thread
    DeleteFiles();
    co_await app.switchToUi();               // back before touching the App
    co_await app.callAsync("showMessage", {"Deleted"});
}

app.on("delete-clicked", [&app](const uniapi::json&) { deleteFlow(app); });   // start it

// An exposed coroutine answers the page's call when it finishes (parameters by value).
app.expose("slowAdd", [&app](int a, int b) -> uniapi::Task<int> {
    co_await app.sleep(std::chrono::milliseconds(100));   // doesn't block the window
    co_return a + b;
});
```

Both styles run equally fast and mix freely in one app. Use coroutines for flows with several
waits in a row; for simple handlers, events and stream frames the plain style is just as
clear. Either way, anything captured by reference must still exist when the code resumes.

### C (`include/uniapi/uniapi.h`)

The same library for C99 and for languages with C bindings: opaque handles, callbacks with a
`void* user`, JSON as text (with small `uniapi_json_*` helpers), binary data through
`uniapi_bytes`, and errors in `uniapi_last_error()` instead of exceptions. Link `uniapi` as
from C++; `examples/hello_c/main.c` is the hello example in C.

```c
static void add(void* user, const char* args, const uniapi_bytes* bytes, uniapi_reply* reply) {
    double a = 0, b = 0;
    uniapi_json_get_number(args, "/0", &a);
    uniapi_json_get_number(args, "/1", &b);
    char result[64];
    snprintf(result, sizeof result, "%g", a + b);
    uniapi_reply_resolve(reply, result);
}
// ... uniapi_app* app = uniapi_app_new(&options); uniapi_expose(app, "add", add, NULL); uniapi_run(app);
```

More windows from C: `uniapi_open_window(app, &options)` (with `uniapi_window_options_init`)
gives a `uniapi_window*`; `uniapi_window_navigate`, `_expose`, `_on`, `_emit`, `_call`,
`_on_connect`, `_on_disconnect`, `_on_closed`, `_close`, `_stream_create`, `_send_dom`,
`_set_title`, `_set_size`, ... act on it, and `uniapi_main_window(app)` is the main window as
one. `uniapi_stream_add_window(stream, window)` / `_remove_window` send a stream to more windows.

### JavaScript (`js/uniapi.js`, served at `/uniapi/uniapi.js`)

```html
<script src="/uniapi/uniapi.js"></script>
<script>
    const app = uniapi.connect({ page: 'main' });           // the name C++ sees in onConnect

    const sum = await app.native.add(1, 2);                  // calls a C++ function
    app.expose({ confirm: (text) => window.confirm(text) }); // C++ can call these
    app.unexpose('confirm');                                 // no longer callable
    app.on('progress', ({ done, total }) => { /* ... */ });  // events from C++
    app.off('progress', handler);                            // stops a handler (none given: all)
    app.emit('clicked', { id: 3 });                          // events to C++
    app.stream('frames', (bytes, meta) => { /* ... */ });    // large data from C++
    app.unstream('frames');                                  // stops receiving it
    const small = await app.native.thumbnail(file, 200);     // binary data both ways (Uint8Array back)
</script>
```

Register handlers right after `connect()`, in the same task: C++ only starts calling once
the page is ready, which can't happen before your code yields. `app.ready` is a promise for
that moment, and `app.capabilities.sharedMemory` says which transfer mode is active.

The window never appears empty or white: it stays invisible until its first page has loaded.
A page that builds itself after loading (it asks C++ for something first, or has the window
resized) sets `AppOptions::showWhenReady` and calls `app.show()` once it is drawn (C++:
`App::show()`, C: `show_when_ready`, `uniapi_show`); after 5 seconds the window appears anyway
(3 seconds without `showWhenReady`; on Linux both count from the page's load, so WebKit's slow
first start after a reboot doesn't use them up).
On Windows the window is cloaked meanwhile. On Linux the window opens with its page hidden (the
window's plain background shows), or, for GTK 3 on X11 with a compositor such as KWin, wholly
transparent; the page appears once it has drawn a frame after loading. On macOS the window is
on screen but fully transparent until then.

### Large data: streams

A stream is a pool of equally sized slots. C++ fills a slot and publishes it; the page
receives the bytes and hands the slot back when its handler returns (or its promise settles).

```cpp
uniapi::Stream& frames = app.stream("frames", 1920 * 1080 * 4, /*slots=*/3);
uniapi::Stream::Slot slot;
if (frames.acquire(slot)) {                  // false: every slot is with the page; drop or wait
    Render(slot.data);
    frames.publish(slot, 1920 * 1080 * 4, {{"width", 1920}, {"height", 1080}});
}
frames.onSlotFreed = [&] { /* a slot came back after acquire() failed */ };
```

How the bytes get to the page depends on the platform. The API is the same either way:

| Mode | When | Copies |
|---|---|---|
| Shared memory | Windows with WebView2 Runtime 1.0.1661+; Linux with the web process extension | none: the page reads the slot itself |
| Fetch | macOS always; otherwise when shared memory isn't available, or with `AppOptions::sharedMemory = false` | the page fetches `/uniapi/stream/<name>/<slot>`: 1–3 copies inside the engine, no base64 |

Shared memory is used only when it really works. If a page's shared regions don't arrive
within 3 seconds (`uniapi.connect({ regionTimeout })`), it fetches instead of hanging: every
stream if this happens while connecting, otherwise just the stream concerned.
`App::sharedMemory()` and `Stream::shared()` report the mode actually in use.

A stream can reach more windows' pages than its own (`stream.addWindow(window)`; see
[More windows](#more-windows)): one set of slots, each page told of every frame, each slot free
again once all of them released it.

The handler's `bytes` are valid only until it returns; copy what you keep (`bytes.slice()`).

### Binary data in calls and events

From the page, pass binary values (`ArrayBuffer`, typed arrays, `DataView`, `Blob`/`File`) to
`app.native.fn(...)` or `app.emit(...)`, or return them from an exposed function, anywhere in
the value, e.g. `app.emit('dropped', { name: file.name, data: file })`. Each one travels
outside the JSON as a POST to the app (no base64), and the message follows once the data has
arrived; messages still reach C++ in the order they were sent. In C++:

- a `uniapi::Bytes` parameter takes it directly (`[](uniapi::Bytes data, int width)`);
- from JSON, `value.get<uniapi::Bytes>()` works until the call is answered, or until an event
  handler (or a `call()`'s `done`) returns;
- a `Bytes` you keep stays valid: it is a shared, read-only buffer, cheap to copy and safe to
  hand to another thread (`data()`, `size()`, `begin()`/`end()`, `vector()`).

From C++, put a `uniapi::Bytes` in any JSON you send (a return value, `Reply::resolve`,
`App::call` arguments, `App::emit` data): the page gets a `Uint8Array` there, fetched once
before its code sees the message, again in order.

```cpp
app.expose("thumbnail", [](uniapi::Bytes image, int width) {
    return uniapi::Bytes(MakeThumbnail(image, width));   // from a std::vector<uint8_t>
});
```

Measured on Linux: about 570 MB/s from the page to C++ for large data (8 MB per call in
~14 ms), and ~0.4 ms per call for small data. For a steady flow of frames from C++, use a
stream (below).

### Changing the page from C++: DOM command lists (`include/uniapi/dom.hpp`)

C++ can't touch the page's elements directly; they live in the browser's process. Instead it
sends a compact binary list of commands, which `uniapi.js` applies in one go, so the browser
restyles and repaints once per list:

```cpp
app.sendDom([](uniapi::DomCommandWriter& dom) {
    dom.Create(1, "p");                          // element IDs are chosen by C++
    dom.SetText(1, "Hello from C++");
    dom.SetStyle(1, "color", "#4c8dff");
    dom.Append(uniapi::DomCommandWriter::kBody, 1);  // ID 0 is document.body
});
```

| Command | Effect |
|---|---|
| `Bind(id, selector)` | Give an existing element (first match of a CSS selector) an ID |
| `Create(id, tag)` | Create a new, detached element |
| `Append(parent, child)` | Move or insert `child` as the last child of `parent` |
| `Remove(id)` | Detach the element and forget its ID |
| `Clear(id)` | Remove all children |
| `SetText(id, text)` | Replace the text content |
| `SetAttribute(id, name, value)` | Set an attribute, e.g. `class` |
| `SetStyle(id, property, value)` | Set a CSS property, e.g. `height`, `50%` |
| `ToggleClass(id, name, on)` | Add or remove a class |

Lists travel through a built-in stream, so they use shared memory too where available.
Commands use `setAttribute`, `textContent` and `style`, never HTML parsing, but C++ is fully
trusted: an attribute such as `onclick` would run script.

## The demo

`build\uniapi_stream.exe` streams an animated test pattern to a canvas and measures it.

- **Stats** (or the **S** key) shows the Native stats overlay, which C++ builds and updates
  entirely with DOM command lists.
- **Controls** opens a settings page: resolution, mode, draw method, slot count and the
  overlay. **Apply & run** applies them live. The page also keeps a run history, which
  **Save results…** writes to a CSV file.
- Applied settings are saved to `uniapi_stream.conf` next to the exe; command-line
  options override them for one run:

| Option | Description |
|---|---|
| `--size=WxH` | Frame resolution, e.g. `--size=1920x1080` (up to 16384 per side). Presets: `360p`, `720p`, `1080p`, `1440p`, `4k`, `8k`. |
| `--timed` | About 60 fps, paced by a timer (the default mode). |
| `--benchmark` | Unthrottled: a new frame as soon as the page hands a slot back. |
| `--benchmark=transport` | Unthrottled, without rendering or drawing: measures the hand-off alone. |
| `--slots=N` | Stream slots, 1–64 (default 3). All slots together are limited to 4 GB. |
| `--draw=MODE` | `2d` (default, GPU canvas), `2d-cpu` (canvas in CPU memory) or `webgl`. |
| `--stats=on\|off` | Show the overlay (default `off`). |
| `--no-shared-memory` | Use the fetch fallback even where shared memory works. |

The window title shows fps, time per frame, throughput, the C++ render time, the page's draw
time and the transfer mode. What the pages call is listed at the top of
`examples/uniapi_stream/main.cpp`.

## Performance

Measured with the demo on an NVIDIA RTX 4070 Ti SUPER, `--draw=2d`. Windows 11 with WebView2
Runtime 156:

| | Shared memory | Fetch fallback |
|---|---|---|
| 1280×720 (`--benchmark`) | ~505 fps | ~29 fps |
| 1920×1080 | ~250 fps (~268 with `--draw=webgl`) | ~15 fps |
| 3840×2160 | ~70 fps | ~4 fps |
| 7680×4320 | ~17 fps | — |
| Hand-offs only, 1080p (`--benchmark=transport`) | ~28,000/s (3 slots), ~29,600/s (8 slots) | ~19/s |

Linux (Manjaro, KDE Plasma on Wayland, WebKitGTK 2.52):

| | GTK 3, shared memory | GTK 4, shared memory | GTK 3, fetch |
|---|---|---|---|
| 1280×720 (`--benchmark`) | ~388 fps (~449 with `--draw=webgl`) | ~437 fps (~509 with `--draw=webgl`) | ~76 fps |
| 1920×1080 | ~194 fps (~208 with `--draw=webgl`) | ~182 fps (~225 with `--draw=webgl`) | ~33 fps |
| Hand-offs only, 1080p (`--benchmark=transport`) | ~16,700/s (3 slots) | ~17,700/s (3 slots) | — |

macOS 26.5 (MacBook, Apple M4, built-in display), WKWebView, fetch (no shared memory):

| | Fetch |
|---|---|
| 1280×720 (`--benchmark`) | ~1,270 fps |
| 1920×1080 | ~573 fps (~336 with `--draw=webgl`) |
| 3840×2160 | ~142 fps |
| Hand-offs only, 1080p (`--benchmark=transport`) | ~1,060/s (3 slots) |

That is about 4.7 GB/s through the custom scheme: the app hands WebKit each slot without a
copy, and on Apple silicon the page's process reads it from the same memory.

GTK 4 numbers are with GTK's OpenGL renderer, which uniapi selects (`GSK_RENDERER=gl`, `ngl` before GTK 4.18, unless
you set the variable yourself). With GTK's default Vulkan renderer, each web frame took about
one display refresh to reach the screen on NVIDIA + Wayland, the page waited for it, and
unthrottled drawing ran at roughly half these rates.

- With shared memory, the limit is the work on the pixels (the C++ test pattern and the
  page's draw), not the transfer.
- The fetch fallback reaches about 140 MB/s on WebView2. WebView2 reads each 8 MB response
  in ~11 ms; the rest is Chromium delivering it to the page. That's fine for UI-sized data
  (a 1 MB image takes ~7 ms) and too slow for full-HD video. WebKitGTK's fetch is about twice
  as fast (~0.28 GB/s).

## Project layout

```
include/uniapi/          public headers: uniapi.hpp, dom.hpp, assets.hpp; coro.hpp (C++20); uniapi.h (C)
src/core/                platform-independent core; the protocol is described in app.cpp
src/c/                   the C API over the C++ library
src/platform/windows/    WebView2 backend
src/platform/linux/      WebKitGTK backend; extension/ is the web process extension (shared memory)
src/platform/macos/      WKWebView backend (Objective-C++)
js/uniapi.js             page library (one file for every platform)
cmake/                   uniapi_embed(), uniapi_install(), uniapi_icon() (icon and details)
tools/embed/             uniapi_embed_tool: writes the embedded files' C++ at build time
examples/hello/          smallest example (C++); its page is shared with examples/hello_c/ (C)
examples/uniapi_stream/  demo app and its pages
examples/uniapi_logo/    the logo's versions and sizes on changing backgrounds
examples/uniapi_notify/  a notification from C++ (the app's name and icon as the desktop shows them)
examples/uniapi_tray/    an icon in the tray with a menu; closing the window hides it
doc/logo/                the logo: make.py (its source), SVGs (uniapi-name*.svg: with its name), PNGs, uniapi.ico, colors.json, name.json/.js, name-fit.js
tests/selftest/          API test (uniapi_test), C++20 (run with ctest)
tests/c_api/             C API test, in C (run with ctest)
tests/windows/           windows test: further windows (run with ctest)
tests/lifecycle/         lifecycle test: hiding, showing, closing, the developer tools (run with ctest)
tests/desktop/           desktop test: clipboard, links, drops, one instance, the .exe's details (run with ctest)
tests/icon/              icon test: what uniapi_icon() writes into a Windows program (cmake -P, run with ctest)
tests/placement/         placement test: position, size, state, remembered (run with ctest)
tests/trayicon/          tray test: icons, menus, removal, the C API (run with ctest)
tests/strict/            strict calls test: strictCalls on and off (run with ctest)
tests/modal/             modal test (Linux): a modal window keeps clicks from the main window
tray/                    the tray: uniapi_tray, linked by apps that use it (tray.hpp, tray.h)
tests/checker/           checker app: the checks by hand (dialogs, alert, window), C++
tests/checker_c/         the same through the C API
tests/latency/           latency probe: messages each way, C++'s answer to a click
build.ps1, build.sh      build, clean, test, install and uninstall (Windows; Linux and macOS)
doc/api.html             API guide and reference with diagrams
```

## Limitations

- Windows, Linux and macOS 14+. macOS: no zero-copy streams (fetch only), and
  `keepAspectRatio`, title bars, icons and quitting work the Mac way (see above).
- Linux: zero-copy turns Gigacage off in the web process (see Requirements).
- Calls are asynchronous; there are no synchronous calls in either direction.

## License

uniapi is licensed under the [Apache License, Version 2.0](LICENSE).
