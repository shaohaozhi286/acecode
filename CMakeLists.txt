cmake_minimum_required(VERSION 3.20)
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "" FORCE)
project(acecode VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force UTF-8 source and execution charset on MSVC
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Generate version header from CMake project version
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

find_package(cpr CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(acecode 
    main.cpp
    src/config/config.cpp
    src/provider/openai_provider.cpp
    src/provider/copilot_provider.cpp
    src/provider/provider_factory.cpp
    src/auth/github_auth.cpp
    src/tool/tool_executor.cpp
    src/tool/bash_tool.cpp
    src/tool/file_read_tool.cpp
    src/tool/file_write_tool.cpp
    src/tool/file_edit_tool.cpp
    src/tool/grep_tool.cpp
    src/tool/glob_tool.cpp
    src/tool/diff_utils.cpp
    src/tool/mtime_tracker.cpp
    src/prompt/system_prompt.cpp
    src/agent_loop.cpp
    src/commands/configure.cpp
)

target_include_directories(acecode PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(acecode PRIVATE 
    cpr::cpr 
    ftxui::screen 
    ftxui::dom 
    ftxui::component
    nlohmann_json::nlohmann_json
)

