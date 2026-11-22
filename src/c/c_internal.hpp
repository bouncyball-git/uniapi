// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the C API's parts share (uniapi_c.cpp, and any further C API built on the library):
// the app handle's layout and the error helpers. Not installed: internal to the library.

#include <uniapi/uniapi.h>
#include <uniapi/uniapi.hpp>

#include <exception>
#include <string>

struct uniapi_app {
    explicit uniapi_app(const uniapi::AppOptions& options) : app(options) {}
    uniapi::App app;
};

namespace uniapi::c_api {

// Leaves the message for uniapi_last_error() (per thread).
void SetError(std::string message);

// Runs fn; on any exception records it and returns `failure`. No C++ exception may reach C code.
template <typename R, typename F>
R Guard(R failure, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        SetError(e.what());
    } catch (...) {
        SetError("unknown C++ exception");
    }
    return failure;
}

template <typename F>
void GuardVoid(F&& fn) {
    Guard(0, [&] {
        fn();
        return 0;
    });
}

}  // namespace uniapi::c_api
