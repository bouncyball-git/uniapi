// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <uniapi/assets.hpp>

namespace uniapi {
namespace detail {

// Adds files that an optional part of uniapi brings along to what every app serves under
// /uniapi/: a request for /uniapi/<prefix><path> gets `path` from `table`. An add-on library
// registers its files from a static initializer, before main(), so linking its CMake target is
// all an app does. Not thread-safe; call only
// during static initialization. Returns true, for use as an initializer.
bool AddLibraryFiles(const char* prefix, const AssetTable* table);

}  // namespace detail
}  // namespace uniapi
