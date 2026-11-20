# Writes and installs an app's desktop entry; run by cmake --install for uniapi_install() on
# Linux. It runs then, not when configuring, because only then is the prefix known
# (cmake --install --prefix), and Exec holds the installed program's full path. Set by the
# caller: UNIAPI_ENTRY_NAME (the app, also its icon and window class), UNIAPI_ENTRY_TITLE (the
# name the menu shows), UNIAPI_ENTRY_COMMENT (what the app is; no Comment line when empty),
# UNIAPI_ENTRY_CATEGORIES (the Categories value, each name followed by ";"; Utility; when not
# set), UNIAPI_ENTRY_EXE (the program, relative to the prefix),
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

file(WRITE "${UNIAPI_ENTRY_FILE}" "[Desktop Entry]
Type=Application
Name=${title}
${comment_line}Exec=\"${exe}\"
Icon=${UNIAPI_ENTRY_NAME}
StartupWMClass=${UNIAPI_ENTRY_NAME}
Terminal=false
Categories=${categories}
")
file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/${UNIAPI_ENTRY_DESTINATION}" TYPE FILE FILES "${UNIAPI_ENTRY_FILE}")
