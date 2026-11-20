# uniapi_embed(<target> <name> <directory> [BUNDLE <entry>...] [KEEP <pattern>...] [NO_MINIFY]
#              [COMPRESS] [NO_COMPRESS])
#
# Compiles every file under <directory> into <target>. C++ code reaches them with
#   UNIAPI_ASSETS(<name>);
#   app.serve(<name>);
# Adding or removing files is picked up on the next build (CONFIGURE_DEPENDS), and edited
# files are re-embedded.
#
# The embed tool (tools/embed) writes the generated file at build time in one pass:
# milliseconds per megabyte. .js and .css files are minified with esbuild first, .html, .svg
# and .json with tdewolff/minify (UNIAPI_EMBED_MINIFY, ON; NO_MINIFY turns it off for one call). With COMPRESS (or
# UNIAPI_EMBED_COMPRESS, OFF by default; NO_COMPRESS turns that off for one call) files that
# shrink by a tenth or more are stored zlib-compressed, for a smaller program file, and
# inflated each time they are served. With GCC or Clang on Linux the bytes come in through the assembler's .incbin and
# cost no compile time (UNIAPI_EMBED_INCBIN); elsewhere they are C++ byte arrays (MSVC compiles
# about a megabyte a second). Cross-compiling, where the tool can't run, a CMake script writes
# the byte arrays instead, uncompressed and slower (seconds per megabyte).
#
# KEEP names files to embed as they are, not minified: patterns for the file's name, with *
# (any characters), ? (one) and [a-z] sets, like "*.min.js". For files that are minified
# already, or that must stay byte for byte what they are.
#
# BUNDLE names scripts and style sheets (.js, .mjs, .css; paths inside <directory>) to bundle
# with esbuild: each entry and everything it imports become one file, embedded under the
# entry's path (a script as an ES module), so the page refers to the same path before and
# after. The files that went into a bundle are not embedded by themselves. Bundles are
# minified like the rest (not with NO_MINIFY). It needs the embed tool and esbuild: not when
# cross-compiling, and not with UNIAPI_EMBED_MINIFY=OFF unless UNIAPI_ESBUILD names an esbuild.

set(UNIAPI_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/embed_files.cmake" CACHE INTERNAL "")

function(uniapi_embed target name directory)
    cmake_parse_arguments(PARSE_ARGV 3 EMBED "NO_MINIFY;COMPRESS;NO_COMPRESS" "" "BUNDLE;KEEP")
    file(GLOB_RECURSE paths CONFIGURE_DEPENDS RELATIVE "${directory}" "${directory}/*")
    if(NOT paths)
        message(FATAL_ERROR "uniapi_embed: ${directory} has no files to embed")
    endif()
    list(SORT paths)

    set(dependencies "")
    foreach(path IN LISTS paths)
        list(APPEND dependencies "${directory}/${path}")
    endforeach()

    # The file list is written at configure time; the build step only reads it.
    set(list_file "${CMAKE_CURRENT_BINARY_DIR}/uniapi_embed_${name}.list")
    string(REPLACE ";" "\n" lines "${paths}")
    file(CONFIGURE OUTPUT "${list_file}" CONTENT "${lines}\n" @ONLY)

    set(output "${CMAKE_CURRENT_BINARY_DIR}/uniapi_embed_${name}.cpp")
    if(TARGET uniapi_embed_tool)
        set(flags "")
        set(minifier "")
        if(UNIAPI_EMBED_MINIFY AND NOT EMBED_NO_MINIFY)
            if(UNIAPI_ESBUILD_EXECUTABLE)
                list(APPEND flags --minify "${UNIAPI_ESBUILD_EXECUTABLE}")
                list(APPEND minifier "${UNIAPI_ESBUILD_EXECUTABLE}")
            endif()
            if(UNIAPI_MINIFY_EXECUTABLE)
                list(APPEND flags --minify-markup "${UNIAPI_MINIFY_EXECUTABLE}")
                list(APPEND minifier "${UNIAPI_MINIFY_EXECUTABLE}")
            endif()
        endif()
        foreach(pattern IN LISTS EMBED_KEEP)
            list(APPEND flags --keep "${pattern}")
        endforeach()
        if(EMBED_BUNDLE)
            set(bundler "${UNIAPI_ESBUILD_EXECUTABLE}")
            if(NOT bundler AND UNIAPI_ESBUILD)
                set(bundler "${UNIAPI_ESBUILD}")
            endif()
            if(NOT bundler)
                message(FATAL_ERROR "uniapi_embed(${name}): BUNDLE needs esbuild: leave UNIAPI_EMBED_MINIFY on, or set UNIAPI_ESBUILD to an esbuild program")
            endif()
            list(APPEND flags --esbuild "${bundler}")
            list(APPEND minifier "${bundler}")
            foreach(entry IN LISTS EMBED_BUNDLE)
                if(NOT EXISTS "${directory}/${entry}")
                    message(FATAL_ERROR "uniapi_embed(${name}): BUNDLE ${entry} is not a file under ${directory}")
                endif()
                list(APPEND flags --bundle "${entry}")
            endforeach()
        endif()
        if((UNIAPI_EMBED_COMPRESS OR EMBED_COMPRESS) AND NOT EMBED_NO_COMPRESS)
            list(APPEND flags --compress)
        endif()
        if(UNIAPI_EMBED_INCBIN)
            list(APPEND flags --incbin)
        endif()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND uniapi_embed_tool "${name}" "${directory}" "${list_file}" "${output}" ${flags}
            DEPENDS ${dependencies} "${list_file}" uniapi_embed_tool ${minifier}
            COMMENT "Embedding ${directory} as ${name}"
            VERBATIM)
    else()
        if(EMBED_BUNDLE)
            message(FATAL_ERROR "uniapi_embed(${name}): BUNDLE needs the embed tool, which can't run when cross-compiling")
        endif()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" "-DNAME=${name}" "-DDIRECTORY=${directory}"
                    "-DLIST=${list_file}" "-DOUTPUT=${output}" -P "${UNIAPI_EMBED_SCRIPT}"
            DEPENDS ${dependencies} "${list_file}" "${UNIAPI_EMBED_SCRIPT}"
            COMMENT "Embedding ${directory} as ${name} (CMake script)"
            VERBATIM)
    endif()
    target_sources(${target} PRIVATE "${output}")
endfunction()
