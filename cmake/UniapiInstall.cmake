# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

# uniapi_install(<target> [DESTINATION <dir>] [NAME <name>] [ICON <file.svg|file.png>]
#                [COMMENT <text>] [CATEGORIES <name>...] [BUNDLE_ID <id>] [VERSION <v>]
#                [COPYRIGHT <text>]
#                [FILE_TYPES <ext>... [FILE_TYPE_NAME <text>] [FILE_TYPE_ID <uti>]])
#
# Installs an app built with uniapi so that it runs from where it is installed
# (cmake --install <build> --prefix <prefix>):
#
#   Windows  <prefix>/<dir>/<app>.exe. Nothing else is needed: the pages are compiled in, the
#            WebView2 loader is linked in, and the profile goes to %LOCALAPPDATA%\<app>.
#            (Its icon is uniapi_icon()'s, inside the .exe.)
#   Linux    <prefix>/<dir>/<app>, plus uniapi's web process extension (shared memory) in
#            <prefix>/<dir>/../lib/<app>/uniapi-webext/, where the app looks for it; with the
#            default <dir> (bin) that is <prefix>/lib/<app>/uniapi-webext/. And the app's
#            desktop entry, <prefix>/share/applications/<app>.desktop, with its icon in
#            <prefix>/share/icons/hicolor/ (scalable/apps/<app>.svg, or 256x256/apps/<app>.png):
#            an ELF file has no icon, so the desktop finds a window's icon through the entry
#            whose name is the window's app id (the executable's name). NAME is the name the
#            menu shows (default: <app>); ICON defaults to the uniapi logo. The desktop sees
#            them when <prefix>/share is one of its data folders: ~/.local and /usr/local are.
#   macOS    <prefix>/<dir>/<NAME>.app, a bundle: the program in Contents/MacOS/<app>, an
#            Info.plist, and the icon (Contents/Resources/<app>.icns, made from ICON during the
#            build; default the uniapi logo) that the Dock and Finder show. NAME is the name the
#            menu bar and Finder show (default: <app>), BUNDLE_ID the bundle identifier (default
#            org.uniapi.<app>; WebKit keeps the default profile per identifier), VERSION its
#            version (default 1.0). <dir> defaults to . here: --prefix ~/Applications (or
#            /Applications) puts the app where Launchpad and Spotlight find it. The bundle is
#            signed ad hoc after installing (codesign -), as the linker signed the program.
#
# COMMENT says in a few words what the app is: the menu's second line or tooltip on Linux (the
# entry's Comment), Finder's Get Info text on macOS. CATEGORIES are the Linux menu's categories
# (the Desktop Menu Specification's names: System TerminalEmulator; Utility by default).
# COPYRIGHT is the copyright line Finder shows on macOS.
#
# FILE_TYPES (macOS) are file name extensions (without the dot; any case) the app opens: the
# bundle declares them as one type (FILE_TYPE_ID, a uniform type identifier; default
# <bundle id>.document; FILE_TYPE_NAME is what Finder calls it, default "<NAME> document") and
# itself as a viewer of it. Finder then shows those files with a document icon of the app's
# (the system draws it from the app icon: the app's picture on a page) instead of a blank page,
# and a double-click opens them in the app: App::onOpenFiles gets them. Several apps may
# declare the same identifier; use the one the format's authors or other apps use, if any.
#
# <dir> defaults to bin (. on macOS); use one folder level (like bin) so ../lib stays inside <prefix>. With
# DESTINATION . the app is self-contained instead: on Linux the extension goes to
# uniapi-webext/ next to the executable, and there is no desktop entry.

include(GNUInstallDirs)

# The extension's destination goes through "..": CMake 3.31+ normalizes it ("bin/../lib/x"
# becomes "lib/x", the same place) and warns unless told that's wanted. uniapi_install()
# keeps the setting in effect here, where it is defined.
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

# In the cache, so that a project that has uniapi as a subfolder (add_subdirectory,
# FetchContent) sees them where it calls uniapi_install(): plain variables stay in uniapi's own
# folder, and the default icon and the scripts were then not found.
set(UNIAPI_LOGO_SVG "${CMAKE_CURRENT_LIST_DIR}/../doc/logo/uniapi.svg" CACHE INTERNAL "")
set(UNIAPI_DESKTOP_ENTRY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/UniapiDesktopEntry.cmake" CACHE INTERNAL "")
set(UNIAPI_MAC_ICON_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/UniapiMacIcon.cmake" CACHE INTERNAL "")

function(uniapi_install target)
    # Here too: the folder names (bin, share) are plain variables, set above for uniapi's own
    # folder only; a project that has uniapi as a subfolder calls this from its own.
    include(GNUInstallDirs)
    cmake_parse_arguments(ARG "" "DESTINATION;NAME;ICON;BUNDLE_ID;VERSION;COMMENT;COPYRIGHT;FILE_TYPE_NAME;FILE_TYPE_ID"
                          "CATEGORIES;FILE_TYPES" ${ARGN})
    # The Linux menu's categories as the entry's value: every name followed by ";".
    set(categories "")
    foreach(category IN LISTS ARG_CATEGORIES)
        if(NOT category MATCHES "^[A-Za-z0-9-]+$")
            message(FATAL_ERROR "uniapi_install(${target}): CATEGORIES ${category} is not a category name (like System, TerminalEmulator, X-MyOwn)")
        endif()
        string(APPEND categories "${category};")
    endforeach()
    if(NOT categories)
        set(categories "Utility;")
    endif()
    if(APPLE)
        _uniapi_install_mac_bundle(${target} "${ARG_DESTINATION}" "${ARG_NAME}" "${ARG_ICON}" "${ARG_BUNDLE_ID}"
                                   "${ARG_VERSION}" "${ARG_COMMENT}" "${ARG_COPYRIGHT}"
                                   "${ARG_FILE_TYPES}" "${ARG_FILE_TYPE_NAME}" "${ARG_FILE_TYPE_ID}")
        return()
    endif()
    if(NOT ARG_DESTINATION)
        set(ARG_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
    install(TARGETS ${target} RUNTIME DESTINATION "${ARG_DESTINATION}")

    if(TARGET uniapi_webext)
        get_target_property(name ${target} OUTPUT_NAME)
        if(NOT name)
            set(name ${target})
        endif()
        if(ARG_DESTINATION STREQUAL ".")
            set(extension_dir "uniapi-webext")
        else()
            # The app looks in <its folder>/../lib/<its name>/uniapi-webext.
            set(extension_dir "${ARG_DESTINATION}/../lib/${name}/uniapi-webext")
        endif()
        install(FILES "$<TARGET_FILE:uniapi_webext>" DESTINATION "${extension_dir}")
        if(NOT ARG_DESTINATION STREQUAL ".")
            _uniapi_install_desktop_entry("${name}" "${ARG_DESTINATION}" "${ARG_NAME}" "${ARG_ICON}" "${ARG_COMMENT}"
                                          "${categories}")
        endif()
    endif()
endfunction()

# The desktop entry and its icon (Linux).
function(_uniapi_install_desktop_entry name destination display_name icon comment categories)
    if(NOT display_name)
        set(display_name "${name}")
    endif()
    if(NOT icon)
        set(icon "${UNIAPI_LOGO_SVG}")
    endif()
    get_filename_component(icon "${icon}" ABSOLUTE)
    get_filename_component(icon_ext "${icon}" LAST_EXT)
    string(TOLOWER "${icon_ext}" icon_ext)
    if(icon_ext STREQUAL ".svg")
        set(icon_dir "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    elseif(icon_ext STREQUAL ".png")
        set(icon_dir "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
    else()
        message(FATAL_ERROR "uniapi_install(${name}): ICON must be an .svg or .png file: ${icon}")
    endif()
    install(FILES "${icon}" DESTINATION "${icon_dir}" RENAME "${name}${icon_ext}")

    # Written at install time, by UniapiDesktopEntry.cmake (the values in [[ ]]: no escaping).
    install(CODE "set(UNIAPI_ENTRY_NAME [[${name}]])
set(UNIAPI_ENTRY_TITLE [[${display_name}]])
set(UNIAPI_ENTRY_COMMENT [[${comment}]])
set(UNIAPI_ENTRY_CATEGORIES [[${categories}]])
set(UNIAPI_ENTRY_EXE [[${destination}/${name}]])
set(UNIAPI_ENTRY_FILE [[${CMAKE_CURRENT_BINARY_DIR}/uniapi-desktop/${name}.desktop]])
set(UNIAPI_ENTRY_DESTINATION [[${CMAKE_INSTALL_DATADIR}/applications]])
include([[${UNIAPI_DESKTOP_ENTRY_SCRIPT}]])")
endfunction()

# The .app bundle (macOS). The build folder keeps the plain program (build/<app>, as on Linux);
# the bundle is put together when installing.
function(_uniapi_install_mac_bundle target destination display_name icon bundle_id version comment copyright
                                    file_types file_type_name file_type_id)
    get_target_property(name ${target} OUTPUT_NAME)
    if(NOT name)
        set(name ${target})
    endif()
    if(NOT destination)
        set(destination ".")
    endif()
    if(NOT display_name)
        set(display_name "${name}")
    endif()
    if(NOT bundle_id)
        string(REGEX REPLACE "[^A-Za-z0-9.-]" "-" bundle_id "org.uniapi.${name}")
    endif()
    if(NOT version)
        set(version "1.0")
    endif()
    if(NOT icon)
        set(icon "${UNIAPI_LOGO_SVG}")
    endif()
    get_filename_component(icon "${icon}" ABSOLUTE)
    get_filename_component(icon_ext "${icon}" LAST_EXT)
    string(TOLOWER "${icon_ext}" icon_ext)
    if(NOT icon_ext STREQUAL ".svg" AND NOT icon_ext STREQUAL ".png")
        message(FATAL_ERROR "uniapi_install(${name}): ICON must be an .svg or .png file: ${icon}")
    endif()

    set(work "${CMAKE_CURRENT_BINARY_DIR}/uniapi-bundle/${name}")
    set(icns "${work}/${name}.icns")
    add_custom_command(
        OUTPUT "${icns}"
        COMMAND "${CMAKE_COMMAND}" -DUNIAPI_ICON_SOURCE=${icon} -DUNIAPI_ICON_OUTPUT=${icns}
                -P "${UNIAPI_MAC_ICON_SCRIPT}"
        DEPENDS "${icon}" "${UNIAPI_MAC_ICON_SCRIPT}"
        COMMENT "uniapi: app icon for ${name}"
        VERBATIM)
    add_custom_target(${target}_app_icon ALL DEPENDS "${icns}")

    # The file types: one declared type for every extension given (FILE_TYPES).
    set(extensions_xml "")
    foreach(ext IN LISTS file_types)
        string(REGEX REPLACE "^\\." "" ext "${ext}")
        if(NOT ext MATCHES "^[A-Za-z0-9_+-]+$")
            message(FATAL_ERROR "uniapi_install(${name}): FILE_TYPES ${ext} is not a file name extension (like mlv)")
        endif()
        string(TOLOWER "${ext}" ext)
        string(APPEND extensions_xml "<string>${ext}</string>")
    endforeach()
    if(NOT file_type_name)
        set(file_type_name "${display_name} document")
    endif()
    if(NOT file_type_id)
        set(file_type_id "${bundle_id}.document")
    endif()
    if(NOT file_type_id MATCHES "^[A-Za-z0-9.-]+$")
        message(FATAL_ERROR "uniapi_install(${name}): FILE_TYPE_ID ${file_type_id} is not a type identifier (like org.example.format)")
    endif()

    # XML-escaped values for the property list.
    foreach(var display_name bundle_id version name comment copyright file_type_name)
        string(REPLACE "&" "&amp;" ${var}_xml "${${var}}")
        string(REPLACE "<" "&lt;" ${var}_xml "${${var}_xml}")
        string(REPLACE ">" "&gt;" ${var}_xml "${${var}_xml}")
    endforeach()
    # Left out when not given.
    set(more "")
    if(NOT comment STREQUAL "")
        string(APPEND more "    <key>CFBundleGetInfoString</key><string>${comment_xml}</string>\n")
    endif()
    if(NOT copyright STREQUAL "")
        string(APPEND more "    <key>NSHumanReadableCopyright</key><string>${copyright_xml}</string>\n")
    endif()
    if(extensions_xml)
        # Imported, not exported: the format isn't the app's own, and another app may declare it
        # too. public.data, not public.movie: Quick Look and QuickTime would try to play it.
        string(APPEND more "    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>${file_type_name_xml}</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Default</string>
            <key>LSItemContentTypes</key><array><string>${file_type_id}</string></array>
        </dict>
    </array>
    <key>UTImportedTypeDeclarations</key>
    <array>
        <dict>
            <key>UTTypeIdentifier</key><string>${file_type_id}</string>
            <key>UTTypeDescription</key><string>${file_type_name_xml}</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array>${extensions_xml}</array></dict>
        </dict>
    </array>
")
    endif()
    set(minimum "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT minimum)
        set(minimum "14.0")
    endif()
    file(WRITE "${work}/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>${name_xml}</string>
    <key>CFBundleIconFile</key><string>${name_xml}</string>
    <key>CFBundleIdentifier</key><string>${bundle_id_xml}</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>${display_name_xml}</string>
    <key>CFBundleDisplayName</key><string>${display_name_xml}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${version_xml}</string>
    <key>CFBundleVersion</key><string>${version_xml}</string>
    <key>LSMinimumSystemVersion</key><string>${minimum}</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
${more}</dict>
</plist>
")

    if(destination STREQUAL ".")
        set(bundle "${display_name}.app")  # no "./" in the paths the install records
    else()
        set(bundle "${destination}/${display_name}.app")
    endif()
    install(TARGETS ${target} RUNTIME DESTINATION "${bundle}/Contents/MacOS")
    install(FILES "${work}/Info.plist" DESTINATION "${bundle}/Contents")
    install(FILES "${icns}" DESTINATION "${bundle}/Contents/Resources")
    # Sealed as a whole: the program's own ad-hoc signature doesn't cover the bundle's files.
    install(CODE "set(bundle [[${bundle}]])
if(NOT IS_ABSOLUTE \"\${bundle}\")
    set(bundle \"\${CMAKE_INSTALL_PREFIX}/\${bundle}\")
endif()
execute_process(COMMAND codesign --force --sign - \"\${bundle}\" RESULT_VARIABLE signed ERROR_VARIABLE error)
if(signed EQUAL 0)
    # The seal is a file the install made: listed, so uninstalling removes it too.
    list(APPEND CMAKE_INSTALL_MANIFEST_FILES \"\${bundle}/Contents/_CodeSignature/CodeResources\")
else()
    message(WARNING \"uniapi: codesign of \${bundle} failed: \${error}\")
endif()")
endfunction()
