# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# The icon test: what uniapi_icon() writes into a Windows program, checked on every platform by
# running this script (cmake -P): the resource script for the icon alone, the program's details
# (its version resource) and which versions it takes. The desktop test reads the details back
# from its own .exe on Windows.
#
#   cmake -P tests/icon/check.cmake

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/UniapiIcon.cmake")

set(failed 0)
macro(check name condition)
    if(${condition})
        message("  pass  ${name}")
    else()
        message("  FAIL  ${name}")
        math(EXPR failed "${failed} + 1")
    endif()
endmacro()
# Whether text holds needle.
function(has text needle out)
    string(FIND "${text}" "${needle}" at)
    if(at EQUAL -1)
        set(${out} FALSE PARENT_SCOPE)
    else()
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

# The details reach the version resource: its texts, and the version as numbers too.
uniapi_icon_rc(rc "C:/app/app.ico" NAME "My App" VERSION 1.2.3 COMPANY "My Company" COPYRIGHT "(c) Me"
               COMMENTS "A comment")
set(ok TRUE)
foreach(needle "1 ICON \"C:/app/app.ico\"" "FILEVERSION 1,2,3,0" "PRODUCTVERSION 1,2,3,0"
               "VALUE \"ProductName\", \"My App\"" "VALUE \"FileDescription\", \"My App\""
               "VALUE \"CompanyName\", \"My Company\"" "VALUE \"LegalCopyright\", \"(c) Me\""
               "VALUE \"Comments\", \"A comment\"" "VALUE \"FileVersion\", \"1.2.3\""
               "VALUE \"ProductVersion\", \"1.2.3\"" "BLOCK \"040904B0\"" "VALUE \"Translation\", 0x409, 1200")
    has("${rc}" "${needle}" there)
    if(NOT there)
        set(ok FALSE)
        message("        missing: ${needle}")
    endif()
endforeach()
check("the details reach the version resource (the description is the name when not given)" ok)

# A description of its own is kept, and fields not given are left out.
uniapi_icon_rc(rc "a.ico" NAME "My App" DESCRIPTION "Does things")
has("${rc}" "VALUE \"FileDescription\", \"Does things\"" kept)
set(absent TRUE)
foreach(key CompanyName LegalCopyright Comments FileVersion FILEVERSION)
    has("${rc}" "${key}" there)
    if(there)
        set(absent FALSE)
    endif()
endforeach()
check("a description of its own is kept; fields not given are left out" "kept;AND;absent")

# Without details the program gets its icon alone, as before.
uniapi_icon_rc(rc "a.ico")
has("${rc}" "1 ICON \"a.ico\"" icon)
has("${rc}" "VERSIONINFO" version)
check("without details: the icon alone" "icon;AND;NOT;version")

# Texts as resource strings: quotes doubled, backslashes kept, line breaks as \n, UTF-8.
uniapi_icon_rc(rc "a.ico" NAME "Off" COMMENTS "say \"hi\"\\now\nnext" COPYRIGHT "© 2026 Me")
has("${rc}" "VALUE \"ProductName\", \"Off\"" named)
has("${rc}" "VALUE \"Comments\", \"say \"\"hi\"\"\\\\now\\nnext\"" quoted)
has("${rc}" "VALUE \"LegalCopyright\", \"© 2026 Me\"" utf8)
has("${rc}" "#pragma code_page(65001)" codePage)
check("texts are quoted for the resource compiler (a name \"Off\" too), in UTF-8" "named;AND;quoted;AND;utf8;AND;codePage")

# Which versions it takes.
set(ok TRUE)
foreach(good "1" "1.2" "0.10.40" "1.2.3.4" "65535.0" "0")
    uniapi_icon_check_version("${good}" problem)
    if(problem)
        set(ok FALSE)
        message("        refused: ${good} (${problem})")
    endif()
endforeach()
foreach(bad "v1.2" "1.2.3.4.5" "1..2" "1.2-beta" "70000" " 1.2" "" "1." "123456")
    uniapi_icon_check_version("${bad}" problem)
    if(NOT problem)
        set(ok FALSE)
        message("        accepted: \"${bad}\"")
    endif()
endforeach()
check("versions: one to four numbers of 0..65535" ok)

# The script compiles, where a resource compiler for Windows is at hand (MinGW's windres).
find_program(windres NAMES x86_64-w64-mingw32-windres windres)
if(windres)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/uniapi_icon_check")
    file(MAKE_DIRECTORY "${dir}")
    set(logo "${CMAKE_CURRENT_LIST_DIR}/../../doc/logo/uniapi.ico")
    get_filename_component(logo "${logo}" ABSOLUTE)
    uniapi_icon_rc(rc "${logo}" NAME "My App" VERSION 1.2.3 COPYRIGHT "© 2026 \"Me\"" COMMENTS "two\nlines")
    file(WRITE "${dir}/app.rc" "${rc}")
    execute_process(COMMAND "${windres}" "${dir}/app.rc" -O coff -o "${dir}/app.res"
                    RESULT_VARIABLE result ERROR_VARIABLE errors)
    check("the script compiles (${windres})" "result;EQUAL;0")
    if(NOT result EQUAL 0)
        message("        ${errors}")
    endif()
    file(REMOVE_RECURSE "${dir}")
else()
    message("  n/a   the script compiles: no windres here (the Windows build compiles it)")
endif()

if(failed GREATER 0)
    message(FATAL_ERROR "icon: ${failed} check(s) failed")
endif()
message("icon: all checks passed")
