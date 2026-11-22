# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# uniapi_icon(<target> [<icon.ico>] [NAME <name>] [DESCRIPTION <text>] [VERSION <1.2.3>]
#             [COMPANY <name>] [COPYRIGHT <text>] [COMMENTS <text>])
#
# Gives a program its icon on Windows: Explorer shows it on the .exe, the taskbar on the running
# program, and uniapi's windows in their title bars (the program's first icon resource is their
# default; App::setIcon / Window::setIcon still give one of its own). <icon.ico> defaults to the
# uniapi logo (doc/logo/uniapi.ico: 16, 32, 48 and 256 px). Elsewhere it does nothing: on Linux a
# window's icon comes from App::setIcon (GTK 3) or the app's .desktop file (GTK 4, Wayland),
# which uniapi_install() installs. On macOS too: an app's icon is its .app bundle's, made by
# uniapi_install() from its ICON.
#
# The program's details, each optional, go into its version resource beside the icon: what
# Explorer shows under Properties > Details, and Task Manager as the program's name. With none
# of them the program gets its icon alone.
#
#   NAME         Product name
#   DESCRIPTION  File description, and Task Manager's name for the program (NAME if not given)
#   VERSION      File version and Product version: one to four numbers of 0..65535, 1.2.3
#   COMPANY      Company
#   COPYRIGHT    Copyright
#   COMMENTS     Comments
#
# A VERSION that isn't one is an error on every platform, so a mistake shows wherever you build.
#
#   uniapi_icon(my_app)                                  # the uniapi logo
#   uniapi_icon(my_app "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")
#   uniapi_icon(my_app NAME "My App" VERSION ${PROJECT_VERSION} COMPANY "My Company"
#               COPYRIGHT "© 2026 Me")

# In the cache, so that a project that has uniapi as a subfolder sees it where it calls
# uniapi_icon() (a plain variable stays in uniapi's own folder).
set(UNIAPI_DEFAULT_ICON "${CMAKE_CURRENT_LIST_DIR}/../doc/logo/uniapi.ico" CACHE INTERNAL "")

# A detail given as "" (VERSION ${PROJECT_VERSION} with none set) is kept as "" and then left
# out, without CMake 3.31's warning about it. Functions keep the policies of where they are
# defined.
cmake_policy(PUSH)
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# Sets <out> to why <text> isn't a version (one to four numbers of 0..65535 with dots
# between, as Windows stores it), or to "" when it is one.
function(uniapi_icon_check_version text out)
    set(${out} "" PARENT_SCOPE)
    if(text MATCHES "^[0-9]+(\\.[0-9]+)*$")
        string(REPLACE "." ";" parts "${text}")
        list(LENGTH parts count)
        if(count GREATER 4)
            set(${out} "VERSION \"${text}\": at most four numbers, like 1.2.3.4" PARENT_SCOPE)
            return()
        endif()
        foreach(part IN LISTS parts)
            string(LENGTH "${part}" digits)
            if(digits GREATER 5 OR part GREATER 65535)
                set(${out} "VERSION \"${text}\": each part is a number from 0 to 65535, like 1.2.3" PARENT_SCOPE)
            endif()
        endforeach()
        return()
    endif()
    set(${out} "VERSION \"${text}\": one to four numbers from 0 to 65535 with dots between, like 1.2.3" PARENT_SCOPE)
endfunction()

# A text as a resource script string: quotes doubled, backslashes kept, line breaks as \n.
function(uniapi_icon_rc_string text out)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\"\"" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\n" "\\n" text "${text}")
    set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()

# Sets <out> to the resource script for <icon> (a path with forward slashes) and the details
# given as NAME/DESCRIPTION/VERSION/COMPANY/COPYRIGHT/COMMENTS, the version already checked.
function(uniapi_icon_rc out icon)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "NAME;DESCRIPTION;VERSION;COMPANY;COPYRIGHT;COMMENTS" "")
    # UTF-8, so a © or a name in any script arrives as written.
    set(rc "#pragma code_page(65001)\n")
    # ID 1: the first icon group, which Windows takes as the program's icon.
    string(APPEND rc "1 ICON \"${icon}\"\n")
    # Compared with "", not tested as booleans: a name like "Off" or a version "0" is a value.
    if("${arg_DESCRIPTION}" STREQUAL "")
        set(arg_DESCRIPTION "${arg_NAME}")
    endif()
    if("${arg_NAME}${arg_DESCRIPTION}${arg_VERSION}${arg_COMPANY}${arg_COPYRIGHT}${arg_COMMENTS}" STREQUAL "")
        set(${out} "${rc}" PARENT_SCOPE)
        return()
    endif()
    string(APPEND rc "\n1 VERSIONINFO\n")
    if(NOT "${arg_VERSION}" STREQUAL "")
        string(REPLACE "." ";" numbers "${arg_VERSION}")
        list(LENGTH numbers count)
        while(count LESS 4)
            list(APPEND numbers 0)
            math(EXPR count "${count} + 1")
        endwhile()
        list(JOIN numbers "," numbers)
        string(APPEND rc "FILEVERSION ${numbers}\nPRODUCTVERSION ${numbers}\n")
    endif()
    # An application (VFT_APP) for 32-bit Windows (VOS_NT_WINDOWS32), no flags.
    string(APPEND rc "FILEFLAGSMASK 0x3F\nFILEFLAGS 0x0\nFILEOS 0x40004\nFILETYPE 0x1\nFILESUBTYPE 0x0\n")
    # The texts are US English in Unicode (040904B0), what Explorer falls back to under every
    # display language.
    string(APPEND rc "BEGIN\n    BLOCK \"StringFileInfo\"\n    BEGIN\n        BLOCK \"040904B0\"\n        BEGIN\n")
    foreach(pair "ProductName;NAME" "FileDescription;DESCRIPTION" "CompanyName;COMPANY"
                 "LegalCopyright;COPYRIGHT" "Comments;COMMENTS" "FileVersion;VERSION" "ProductVersion;VERSION")
        list(GET pair 0 key)
        list(GET pair 1 field)
        if(NOT "${arg_${field}}" STREQUAL "")
            uniapi_icon_rc_string("${arg_${field}}" value)
            string(APPEND rc "            VALUE \"${key}\", ${value}\n")
        endif()
    endforeach()
    string(APPEND rc "        END\n    END\n    BLOCK \"VarFileInfo\"\n    BEGIN\n"
                     "        VALUE \"Translation\", 0x409, 1200\n    END\nEND\n")
    set(${out} "${rc}" PARENT_SCOPE)
endfunction()

function(uniapi_icon target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;DESCRIPTION;VERSION;COMPANY;COPYRIGHT;COMMENTS" "")
    list(LENGTH arg_UNPARSED_ARGUMENTS extra)
    if(extra GREATER 1)
        message(FATAL_ERROR "uniapi_icon(${target}): one icon at most, and the details by name "
                            "(NAME, DESCRIPTION, VERSION, COMPANY, COPYRIGHT, COMMENTS): ${arg_UNPARSED_ARGUMENTS}")
    endif()
    # A detail without a value (VERSION ${PROJECT_VERSION} with none set) is left out, as one not given.
    if(NOT "${arg_VERSION}" STREQUAL "")
        uniapi_icon_check_version("${arg_VERSION}" problem)
        if(problem)
            message(FATAL_ERROR "uniapi_icon(${target}): ${problem}")
        endif()
    endif()
    if(NOT WIN32)
        return()
    endif()
    if(extra EQUAL 1)
        set(icon "${arg_UNPARSED_ARGUMENTS}")
    else()
        set(icon "${UNIAPI_DEFAULT_ICON}")
    endif()
    get_filename_component(icon "${icon}" ABSOLUTE)
    if(NOT EXISTS "${icon}")
        message(FATAL_ERROR "uniapi_icon(${target}): no icon at ${icon}")
    endif()
    file(TO_CMAKE_PATH "${icon}" icon)  # forward slashes: the resource compiler takes them
    uniapi_icon_rc(rc "${icon}" NAME "${arg_NAME}" DESCRIPTION "${arg_DESCRIPTION}" VERSION "${arg_VERSION}"
                   COMPANY "${arg_COMPANY}" COPYRIGHT "${arg_COPYRIGHT}" COMMENTS "${arg_COMMENTS}")
    # The resource script, rewritten only when it changes; the .exe is rebuilt when the icon
    # file changes.
    set(script "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc")
    set(old "")
    if(EXISTS "${script}")
        file(READ "${script}" old)
    endif()
    if(NOT old STREQUAL rc)
        file(WRITE "${script}" "${rc}")
    endif()
    target_sources(${target} PRIVATE "${script}")
    set_property(SOURCE "${script}" APPEND PROPERTY OBJECT_DEPENDS "${icon}")
endfunction()

cmake_policy(POP)
