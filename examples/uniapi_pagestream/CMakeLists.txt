# Copyright (C) 2026 bouncyball
# Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
# SPDX-License-Identifier: Apache-2.0

option(UNIAPI_PAGESTREAM_EMBED_WEB
       "Compile uniapi_pagestream's web/ folder into the program (OFF reads it from the source folder, handy for live editing)"
       ON)

add_executable(uniapi_pagestream WIN32 main.cpp)
uniapi_icon(uniapi_pagestream)  # the uniapi logo on the .exe, the taskbar and its windows (Windows)
set_target_properties(uniapi_pagestream PROPERTIES
    OUTPUT_NAME uniapi_pagestream
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(uniapi_pagestream PRIVATE uniapi)
if(MSVC)
    # A GUI app (no console window) that still starts at a portable main().
    target_link_options(uniapi_pagestream PRIVATE /ENTRY:mainCRTStartup)
endif()

if(UNIAPI_PAGESTREAM_EMBED_WEB)
    uniapi_embed(uniapi_pagestream pagestream_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
    target_compile_definitions(uniapi_pagestream PRIVATE UNIAPI_PAGESTREAM_EMBED_WEB)
else()
    target_compile_definitions(uniapi_pagestream PRIVATE UNIAPI_PAGESTREAM_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
endif()
uniapi_install(uniapi_pagestream)
