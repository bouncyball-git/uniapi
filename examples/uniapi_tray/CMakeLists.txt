# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# The tray: the app's icon in the system tray with a menu; closing the window hides it there.
# Links uniapi_tray, the part only apps with a tray carry.
add_executable(uniapi_tray_example WIN32 main.cpp)  # the program is uniapi_tray (uniapi_tray: the library)
uniapi_icon(uniapi_tray_example)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_tray_example PROPERTIES
    OUTPUT_NAME uniapi_tray
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_tray_example PRIVATE uniapi uniapi_tray)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_tray_example PRIVATE /ENTRY:mainCRTStartup)
endif()
uniapi_embed(uniapi_tray_example tray_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
uniapi_embed(uniapi_tray_example tray_icons "${CMAKE_CURRENT_SOURCE_DIR}/icon")
uniapi_install(uniapi_tray_example)
