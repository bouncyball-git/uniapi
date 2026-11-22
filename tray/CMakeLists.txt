# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# uniapi tray: the app's icon in the system tray, with a tooltip, a menu and a click of its own
# (include/uniapi/tray.hpp; its C API include/uniapi/tray.h). An app gets it with
#   target_link_libraries(myapp PRIVATE uniapi_tray)
# and apps that don't link it carry none of it.
#   macOS:   an NSStatusItem in the menu bar (native_macos.mm)
#   Windows: a notification-area icon and popup menu, Shell_NotifyIcon (native_windows.cpp)
#   Linux:   a StatusNotifierItem and dbusmenu over GDBus (native_linux.cpp)
add_library(uniapi_tray STATIC tray.cpp tray_c.cpp)
target_link_libraries(uniapi_tray PUBLIC uniapi)
target_include_directories(uniapi_tray PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(uniapi_tray PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(APPLE)
    target_sources(uniapi_tray PRIVATE native_macos.mm)
    set_source_files_properties(native_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(uniapi_tray PRIVATE "-framework Cocoa")
elseif(WIN32)
    target_sources(uniapi_tray PRIVATE native_windows.cpp)
    target_compile_definitions(uniapi_tray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)  # as uniapi's
    target_link_libraries(uniapi_tray PRIVATE shell32 user32)
else()
    target_sources(uniapi_tray PRIVATE native_linux.cpp)
    # GDBus and GdkPixbuf: GTK's own, already in every uniapi app on Linux.
    pkg_check_modules(UNIAPI_TRAY_DEPS REQUIRED IMPORTED_TARGET gio-2.0 gdk-pixbuf-2.0)
    target_link_libraries(uniapi_tray PRIVATE PkgConfig::UNIAPI_TRAY_DEPS)
endif()
