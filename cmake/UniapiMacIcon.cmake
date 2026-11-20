# Makes a macOS app icon (.icns) from an .svg or .png; run during the build for uniapi_install()
# on macOS (cmake -P). Set by the caller: UNIAPI_ICON_SOURCE (the picture; square, best 1024 px
# or an SVG), UNIAPI_ICON_OUTPUT (the .icns to write). sips draws every size the icon set needs
# (it reads SVG too), iconutil packs them.

set(iconset "${UNIAPI_ICON_OUTPUT}.iconset")
file(REMOVE_RECURSE "${iconset}")
file(MAKE_DIRECTORY "${iconset}")
foreach(size 16 32 128 256 512)
    math(EXPR double "${size} * 2")
    foreach(entry "${size};icon_${size}x${size}.png" "${double};icon_${size}x${size}@2x.png")
        list(GET entry 0 pixels)
        list(GET entry 1 file)
        execute_process(
            COMMAND sips -s format png -z ${pixels} ${pixels} "${UNIAPI_ICON_SOURCE}" --out "${iconset}/${file}"
            RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "uniapi: sips couldn't draw ${UNIAPI_ICON_SOURCE} at ${pixels} px: ${error}")
        endif()
    endforeach()
endforeach()
execute_process(COMMAND iconutil -c icns "${iconset}" -o "${UNIAPI_ICON_OUTPUT}"
                RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "uniapi: iconutil failed for ${UNIAPI_ICON_SOURCE}: ${error}")
endif()
file(REMOVE_RECURSE "${iconset}")
