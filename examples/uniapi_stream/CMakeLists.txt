option(UNIAPI_STREAM_EMBED_WEB
       "Compile uniapi_stream's web/ folder into the program (OFF reads it from the source folder, handy for live editing)"
       ON)

add_executable(uniapi_stream WIN32 main.cpp)
uniapi_icon(uniapi_stream)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_stream PROPERTIES
    OUTPUT_NAME uniapi_stream
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_stream PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_stream PRIVATE /ENTRY:mainCRTStartup)
endif()

if(UNIAPI_STREAM_EMBED_WEB)
    uniapi_embed(uniapi_stream stream_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
    target_compile_definitions(uniapi_stream PRIVATE UNIAPI_STREAM_EMBED_WEB)
else()
    target_compile_definitions(uniapi_stream PRIVATE UNIAPI_STREAM_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
endif()
# The demo keeps its .conf next to the executable; where that folder isn't writable, settings
# simply aren't saved.
uniapi_install(uniapi_stream)
