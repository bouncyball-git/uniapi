# The hello example in C, through uniapi.h. It shows the same page as examples/hello.
add_executable(uniapi_hello_c WIN32 main.c)
uniapi_icon(uniapi_hello_c)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_hello_c PROPERTIES
    OUTPUT_NAME hello_c
    C_STANDARD 99
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_hello_c PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_hello_c PRIVATE /ENTRY:mainCRTStartup)
endif()
uniapi_embed(uniapi_hello_c hello_c_web "${CMAKE_CURRENT_SOURCE_DIR}/../hello/web")
uniapi_install(uniapi_hello_c)
