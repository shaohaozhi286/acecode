cmake_minimum_required(VERSION 3.20)

# Keep MSVC runtime mode consistent with vcpkg dependencies. If the user
# didn't specify a triplet explicitly, default Windows builds to the static
# triplet so dependencies are built with MT/MTd as well.
if(CMAKE_HOST_WIN32 AND DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)
endif()

project(acecode VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: static runtime (MT/MTd), UTF-8, PDB in Release
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /Zi)
    add_link_options(
        /DEBUG
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>"
    )
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

# Crow is the HTTP/WebSocket server used by `acecode daemon`. Wired up as
# QUIET in M1 so existing builds keep working before users re-run vcpkg
# install; M2 introduces the first translation unit that consumes Crow and
# will flip this to REQUIRED.
find_package(Crow CONFIG QUIET)
if(NOT Crow_FOUND)
    message(STATUS "Crow not found yet; run `vcpkg install` to pick it up before M2.")
endif()

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

# cpp-mcp: only build the library, skip examples and tests
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_MCP_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/cpp-mcp EXCLUDE_FROM_ALL)

target_include_directories(acecode PRIVATE
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/include
    # mcp_sse_client.h transitively includes httplib.h from common/; this path
    # is scoped to acecode's TUs so it only affects mcp_manager.cpp in practice.
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/common
)

target_link_libraries(acecode PRIVATE
    cpr::cpr
    ftxui::screen
    ftxui::dom
    ftxui::component
    nlohmann_json::nlohmann_json
    mcp
)

if(Crow_FOUND)
    target_link_libraries(acecode PRIVATE Crow::Crow)
endif()

# Windows-only system libraries used by the daemon process abstraction:
# bcrypt for token RNG, advapi32 for ACL APIs in atomic_file.
if(WIN32)
    target_link_libraries(acecode PRIVATE bcrypt advapi32)
endif()

