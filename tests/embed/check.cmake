# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# The embed test: what uniapi_embed()'s BUNDLE and KEEP do, checked by running the embed tool on
# tests/embed/web (cmake -P; no window): the entries are bundled with what they import, the
# files that went into a bundle are not embedded by themselves, the rest is, and without
# --minify the bundles stay readable.
#
#   cmake -DTOOL=<uniapi_embed_tool> -DESBUILD=<esbuild> -DWORK=<folder> -P tests/embed/check.cmake

set(web "${CMAKE_CURRENT_LIST_DIR}/web")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(names css/part.css css/style.css index.html js/alone.js js/kept.min.js js/main.js js/part.js)
string(REPLACE ";" "\n" lines "${names}")
file(WRITE "${WORK}/web.list" "${lines}\n")

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

# Minified, with both entries bundled.
execute_process(COMMAND "${TOOL}" embed_test "${web}" "${WORK}/web.list" "${WORK}/min.cpp" --minify "${ESBUILD}"
                        --bundle js/main.js --bundle css/style.css --keep "*.min.js"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
check("the tool runs with --bundle (${result}) ${error}" "result;EQUAL;0")
has("${output}" "5 files" four)
has("${output}" "2 bundled" two)
check("five files embedded, two of them bundles: ${output}" "four;AND;two")
file(READ "${WORK}/min.cpp" generated)
foreach(name js/main.js css/style.css js/alone.js js/kept.min.js index.html)
    has("${generated}" "{\"${name}\"" listed)
    check("${name} is embedded" "listed")
endforeach()
foreach(name js/part.js css/part.css)
    has("${generated}" "{\"${name}\"" listed)
    check("${name} went into a bundle and is not embedded by itself" "NOT;listed")
endforeach()
file(READ "${WORK}/min.cpp.min/js/main.js" script)
has("${script}" "the part is in the bundle" holds)
has("${script}" "import" imports)
has("${script}" "// The entry" comment)
check("the script bundle holds its import's code, has no import left, and is minified" "holds;AND;NOT;imports;AND;NOT;comment")
file(READ "${WORK}/min.cpp.min/css/style.css" style)
has("${style}" "from-the-part" holds)
has("${style}" "@import" imports)
check("the style bundle holds its import's rules and has no @import left" "holds;AND;NOT;imports")
file(READ "${WORK}/min.cpp.min/js/alone.js" alone)
has("${alone}" "Not imported" comment)
check("a script that is no entry is minified by itself" "NOT;comment")

check("a kept file (--keep \"*.min.js\") is not minified: it is embedded from its source" "NOT;EXISTS;${WORK}/min.cpp.min/js/kept.min.js")

# Not minified (NO_MINIFY): bundled all the same, and readable.
execute_process(COMMAND "${TOOL}" embed_test "${web}" "${WORK}/web.list" "${WORK}/plain.cpp" --esbuild "${ESBUILD}"
                        --bundle js/main.js
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
check("the tool bundles without minifying (${result}) ${error}" "result;EQUAL;0")
file(READ "${WORK}/plain.cpp" generated)
has("${generated}" "{\"js/part.js\"" part)
has("${generated}" "{\"css/part.css\"" sheet)
check("without minifying: the script's part is in its bundle, the style sheets are embedded as they are" "NOT;part;AND;sheet")
file(READ "${WORK}/plain.cpp.min/js/main.js" script)
has("${script}" "the part is in the bundle" holds)
has("${script}" "greeting" readable)
check("the bundle that is not minified keeps its names" "holds;AND;readable")

# What it refuses.
execute_process(COMMAND "${TOOL}" embed_test "${web}" "${WORK}/web.list" "${WORK}/no.cpp" --bundle js/main.js
                RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
check("--bundle without an esbuild is refused" "NOT;result;EQUAL;0")
execute_process(COMMAND "${TOOL}" embed_test "${web}" "${WORK}/web.list" "${WORK}/no.cpp" --esbuild "${ESBUILD}" --bundle js/none.js
                RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
check("--bundle of a file that is not listed is refused" "NOT;result;EQUAL;0")
execute_process(COMMAND "${TOOL}" embed_test "${web}" "${WORK}/web.list" "${WORK}/no.cpp" --esbuild "${ESBUILD}" --bundle index.html
                RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
check("--bundle of a page is refused" "NOT;result;EQUAL;0")

if(failed GREATER 0)
    message(FATAL_ERROR "${failed} embed check(s) failed")
endif()
message("embed: all checks pass")
