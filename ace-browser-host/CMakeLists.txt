cmake_minimum_required(VERSION 3.20)

project(ace-browser-host VERSION 0.1.0 LANGUAGES CXX)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)

add_executable(ace-browser-host
    src/main.cpp
)

set_target_properties(ace-browser-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)

target_compile_features(ace-browser-host PRIVATE cxx_std_17)
target_link_libraries(ace-browser-host PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(ace-browser-host PRIVATE stdc++fs)
endif()

if(MSVC)
    target_compile_options(ace-browser-host PRIVATE /utf-8)
endif()

if(WIN32)
    target_link_libraries(ace-browser-host PRIVATE ws2_32 shell32)
endif()
