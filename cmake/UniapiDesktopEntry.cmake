# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# Writes and installs an app's desktop entry; run by cmake --install for uniapi_install() on
# Linux. It runs then, not when configuring, because only then is the prefix known
# (cmake --install --prefix), and Exec holds the installed program's full path. Set by the
# caller: UNIAPI_ENTRY_NAME (the app, also its icon and window class), UNIAPI_ENTRY_TITLE (the
# name the menu shows), UNIAPI_ENTRY_COMMENT (what the app is; no Comment line when empty),
# UNIAPI_ENTRY_CATEGORIES (the Categories value, each name followed by ";"; Utility; when not
# set), UNIAPI_ENTRY_MIME (the MIME type of the files the app opens, declared by
# uniapi_install(); empty: none), UNIAPI_ENTRY_DATADIR (share, relative),
# UNIAPI_ENTRY_EXE (the program, relative to the prefix),
# UNIAPI_ENTRY_FILE (where to write the entry first), UNIAPI_ENTRY_DESTINATION (relative).

set(exe "${CMAKE_INSTALL_PREFIX}/${UNIAPI_ENTRY_EXE}")
cmake_path(NORMAL_PATH exe)
# Desktop Entry Specification: Exec quotes the path (it may have spaces); inside the quotes
# \ " ` and $ take a backslash. Then, as in every value, a backslash is written \\.
string(REGEX REPLACE "([\\\"`$])" "\\\\\\1" exe "${exe}")
string(REPLACE "\\" "\\\\" exe "${exe}")
string(REPLACE "\\" "\\\\" title "${UNIAPI_ENTRY_TITLE}")
# A value is one line: a line break is written \n.
set(comment_line "")
if(NOT "${UNIAPI_ENTRY_COMMENT}" STREQUAL "")
    string(REPLACE "\\" "\\\\" comment "${UNIAPI_ENTRY_COMMENT}")
    string(REPLACE "\n" "\\n" comment "${comment}")
    set(comment_line "Comment=${comment}\n")
endif()
set(categories "${UNIAPI_ENTRY_CATEGORIES}")
if(NOT categories)
    set(categories "Utility;")
endif()
# Files of the app's type: the entry says it opens them (MimeType), and takes them as
# arguments (%F: every file selected, as paths).
set(files "")
set(mime_line "")
if(NOT "${UNIAPI_ENTRY_MIME}" STREQUAL "")
    set(files " %F")
    set(mime_line "MimeType=${UNIAPI_ENTRY_MIME};\n")
endif()

file(WRITE "${UNIAPI_ENTRY_FILE}" "[Desktop Entry]
Type=Application
Name=${title}
${comment_line}Exec=\"${exe}\"${files}
Icon=${UNIAPI_ENTRY_NAME}
StartupWMClass=${UNIAPI_ENTRY_NAME}
Terminal=false
Categories=${categories}
${mime_line}")
file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/${UNIAPI_ENTRY_DESTINATION}" TYPE FILE FILES "${UNIAPI_ENTRY_FILE}")

# The desktop reads its lists of types and of entries from caches: brought up to date here when
# the prefix is one it looks in and the files went straight there (no DESTDIR: a package is
# being put together, and its own hooks do this where it is installed).
if(NOT "${UNIAPI_ENTRY_MIME}" STREQUAL "" AND "$ENV{DESTDIR}" STREQUAL "")
    set(prefix "${CMAKE_INSTALL_PREFIX}")
    cmake_path(NORMAL_PATH prefix)
    string(REGEX REPLACE "/+$" "" prefix "${prefix}")
    if(prefix STREQUAL "$ENV{HOME}/.local" OR prefix STREQUAL "/usr/local" OR prefix STREQUAL "/usr")
        set(data "${prefix}/${UNIAPI_ENTRY_DATADIR}")
        find_program(UNIAPI_UPDATE_MIME update-mime-database)
        if(UNIAPI_UPDATE_MIME)
            execute_process(COMMAND "${UNIAPI_UPDATE_MIME}" "${data}/mime" OUTPUT_QUIET ERROR_QUIET)
        endif()
        find_program(UNIAPI_UPDATE_DESKTOP update-desktop-database)
        if(UNIAPI_UPDATE_DESKTOP)
            execute_process(COMMAND "${UNIAPI_UPDATE_DESKTOP}" "${data}/applications" OUTPUT_QUIET ERROR_QUIET)
        endif()
    endif()
endif()
