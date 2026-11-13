cmake_minimum_required(VERSION 3.22)
project(ue5mcp LANGUAGES CXX)

option(UE5MCP_BUILD_HOST "Build the out-of-process MCP host (ue5mcp.exe)" ON)
option(UE5MCP_BUILD_BRIDGE "Build the UE4SS C++ mod (needs external/RE-UE4SS with all submodules)" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ue5mcp_common INTERFACE)
target_include_directories(ue5mcp_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(ue5mcp_common INTERFACE
    UE5MCP_VERSION="0.1.0"
    NOMINMAX WIN32_LEAN_AND_MEAN)

if(UE5MCP_BUILD_BRIDGE)
    # UE4SS brings its own build configurations (Game__Shipping__Win64 etc.) and must be configured with one.
    if(NOT CMAKE_BUILD_TYPE MATCHES "__")
        message(FATAL_ERROR "UE4SS requires -DCMAKE_BUILD_TYPE=<Target>__<Config>__Win64, e.g. Game__Shipping__Win64")
    endif()
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/RE-UE4SS/deps/first/Unreal/CMakeLists.txt)
        message(FATAL_ERROR "external/RE-UE4SS/deps/first/Unreal is empty: the UEPseudo submodule is not checked out. See docs/BUILDING.md")
    endif()
    add_subdirectory(external/RE-UE4SS)
    add_subdirectory(bridge)
endif()

if(UE5MCP_BUILD_HOST)
    add_subdirectory(host)
endif()
