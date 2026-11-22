// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// Linux side of the desktop test: the clipboard as other programs see it, through wl-copy and
// wl-paste on Wayland, xclip on X11. Drops: not here (GTK takes them from the display server).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Whether the app is on Wayland (not GTK's X11 backend inside a Wayland session, whose clipboard
// XWayland hands to Wayland only while an X window has focus).
bool Wayland() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    const char* backend = std::getenv("GDK_BACKEND");
    return display && *display && !(backend && std::string(backend).rfind("x11", 0) == 0);
}

bool Have(const char* tool) { return std::system((std::string("command -v ") + tool + " >/dev/null 2>&1").c_str()) == 0; }

}  // namespace

bool Drop(const std::string&, const std::vector<std::string>&, int, int) { return false; }

bool HasSystemClipboard() { return Wayland() ? Have("wl-paste") && Have("wl-copy") : Have("xclip"); }

bool SystemClipboardGet(std::string& text) {
    FILE* pipe = popen(Wayland() ? "wl-paste --no-newline 2>/dev/null" : "xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe) return false;
    text.clear();
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) text.append(chunk, n);
    return pclose(pipe) == 0;
}

// wl-copy and xclip leave a process behind serving the clipboard: its output goes nowhere, so
// it holds no one's pipe open (ctest's would wait for it).
bool SystemClipboardSet(const std::string& text) {
    FILE* pipe = popen(Wayland() ? "wl-copy >/dev/null 2>&1" : "xclip -selection clipboard >/dev/null 2>&1", "w");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
}
