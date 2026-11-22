// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A compressed embedded file's bytes for one use (src/core/assets.cpp).

#include <uniapi/assets.hpp>

#include <memory>
#include <vector>

namespace uniapi::detail {

// A compressed file (asset.packedSize > 0) inflated into memory of its own: nothing is kept,
// the bytes go when the last copy of the pointer does. Null if it can't be inflated (a damaged
// build). Safe from any thread.
std::shared_ptr<const std::vector<unsigned char>> InflateAsset(const Asset& asset);

}  // namespace uniapi::detail
