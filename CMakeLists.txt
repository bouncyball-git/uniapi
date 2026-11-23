# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.18)
# macOS: the oldest version apps run on (WKWebsiteDataStore by identifier, for
# AppOptions::dataDirectory, needs 14). Set before project(); ignored elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Oldest macOS uniapi apps run on (14 or later)")
project(uniapi LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)  # the macOS backend (src/platform/macos/backend_macos.mm)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release at -O2 with GCC or Clang on Linux and macOS, as MSVC's Release (/O2), instead of CMake's -O3:
# -O3's extra inlining, unrolling and vectorising grew nlohmann::json's templates most, for no
# speed these apps show (a larger example app, stripped: 725 KB -> 631 KB). Only for this project's
# own build, and only CMake's default value, once per build folder: a project that adds uniapi
# keeps its flags, and so do other flags given, or -O3 set again after the first configure.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
    set(langs C CXX)
    if(APPLE)
        list(APPEND langs OBJCXX)  # the macOS backend
    endif()
    foreach(lang ${langs})
        if(CMAKE_${lang}_COMPILER_ID MATCHES "GNU|Clang" AND "${CMAKE_${lang}_FLAGS_RELEASE}" STREQUAL "-O3 -DNDEBUG"
           AND NOT UNIAPI_RELEASE_FLAGS_SET_${lang})
            set(CMAKE_${lang}_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Flags used by the ${lang} compiler during RELEASE builds." FORCE)
        endif()
        # Once: a -O3 set later in the cache (ccmake, -D) is the user's and stays.
        set(UNIAPI_RELEASE_FLAGS_SET_${lang} ON CACHE INTERNAL "")
    endforeach()
endif()

option(UNIAPI_BUILD_EXAMPLES "Build the examples in examples/ (hello, hello_c, uniapi_stream, uniapi_pagestream, uniapi_logo, uniapi_notify, uniapi_tray)" ON)
option(UNIAPI_BUILD_TESTS "Build the tests in tests/ (self-test, C API test) and register them with CTest" ON)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
include(cmake/UniapiEmbed.cmake)
include(cmake/UniapiInstall.cmake)
include(cmake/UniapiIcon.cmake)

# JSON for call arguments and results (header-only, MIT).
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# zlib compression for embedded files (miniz: one C file, MIT): the embed tool compresses, the
# library inflates (src/core/assets.cpp).
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
)
FetchContent_MakeAvailable(miniz)

# --- Embedding files (cmake/UniapiEmbed.cmake) --------------------------------------------

option(UNIAPI_EMBED_COMPRESS "uniapi_embed(): store files that shrink by a tenth or more zlib-compressed, for a smaller program file (inflated each time they are served); COMPRESS does it for one call" OFF)
# .incbin: on by default with GCC or Clang on Linux, if a small program using it compiles,
# links and runs here.
set(UNIAPI_EMBED_INCBIN_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # The probe goes through a file and try_run(): through check_cxx_source_runs(), a macro, its
    # backslashes would read differently with CMake 4's policy CMP0219 (and warn).
    set(probe "${CMAKE_CURRENT_BINARY_DIR}/uniapi_incbin_probe")
    file(WRITE "${probe}.bin" "uniapi")
    file(WRITE "${probe}.cpp" "
extern \"C\" const unsigned char uniapi_incbin_probe[];
__asm__(\".section .rodata\\n.balign 16\\n.globl uniapi_incbin_probe\\n.hidden uniapi_incbin_probe\\nuniapi_incbin_probe:\\n.incbin \\\"${probe}.bin\\\"\\n.byte 0\\n.previous\\n\");
int main() { return uniapi_incbin_probe[0] == 'u' && uniapi_incbin_probe[5] == 'i' && uniapi_incbin_probe[6] == 0 ? 0 : 1; }
")
    try_run(incbin_run incbin_compiled "${probe}" "${probe}.cpp")
    set(UNIAPI_INCBIN_WORKS OFF)
    if(incbin_compiled AND incbin_run EQUAL 0)
        set(UNIAPI_INCBIN_WORKS ON)
    endif()
    if(UNIAPI_INCBIN_WORKS)
        set(UNIAPI_EMBED_INCBIN_DEFAULT ON)
    endif()
endif()
option(UNIAPI_EMBED_INCBIN "uniapi_embed(): bring the bytes in with the assembler's .incbin (GCC/Clang on Linux), not compiled C++ arrays" ${UNIAPI_EMBED_INCBIN_DEFAULT})

# Minifying .js and .css before embedding: esbuild, a single native program, pinned and fetched
# for the machine that builds (npm's package of it; the hashes are npm's own). UNIAPI_ESBUILD
# names another one instead.
option(UNIAPI_EMBED_MINIFY "uniapi_embed(): minify .js and .css files with esbuild (the engine parses less, less is stored)" ON)
set(UNIAPI_ESBUILD "" CACHE FILEPATH "esbuild to minify with (empty: the pinned 0.24.0, fetched)")
set(UNIAPI_ESBUILD_EXECUTABLE "" CACHE INTERNAL "")
if(UNIAPI_EMBED_MINIFY AND NOT CMAKE_CROSSCOMPILING)
    if(UNIAPI_ESBUILD)
        set(UNIAPI_ESBUILD_EXECUTABLE "${UNIAPI_ESBUILD}" CACHE INTERNAL "")
    else()
        string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" esbuild_cpu)
        set(esbuild_package "")
        if(CMAKE_HOST_WIN32 AND esbuild_cpu MATCHES "amd64|x86_64")
            set(esbuild_package win32-x64)
            set(esbuild_hash ec80053eb8d240826b1ac2ba7e5c20ecd166c01a124f2177ae5ec87f484d505414e22953b043cbe86b8cb94f417c8595546b919ee22276448c0be7343adbbc20)
            set(esbuild_file esbuild.exe)
        elseif(CMAKE_HOST_WIN32 AND esbuild_cpu MATCHES "arm64|aarch64")
            set(esbuild_package win32-arm64)
            set(esbuild_hash 88a73c180b25cd1a41cad3b6fda377776c9bdb3f174d57cd5743e31a50b12a8e5281698d5f1f3623f437686d6d15f4be0368a054263ded327cb67630a5458b08)
            set(esbuild_file esbuild.exe)
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND esbuild_cpu MATCHES "x86_64|amd64")
            set(esbuild_package linux-x64)
            set(esbuild_hash bdbbadb05a90f9fa32df04926e68c15d7209e8f2f7b1c82126833ccc22f5e36706699280742647c9ff81a6efcc997f734fd434cc505529bdfa31ae45cdf6bcc4)
            set(esbuild_file bin/esbuild)
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND esbuild_cpu MATCHES "aarch64|arm64")
            set(esbuild_package linux-arm64)
            set(esbuild_hash 4c38a33d74ce784dde68c911629732dcb6ab220d77752f7058745dc18467ce5c25037ef4acd759a9b729d164f2c95fe4db34b17e4a39dbe0bb8d4e45f537e3d6)
            set(esbuild_file bin/esbuild)
        elseif(CMAKE_HOST_APPLE AND esbuild_cpu MATCHES "arm64|aarch64")
            set(esbuild_package darwin-arm64)
            set(esbuild_hash 08ac83a516cad615f0bfbf6ca1e4c93476f91221ba72ddde7ddfc54cf77339675b6597c68696dca88a620f4fafc26a6ed304c82fded944f66ef2f52de3a9c14b)
            set(esbuild_file bin/esbuild)
        elseif(CMAKE_HOST_APPLE AND esbuild_cpu MATCHES "x86_64|amd64")
            set(esbuild_package darwin-x64)
            set(esbuild_hash ae0b73e9f964564879f287783f04d1ab16ca1fd70e8da5c23198160fdd3924ece314a5beec489439b7ddfca6aff80e86cae77a59993dc3ec6ee902f2b5d8b638)
            set(esbuild_file bin/esbuild)
        endif()
        if(esbuild_package)
            FetchContent_Declare(
                esbuild_bin
                URL https://registry.npmjs.org/@esbuild/${esbuild_package}/-/${esbuild_package}-0.24.0.tgz
                URL_HASH SHA512=${esbuild_hash}
            )
            FetchContent_MakeAvailable(esbuild_bin)
            set(UNIAPI_ESBUILD_EXECUTABLE "${esbuild_bin_SOURCE_DIR}/${esbuild_file}" CACHE INTERNAL "")
            if(NOT CMAKE_HOST_WIN32)
                execute_process(COMMAND chmod +x "${UNIAPI_ESBUILD_EXECUTABLE}")
            endif()
        else()
            message(STATUS "uniapi: no esbuild for ${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}; pages are embedded unminified (set UNIAPI_ESBUILD to one)")
        endif()
    endif()
endif()

# ...and .html, .svg and .json with tdewolff/minify (esbuild doesn't do those), pinned and
# fetched the same way (its GitHub release; the hashes are the release's own). No build for
# Windows on ARM64: there those files are embedded as written. UNIAPI_MINIFY names another.
set(UNIAPI_MINIFY "" CACHE FILEPATH "tdewolff/minify to minify .html, .svg and .json with (empty: the pinned 2.24.21, fetched)")
set(UNIAPI_MINIFY_EXECUTABLE "" CACHE INTERNAL "")
if(UNIAPI_EMBED_MINIFY AND NOT CMAKE_CROSSCOMPILING)
    if(UNIAPI_MINIFY)
        set(UNIAPI_MINIFY_EXECUTABLE "${UNIAPI_MINIFY}" CACHE INTERNAL "")
    else()
        string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" minify_cpu)
        set(minify_package "")
        if(CMAKE_HOST_WIN32 AND minify_cpu MATCHES "amd64|x86_64")
            set(minify_package windows_amd64.zip)
            set(minify_hash e68b2c02f0f7485f875cd961bca54fba50da68438d2233e07a47111771a09673)
            set(minify_file minify.exe)
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND minify_cpu MATCHES "x86_64|amd64")
            set(minify_package linux_amd64.tar.gz)
            set(minify_hash 53802786c0a7417d5d5d0b6043980039e17de13ab6a54f6ca5c38d27f030fbd0)
            set(minify_file minify)
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND minify_cpu MATCHES "aarch64|arm64")
            set(minify_package linux_arm64.tar.gz)
            set(minify_hash 589999def5a07536225e6058966eb470567df1c46543a26ccbdab01618d5fb95)
            set(minify_file minify)
        elseif(CMAKE_HOST_APPLE AND minify_cpu MATCHES "arm64|aarch64")
            set(minify_package darwin_arm64.tar.gz)
            set(minify_hash 036aa5fcf893e0ef1604c8c41cbbb9f74f41372782b694cfaaa36b6d2784288a)
            set(minify_file minify)
        elseif(CMAKE_HOST_APPLE AND minify_cpu MATCHES "x86_64|amd64")
            set(minify_package darwin_amd64.tar.gz)
            set(minify_hash bd13c9dba1265e67edddbea1c2be40df4986221dfe5dbcba3dbd70808836c189)
            set(minify_file minify)
        endif()
        if(minify_package)
            FetchContent_Declare(
                minify_bin
                URL https://github.com/tdewolff/minify/releases/download/v2.24.21/minify_${minify_package}
                URL_HASH SHA256=${minify_hash}
            )
            FetchContent_MakeAvailable(minify_bin)
            set(UNIAPI_MINIFY_EXECUTABLE "${minify_bin_SOURCE_DIR}/${minify_file}" CACHE INTERNAL "")
            if(NOT CMAKE_HOST_WIN32)
                execute_process(COMMAND chmod +x "${UNIAPI_MINIFY_EXECUTABLE}")
            endif()
        else()
            message(STATUS "uniapi: no tdewolff/minify for ${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}; .html, .svg and .json are embedded as written (set UNIAPI_MINIFY to one)")
        endif()
    endif()
endif()

# The embed tool runs during the build on the machine that builds: not when cross-compiling
# (uniapi_embed then falls back to a CMake script, slower and without compression).
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(uniapi_embed_tool tools/embed/embed.cpp "${miniz_SOURCE_DIR}/miniz.c")
    uniapi_icon(uniapi_embed_tool)
    target_include_directories(uniapi_embed_tool PRIVATE "${miniz_SOURCE_DIR}")
    target_compile_definitions(uniapi_embed_tool PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME
                               MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    # Built optimized whatever the build type: it runs at every embed.
    if(MSVC)
        target_compile_options(uniapi_embed_tool PRIVATE /O2)
    else()
        target_compile_options(uniapi_embed_tool PRIVATE -O2)
    endif()
endif()

# --- The library --------------------------------------------------------------------------

add_library(uniapi STATIC
    src/core/app.cpp
    src/core/assets.cpp     # embedded files, inflated when compressed
    src/core/dom.cpp
    src/core/desktop.cpp    # links, opening things, one instance
    src/core/placement.cpp  # window placements kept and remembered
    src/c/uniapi_c.cpp      # the C API, include/uniapi/uniapi.h
    "${miniz_SOURCE_DIR}/miniz.c"
)
# miniz inflates only: no archives, files, time or compression; mz_ names (no zlib ones, so an
# app may link zlib too).
target_include_directories(uniapi PRIVATE "${miniz_SOURCE_DIR}")
target_compile_definitions(uniapi PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
                           MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_include_directories(uniapi PUBLIC include PRIVATE src)
# The headers need C++17; PUBLIC passes that on to every app that links uniapi, including apps
# in projects that add uniapi with add_subdirectory/FetchContent and set no standard themselves.
target_compile_features(uniapi PUBLIC cxx_std_17)
target_link_libraries(uniapi PUBLIC nlohmann_json::nlohmann_json)
# get<T>() instead of silent implicit conversions from JSON.
target_compile_definitions(uniapi PUBLIC JSON_USE_IMPLICIT_CONVERSIONS=0)
target_compile_options(uniapi PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
# Smaller apps with GCC or Clang on Linux: every function and data item in a section of its own,
# and the linker drops the sections nothing uses: the library's, nlohmann::json's and the app's
# own unused code (MSVC's Release linker does this already, /OPT:REF). PUBLIC and INTERFACE, so
# they reach the app's own code and its link. A larger example app, stripped, at -O3: 1004 KB ->
# 725 KB, the same code that runs.
option(UNIAPI_GC_SECTIONS "GCC/Clang on Linux and macOS: drop unused functions and data from apps (Linux: -ffunction-sections -fdata-sections, --gc-sections; macOS: -dead_strip)" ON)
if(UNIAPI_GC_SECTIONS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uniapi PUBLIC -ffunction-sections -fdata-sections)
    target_link_options(uniapi INTERFACE -Wl,--gc-sections)
elseif(UNIAPI_GC_SECTIONS AND APPLE)
    # Apple's linker drops what nothing reaches by itself with -dead_strip: Mach-O objects keep
    # each function and data item apart already (subsections via symbols), no compiler flags.
    target_link_options(uniapi INTERFACE -Wl,-dead_strip)
endif()
# Release and MinSizeRel apps linked stripped (-s), as MSVC's exes carry no symbols (they go to
# a .pdb): no symbol table, which a crash report or a debugger would want (build Debug or
# RelWithDebInfo for those, or turn this off). A larger example app: 862 KB -> 725 KB.
option(UNIAPI_STRIP "GCC/Clang on Linux and macOS: link Release and MinSizeRel apps stripped (Linux -s; macOS -x, the local symbols)" ON)
set(uniapi_strip_flag "")
if(UNIAPI_STRIP AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(uniapi_strip_flag "$<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-s>")
    target_link_options(uniapi INTERFACE ${uniapi_strip_flag})
elseif(UNIAPI_STRIP AND APPLE)
    # Apple's linker has no full strip (-s is obsolete there): -x leaves out the local symbols,
    # nearly all of them (a larger example app: 1,648 of 1,958; 763 KB -> 563 KB). A full strip
    # after linking saved 10 KB more, not worth a step after every app's link. The linker still
    # signs the result (ad hoc), as arm64 needs.
    target_link_options(uniapi INTERFACE "$<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-Wl,-x>")
endif()
# The page library, served by every app at /uniapi/.
uniapi_embed(uniapi uniapi_library_js "${CMAKE_CURRENT_SOURCE_DIR}/js")

if(WIN32)
    # WebView2 SDK from NuGet. Shared buffers need SDK 1.0.1661+ (and the matching Runtime).
    set(WEBVIEW2_VERSION 1.0.4314.35)
    FetchContent_Declare(
        webview2_sdk
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}
        URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
        DOWNLOAD_NAME microsoft.web.webview2.${WEBVIEW2_VERSION}.zip
    )
    FetchContent_MakeAvailable(webview2_sdk)
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
        set(WEBVIEW2_ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WEBVIEW2_ARCH x64)
    else()
        set(WEBVIEW2_ARCH x86)
    endif()

    target_sources(uniapi PRIVATE src/platform/windows/backend_windows.cpp)
    target_include_directories(uniapi PRIVATE ${webview2_sdk_SOURCE_DIR}/build/native/include)
    target_compile_definitions(uniapi PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Static loader: no WebView2Loader.dll to ship. dwmapi: dark title bars. shcore: monitors' DPI.
    target_link_libraries(uniapi PRIVATE
        ${webview2_sdk_SOURCE_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib dwmapi shcore)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GTK + WebKitGTK. UNIAPI_GTK=auto takes GTK 4 + webkitgtk-6.0 when installed, GTK 3 +
    # webkit2gtk-4.1 otherwise; 4 or 3 insists on one. Packages: see the README.
    set(UNIAPI_GTK "auto" CACHE STRING "Linux toolkit: auto (GTK 4 if found, else 3), 4 or 3")
    set_property(CACHE UNIAPI_GTK PROPERTY STRINGS auto 4 3)
    if(NOT UNIAPI_GTK MATCHES "^(auto|4|3)$")
        message(FATAL_ERROR "UNIAPI_GTK must be auto, 4 or 3 (it is \"${UNIAPI_GTK}\").")
    endif()
    find_package(PkgConfig REQUIRED)
    set(uniapi_gtk_found "")
    if(NOT UNIAPI_GTK STREQUAL "3")
        if(UNIAPI_GTK STREQUAL "4")
            set(required REQUIRED)
        else()
            set(required QUIET)
        endif()
        # GTK 4.10+ for GtkFileDialog and GtkAlertDialog.
        pkg_check_modules(UNIAPI_GTK4 ${required} IMPORTED_TARGET gtk4>=4.10 webkitgtk-6.0)
        pkg_check_modules(UNIAPI_GTK4_EXT ${required} IMPORTED_TARGET webkitgtk-web-process-extension-6.0)
        if(UNIAPI_GTK4_FOUND AND UNIAPI_GTK4_EXT_FOUND)
            set(uniapi_gtk_found 4)
        endif()
    endif()
    if(NOT uniapi_gtk_found)
        pkg_check_modules(UNIAPI_GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
        pkg_check_modules(UNIAPI_GTK3_EXT REQUIRED IMPORTED_TARGET webkit2gtk-web-extension-4.1)
        set(uniapi_gtk_found 3)
    endif()
    if(uniapi_gtk_found STREQUAL "4")
        message(STATUS "uniapi: Linux backend uses GTK 4 + webkitgtk-6.0")
        set(uniapi_webkit PkgConfig::UNIAPI_GTK4)
        set(uniapi_webext_lib PkgConfig::UNIAPI_GTK4_EXT)
        set(uniapi_gtk_define UNIAPI_GTK4)
    else()
        message(STATUS "uniapi: Linux backend uses GTK 3 + webkit2gtk-4.1")
        set(uniapi_webkit PkgConfig::UNIAPI_GTK3)
        set(uniapi_webext_lib PkgConfig::UNIAPI_GTK3_EXT)
        set(uniapi_gtk_define "")  # the GTK 3 code is the #else of UNIAPI_GTK4
    endif()
    target_sources(uniapi PRIVATE src/platform/linux/backend_linux.cpp)
    target_link_libraries(uniapi PRIVATE ${uniapi_webkit})
    if(uniapi_gtk_define)
        target_compile_definitions(uniapi PRIVATE ${uniapi_gtk_define})
    endif()

    # Shared memory: the web process extension WebKitGTK loads into the page's process. It goes
    # in its own directory (WebKitGTK loads every library there): build/uniapi-webext/. Ship that
    # directory next to your executable; the app also finds it in the build tree.
    add_library(uniapi_webext MODULE src/platform/linux/extension/extension.cpp)
    set_target_properties(uniapi_webext PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/uniapi-webext
        CXX_VISIBILITY_PRESET hidden)
    # WebKit's extension package. Its functions live in the WebKit library itself, which the web
    # process has loaded already.
    target_link_libraries(uniapi_webext PRIVATE ${uniapi_webext_lib})
    target_link_options(uniapi_webext PRIVATE ${uniapi_strip_flag})  # stripped as the apps (UNIAPI_STRIP)
    if(uniapi_gtk_define)
        target_compile_definitions(uniapi_webext PRIVATE ${uniapi_gtk_define})
    endif()
    add_dependencies(uniapi uniapi_webext)
    target_compile_definitions(uniapi PRIVATE UNIAPI_WEBEXT_BUILD_DIR="${CMAKE_BINARY_DIR}/uniapi-webext")
elseif(APPLE)
    # WKWebView (WebKit.framework) in a Cocoa window. Objective-C++ with ARC; the rest of the
    # library stays C++. No shared memory: streams use the fetch fallback.
    target_sources(uniapi PRIVATE src/platform/macos/backend_macos.mm)
    set_source_files_properties(src/platform/macos/backend_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(uniapi PRIVATE "-framework Cocoa" "-framework WebKit" "-framework UniformTypeIdentifiers"
        "-framework UserNotifications")
else()
    message(FATAL_ERROR "uniapi supports Windows, Linux and macOS only.")
endif()

# The tray, linked by apps that use it (uniapi_tray).
add_subdirectory(tray)

# --- Examples and tests -------------------------------------------------------------------

if(UNIAPI_BUILD_EXAMPLES)
    add_subdirectory(examples/hello)
    add_subdirectory(examples/hello_c)
    add_subdirectory(examples/uniapi_stream)
    add_subdirectory(examples/uniapi_pagestream)
    add_subdirectory(examples/uniapi_logo)
    add_subdirectory(examples/uniapi_notify)
    add_subdirectory(examples/uniapi_tray)
endif()

if(UNIAPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
