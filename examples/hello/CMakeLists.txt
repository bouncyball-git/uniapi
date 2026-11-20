add_executable(uniapi_hello WIN32 main.cpp)
uniapi_icon(uniapi_hello)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_hello PROPERTIES
    OUTPUT_NAME hello
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_hello PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_hello PRIVATE /ENTRY:mainCRTStartup)
endif()
uniapi_embed(uniapi_hello hello_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
uniapi_install(uniapi_hello)
