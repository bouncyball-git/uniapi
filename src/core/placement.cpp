// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// Window placements as text, fitted to the screens there are, and remembered between runs in one
// file per program: <config>/<program>/uniapi-placement.json (Linux: $XDG_CONFIG_HOME or
// ~/.config; macOS: ~/Library/Application Support; Windows: %AppData%).

#include "core/placement.hpp"

#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace uniapi::detail {

namespace fs = std::filesystem;

std::string PlacementText(const SavedPlacement& p) {
    const json text = {{"x", p.x},           {"y", p.y},
                       {"width", p.width},   {"height", p.height},
                       {"hasPosition", p.hasPosition}, {"maximized", p.maximized}};
    return text.dump();
}

namespace {

std::optional<SavedPlacement> FromJson(const json& value) {
    if (!value.is_object()) return std::nullopt;
    try {
        SavedPlacement p;
        p.x = value.value("x", 0);
        p.y = value.value("y", 0);
        p.width = value.value("width", 0);
        p.height = value.value("height", 0);
        p.hasPosition = value.value("hasPosition", false);
        p.maximized = value.value("maximized", false);
        if (p.width <= 0 || p.height <= 0) return std::nullopt;
        return p;
    } catch (...) {
        return std::nullopt;  // a field of the wrong type
    }
}

json ToJson(const SavedPlacement& p) { return json::parse(PlacementText(p)); }

}  // namespace

std::optional<SavedPlacement> ParsePlacement(std::string_view text) {
    const json value = json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return FromJson(value);
}

SavedPlacement FitPlacement(SavedPlacement p, const std::vector<Rect>& screens, bool keepPosition) {
    if (screens.empty()) {
        if (!keepPosition) p.hasPosition = false;
        return p;
    }
    // The screen it is on: the one with the frame's top-left part (where the title bar is).
    constexpr int kGrab = 48;  // logical pixels of title bar that must be on a screen to drag it
    const Rect* on = nullptr;
    if (p.hasPosition) {
        for (const Rect& s : screens) {
            if (p.x + kGrab > s.x && p.x < s.x + s.width - kGrab && p.y >= s.y && p.y < s.y + s.height - kGrab) {
                on = &s;
                break;
            }
        }
    }
    if (!on) p.hasPosition = false;
    const Rect& screen = on ? *on : screens.front();
    p.width = std::min(p.width, screen.width);
    p.height = std::min(p.height, screen.height);
    return p;
}

namespace {

std::mutex placementsMutex;

fs::path ConfigDirectory() {
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA"); appData && *appData) return fs::path(appData);
    return {};
#else
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    if (home && *home) return fs::path(home) / "Library" / "Application Support";
    return {};
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config == '/') return fs::path(config);
    if (home && *home) return fs::path(home) / ".config";
    return {};
#endif
#endif
}

fs::path PlacementsFile() {
    const fs::path config = ConfigDirectory();
    if (config.empty()) return {};
    fs::path program = fs::u8path(executablePath()).filename();
    if (program.extension() == ".exe" || program.extension() == ".EXE") program.replace_extension();
    if (program.empty()) program = "uniapi";
    return config / program / "uniapi-placement.json";
}

json ReadPlacements(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return json::object();
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json value = json::parse(text, nullptr, false);
    return value.is_object() ? value : json::object();
}

}  // namespace

std::optional<SavedPlacement> RememberedPlacement(const std::string& name) {
    std::lock_guard<std::mutex> lock(placementsMutex);
    const fs::path file = PlacementsFile();
    if (file.empty()) return std::nullopt;
    const json placements = ReadPlacements(file);
    auto it = placements.find(name);
    return it == placements.end() ? std::nullopt : FromJson(*it);
}

void RememberPlacement(const std::string& name, const SavedPlacement& placement) {
    std::lock_guard<std::mutex> lock(placementsMutex);
    const fs::path file = PlacementsFile();
    if (file.empty()) return;
    json placements = ReadPlacements(file);
    placements[name] = ToJson(placement);
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    // Whole or not at all: a file of its own, then put in place.
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << placements.dump(2);
        if (!out.flush()) return;
    }
    fs::rename(temp, file, error);
    if (error) fs::remove(temp, error);
}

}  // namespace uniapi::detail
