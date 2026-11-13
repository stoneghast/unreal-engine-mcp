set(TARGET ue5mcp)
add_executable(${TARGET}
    src/main.cpp
    src/McpServer.cpp
    src/BridgeClient.cpp
    src/ProcessManager.cpp
    src/Session.cpp
    src/HostTools.cpp
    src/Config.cpp)
target_include_directories(${TARGET} PRIVATE src)
target_link_libraries(${TARGET} PRIVATE ue5mcp_common ws2_32)
target_compile_definitions(${TARGET} PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(${TARGET} PRIVATE /W4 /utf-8 /EHsc /bigobj)
endif()
