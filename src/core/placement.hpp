// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Window placements kept as text (Window::placement) and between runs (rememberPlacement): see
// placement.cpp.

#include "core/backend.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uniapi::detail {

// What a placement keeps: the normal bounds, and whether the window was maximised. Minimised and
// full screen are left out: a window comes back normal (or maximised).
struct SavedPlacement {
    int x = 0, y = 0, width = 0, height = 0;
    bool hasPosition = false;
    bool maximized = false;
};

std::string PlacementText(const SavedPlacement& placement);  // {"x":..,"y":..,...}
std::optional<SavedPlacement> ParsePlacement(std::string_view text);  // nothing: not a placement

// Keeps the placement on the screens there are: no larger than the screen it is on (or the
// first), its title bar's top-left part on a screen, else no position (centred). Without
// screens: no position, unless keepPosition (the window not open yet: the backend checks it then).
SavedPlacement FitPlacement(SavedPlacement placement, const std::vector<Rect>& screens, bool keepPosition);

// The placements remembered, by name, in <config>/<program>/uniapi-placement.json. Safe from any
// thread.
std::optional<SavedPlacement> RememberedPlacement(const std::string& name);
void RememberPlacement(const std::string& name, const SavedPlacement& placement);

}  // namespace uniapi::detail
