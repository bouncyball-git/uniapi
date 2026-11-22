# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# The self-test: a console program that drives a real web view through calls, events, binary
# data, coroutines, streams, DOM command lists and page reloads; the exit code is 0 when every
# check passes.
# Started directly it keeps the window open with the results; ctest passes --exit so each run
# closes by itself without taking keyboard focus.
add_executable(uniapi_test selftest/main.cpp)
uniapi_icon(uniapi_test)
set_target_properties(uniapi_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_test PRIVATE uniapi)
# C++20 for the coroutine checks (uniapi/coro.hpp); the library itself and hello stay C++17.
target_compile_features(uniapi_test PRIVATE cxx_std_20)
# COMPRESS: compression is off by default, and this test checks it (a compressed file comes back
# whole, read and served).
uniapi_embed(uniapi_test selftest_web "${CMAKE_CURRENT_SOURCE_DIR}/selftest/web" COMPRESS)
# Whether its files were compressed (only the embed tool does it): the checks of embedded files
# expect it then.
if(TARGET uniapi_embed_tool)
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_COMPRESSED=1)
else()
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_COMPRESSED=0)
endif()
# ...and minified (esbuild with UNIAPI_EMBED_MINIFY).
if(TARGET uniapi_embed_tool AND UNIAPI_EMBED_MINIFY AND UNIAPI_ESBUILD_EXECUTABLE)
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_MINIFIED=1)
else()
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_MINIFIED=0)
endif()
# ...and its .html, .svg and .json minified (tdewolff/minify).
if(TARGET uniapi_embed_tool AND UNIAPI_EMBED_MINIFY AND UNIAPI_MINIFY_EXECUTABLE)
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_MINIFIED_MARKUP=1)
else()
    target_compile_definitions(uniapi_test PRIVATE UNIAPI_TEST_EMBED_MINIFIED_MARKUP=0)
endif()

add_test(NAME api.shared-memory COMMAND uniapi_test --exit)
add_test(NAME api.fetch COMMAND uniapi_test --no-shared-memory --exit)

# The windows test: App::openWindow against real web views (further windows' pages, their calls,
# events, binary data and streams, one stream reaching two windows, a modal window, closing).
# On Linux the modal check can't look at the main window's input (taken as passed there: the
# modal test below checks it with clicks);
# on macOS it asks Cocoa whether the main window may take input (windows/mac.mm).
add_executable(uniapi_windows_test windows/main.cpp)
if(APPLE)
    target_sources(uniapi_windows_test PRIVATE windows/mac.mm)
    set_source_files_properties(windows/mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
uniapi_icon(uniapi_windows_test)
set_target_properties(uniapi_windows_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_windows_test PRIVATE uniapi)
target_compile_features(uniapi_windows_test PRIVATE cxx_std_20)  # its flow is a coroutine
uniapi_embed(uniapi_windows_test windows_web "${CMAKE_CURRENT_SOURCE_DIR}/windows/web")
add_test(NAME windows.shared-memory COMMAND uniapi_windows_test --exit)
add_test(NAME windows.fetch COMMAND uniapi_windows_test --no-shared-memory --exit)

# The lifecycle test: windows hidden and shown again (startHidden, hide, show, visible) and
# closing the app decides (onCloseRequested): the user closing a window, by the platform's own
# close (WM_CLOSE, gtk_window_close, performClose:), and on macOS Quit (Cmd+Q); and openDevTools
# opening the Web Inspector's window (Linux; n/a elsewhere). Its helpers find
# the windows by title: lifecycle/linux.cpp (GTK), lifecycle/mac.mm, Win32 in main.cpp.
add_executable(uniapi_lifecycle_test lifecycle/main.cpp)
if(APPLE)
    target_sources(uniapi_lifecycle_test PRIVATE lifecycle/mac.mm)
    set_source_files_properties(lifecycle/mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(UNIX)
    target_sources(uniapi_lifecycle_test PRIVATE lifecycle/linux.cpp)
    target_link_libraries(uniapi_lifecycle_test PRIVATE ${uniapi_webkit})
endif()
uniapi_icon(uniapi_lifecycle_test)
set_target_properties(uniapi_lifecycle_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_lifecycle_test PRIVATE uniapi)
target_compile_features(uniapi_lifecycle_test PRIVATE cxx_std_20)  # its flow is a coroutine
uniapi_embed(uniapi_lifecycle_test lifecycle_web "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/web")
add_test(NAME lifecycle COMMAND uniapi_lifecycle_test --exit)
set_tests_properties(lifecycle PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# The desktop test: what crosses between the app and the desktop. The clipboard both ways (as
# other programs see it: wl-copy/wl-paste or xclip on Linux, pbcopy/pbpaste, PowerShell), links
# and new windows to other sites going outside (onExternalURL), a file: navigation refused, files
# dropped on the window (macOS, through a hook in desktop/mac.mm), and a second launch handed to
# the running app (singleInstance: the test starts itself again).
add_executable(uniapi_desktop_test desktop/main.cpp)
if(APPLE)
    target_sources(uniapi_desktop_test PRIVATE desktop/mac.mm)
    set_source_files_properties(desktop/mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(UNIX)
    target_sources(uniapi_desktop_test PRIVATE desktop/linux.cpp)
endif()
# Its details (Windows: Properties > Details), which it reads back from its .exe (version.lib).
uniapi_icon(uniapi_desktop_test NAME "uniapi desktop test" VERSION 1.2.3 COMPANY "uniapi"
            COPYRIGHT "© uniapi" COMMENTS "The desktop test's \"details\"")
set_target_properties(uniapi_desktop_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_desktop_test PRIVATE uniapi)
if(WIN32)
    target_link_libraries(uniapi_desktop_test PRIVATE version)
endif()
target_compile_features(uniapi_desktop_test PRIVATE cxx_std_20)  # its flow is a coroutine
uniapi_embed(uniapi_desktop_test desktop_web "${CMAKE_CURRENT_SOURCE_DIR}/desktop/web")
add_test(NAME desktop COMMAND uniapi_desktop_test --exit)
set_tests_properties(desktop PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# The icon test: what uniapi_icon() writes into a Windows program (the resource script for the
# icon and the program's details, which versions it takes), on every platform; compiled with
# MinGW's windres where that is installed. No window.
add_test(NAME icon COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/icon/check.cmake")

# The embed test: uniapi_embed()'s BUNDLE, by running the embed tool on tests/embed/web: entries
# bundled with what they import, the parts not embedded by themselves. No window. Where the
# tool and an esbuild are there (not when cross-compiling).
if(TARGET uniapi_embed_tool AND UNIAPI_ESBUILD_EXECUTABLE)
    add_test(NAME embed COMMAND ${CMAKE_COMMAND} "-DTOOL=$<TARGET_FILE:uniapi_embed_tool>" "-DESBUILD=${UNIAPI_ESBUILD_EXECUTABLE}"
                                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/embed-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed/check.cmake")
endif()

# The placement test: where a window is and how (size, position, centring, minimised, maximised
# and its normal size, on top, onPlacementChanged), a placement put back (and one off every screen
# landing on one), and a window's placement remembered between openings. Where windows can't be
# placed (Linux: Wayland, GTK 4) the checks needing a position are n/a.
add_executable(uniapi_placement_test placement/main.cpp)
uniapi_icon(uniapi_placement_test)
set_target_properties(uniapi_placement_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_placement_test PRIVATE uniapi)
target_compile_features(uniapi_placement_test PRIVATE cxx_std_20)  # its flow is a coroutine
if(uniapi_gtk_found STREQUAL "3")
    target_compile_definitions(uniapi_placement_test PRIVATE UNIAPI_TEST_GTK3=1)
endif()
uniapi_embed(uniapi_placement_test placement_web "${CMAKE_CURRENT_SOURCE_DIR}/placement/web")
add_test(NAME placement COMMAND uniapi_placement_test --exit)
set_tests_properties(placement PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# The C API test: a C program (compiled as C, which also checks that uniapi.h is valid C)
# driving a page through uniapi.h; exits with 0 when every check passes. Like the self-test,
# started directly it keeps the window open with the results; ctest passes --exit.
add_executable(uniapi_test_c c_api/main.c)
uniapi_icon(uniapi_test_c)
set_target_properties(uniapi_test_c PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} C_STANDARD 99)
target_link_libraries(uniapi_test_c PRIVATE uniapi)
uniapi_embed(uniapi_test_c c_api_web "${CMAKE_CURRENT_SOURCE_DIR}/c_api/web")
add_test(NAME c-api.shared-memory COMMAND uniapi_test_c --exit)
add_test(NAME c-api.fetch COMMAND uniapi_test_c --no-shared-memory --exit)

set_tests_properties(api.shared-memory api.fetch c-api.shared-memory c-api.fetch
                     PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

# The checker: the checks that need a person (dialogs, window options, icon, full screen, a
# dialog before run()), one button each. Built next to the tests; not run by ctest.
add_executable(uniapi_checker checker/main.cpp)
uniapi_icon(uniapi_checker)
set_target_properties(uniapi_checker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_checker PRIVATE uniapi)
uniapi_embed(uniapi_checker checker_web "${CMAKE_CURRENT_SOURCE_DIR}/checker/web")

# The C checker: the same by hand through the C API (uniapi_alert and the dialogs are the C
# functions the C API test can't call unattended). Compiled as C; not run by ctest.
add_executable(uniapi_checker_c checker_c/main.c)
uniapi_icon(uniapi_checker_c)
set_target_properties(uniapi_checker_c PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} C_STANDARD 99)
target_link_libraries(uniapi_checker_c PRIVATE uniapi)
uniapi_embed(uniapi_checker_c checker_c_web "${CMAKE_CURRENT_SOURCE_DIR}/checker_c/web")

# The latency probe: how long messages take between C++ and the page (each way, idle and busy)
# and how long C++'s answer to a click takes to come back. A measure to run by hand (--exit:
# the messages part only, printed); not run by ctest.
add_executable(uniapi_latency latency/main.cpp)
uniapi_icon(uniapi_latency)
set_target_properties(uniapi_latency PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_latency PRIVATE uniapi)
uniapi_embed(uniapi_latency latency_web "${CMAKE_CURRENT_SOURCE_DIR}/latency/web")

# The tray test: icons in the real tray (macOS's menu bar, Windows' notification area, the Linux
# host's StatusNotifierWatcher), their menus chosen through the platform where it can be driven,
# remove() and the App's end taking them away, and the C API. Without a tray (stock GNOME) it
# says so and passes: nothing to check.
add_executable(uniapi_tray_test trayicon/main.cpp)
uniapi_icon(uniapi_tray_test)
set_target_properties(uniapi_tray_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_tray_test PRIVATE uniapi uniapi_tray)
target_include_directories(uniapi_tray_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tray")
target_compile_features(uniapi_tray_test PRIVATE cxx_std_20)  # its flow is a coroutine
uniapi_embed(uniapi_tray_test trayicon_web "${CMAKE_CURRENT_SOURCE_DIR}/trayicon/web")
add_test(NAME tray COMMAND uniapi_tray_test --exit)
set_tests_properties(tray PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# The strict calls test: AppOptions::strictCalls refusing an extra argument and an unknown field
# (strict), and letting them through as before (--loose).
add_executable(uniapi_strict_test strict/main.cpp)
uniapi_icon(uniapi_strict_test)
set_target_properties(uniapi_strict_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_strict_test PRIVATE uniapi)
uniapi_embed(uniapi_strict_test strict_web "${CMAKE_CURRENT_SOURCE_DIR}/strict/web")
add_test(NAME strict COMMAND uniapi_strict_test --exit)
add_test(NAME strict.loose COMMAND uniapi_strict_test --loose --exit)
set_tests_properties(strict strict.loose PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

# The modal test (Linux): a modal window keeps clicks from the main window. GTK 3: clicks made
# inside the app (modal/linux.cpp), automatic. GTK 4 can't make events: there ctest counts it as
# skipped (exit code 3); --by-hand has you click.
if(UNIX AND NOT APPLE)
    add_executable(uniapi_modal_test modal/main.cpp modal/linux.cpp)
    uniapi_icon(uniapi_modal_test)
    set_target_properties(uniapi_modal_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(uniapi_modal_test PRIVATE uniapi ${uniapi_webkit})
    if(uniapi_gtk_define)
        target_compile_definitions(uniapi_modal_test PRIVATE ${uniapi_gtk_define})
    endif()
    target_compile_features(uniapi_modal_test PRIVATE cxx_std_20)  # its flow is a coroutine
    uniapi_embed(uniapi_modal_test modal_web "${CMAKE_CURRENT_SOURCE_DIR}/modal/web")
    add_test(NAME modal COMMAND uniapi_modal_test)
    set_tests_properties(modal PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 3)
endif()
