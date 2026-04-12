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

# cpr ships with libcurl; on Windows libcurl >= 8.14 is required for proper
# certificate handling.  Detect the version through the CURL::libcurl target
# that vcpkg exposes and fail early with a clear message.
if(WIN32)
    find_package(CURL CONFIG QUIET)
    if(CURL_FOUND AND DEFINED CURL_VERSION_STRING)
        if(CURL_VERSION_STRING VERSION_LESS "8.14.0")
            message(FATAL_ERROR
                "libcurl >= 8.14.0 is required on Windows (found ${CURL_VERSION_STRING}). "
                "Please update your vcpkg checkout so that the curl port is >= 8.14.0.")
        endif()
    endif()
endif()

file(GLOB_RECURSE ACECODE_SOURCE_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)

set(ACECODE_PROJECT_FILES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_SOURCE_FILES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
)

add_executable(acecode)
target_sources(acecode PRIVATE ${ACECODE_PROJECT_FILES})

source_group(TREE ${CMAKE_SOURCE_DIR} FILES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_SOURCE_FILES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
)

source_group("generated" FILES
    ${CMAKE_BINARY_DIR}/generated/version.hpp
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

