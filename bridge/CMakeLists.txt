set(TARGET UE5MCPBridge)
project(${TARGET})

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/Bridge.cpp
    src/Server.cpp
    src/GameThread.cpp
    src/Handles.cpp
    src/Marshal.cpp
    src/Tools_Core.cpp
    src/Tools_Hooks.cpp
    src/Tools_World.cpp
    src/Tools_Assets.cpp
    src/Tools_Lua.cpp
    src/Tools_System.cpp
    src/Win32Capture.cpp)
target_include_directories(${TARGET} PRIVATE include)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE ue5mcp_common ws2_32 windowscodecs ole32 gdi32 user32)
target_compile_definitions(${TARGET} PRIVATE UE5MCP_BRIDGE)
