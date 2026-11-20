add_executable(uniapi_logo WIN32 main.cpp)
uniapi_icon(uniapi_logo)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_logo PROPERTIES
    OUTPUT_NAME uniapi_logo
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_logo PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_logo PRIVATE /ENTRY:mainCRTStartup)
endif()
uniapi_embed(uniapi_logo logo_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
# The logo's files as doc/logo/make.py writes them (NO_MINIFY: the SVGs exactly as made).
uniapi_embed(uniapi_logo logo_files "${PROJECT_SOURCE_DIR}/doc/logo" NO_MINIFY)
uniapi_install(uniapi_logo)
