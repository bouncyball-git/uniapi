# System notifications: the page asks C++ for one, a click on it comes back. Installed with
# uniapi_install() (the .app bundle macOS needs, the .desktop entry Linux names it by).
add_executable(uniapi_notify WIN32 main.cpp)
uniapi_icon(uniapi_notify)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_notify PROPERTIES
    OUTPUT_NAME uniapi_notify
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_notify PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_notify PRIVATE /ENTRY:mainCRTStartup)
endif()
uniapi_embed(uniapi_notify notify_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
uniapi_install(uniapi_notify)
