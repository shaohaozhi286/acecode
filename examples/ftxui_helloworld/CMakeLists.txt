cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_WIN32 AND DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)
endif()

project(helloworld LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(ftxui CONFIG REQUIRED)

add_executable(helloworld main.cpp)

target_link_libraries(helloworld PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(MSVC)
    target_compile_options(helloworld PRIVATE /utf-8)
endif()
