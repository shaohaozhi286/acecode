cmake_minimum_required(VERSION 3.20)

project(ace-browser-cli VERSION 0.1.0 LANGUAGES CXX)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

add_executable(ace-browser-cli
    src/main.cpp
)

target_compile_features(ace-browser-cli PRIVATE cxx_std_17)
target_link_libraries(ace-browser-cli PRIVATE nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(ace-browser-cli PRIVATE /utf-8)
endif()

if(WIN32)
    target_link_libraries(ace-browser-cli PRIVATE ws2_32)
endif()
