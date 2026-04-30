cmake_minimum_required(VERSION 3.20)

# Keep MSVC runtime mode consistent with vcpkg dependencies. If the user
# didn't specify a triplet explicitly, default Windows builds to the static
# triplet so dependencies are built with MT/MTd as well.
if(CMAKE_HOST_WIN32 AND DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)
endif()

project(acecode VERSION 0.1.2 LANGUAGES C CXX)

# CTest integration: defines BUILD_TESTING (default ON). Release packaging can
# disable it with -DBUILD_TESTING=OFF to skip the test binary and the gtest
# lookup that `tests/` performs.
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: static runtime (MT/MTd), UTF-8, PDB in Release
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /Zi /FS)
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

# Embed web/ assets into acecode_testable. Generates a single .cpp with byte
# arrays + a name → span lookup map. Re-runs on every cmake re-configure when
# files in web/ change (CONFIGURE_DEPENDS in the cmake function).
include(${CMAKE_SOURCE_DIR}/cmake/acecode_embed_assets.cmake)
acecode_compute_build_version(ACECODE_BUILD_VERSION)
acecode_generate_embedded_assets(
    ${CMAKE_SOURCE_DIR}/web
    ${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp
    ${ACECODE_BUILD_VERSION}
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

# cpp-mcp: only build the library, skip examples and tests
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_MCP_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/cpp-mcp EXCLUDE_FROM_ALL)

# Gather all sources under src/ once, then split them into:
#   - ACECODE_TESTABLE_SOURCES: headless logic that unit tests can link against
#   - ACECODE_TUI_SOURCES     : FTXUI-coupled code (src/tui, src/markdown)
# main.cpp always stays out of acecode_testable so tests never drag in the
# full TUI entry point.
file(GLOB_RECURSE ACECODE_ALL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)

set(ACECODE_TESTABLE_SOURCES ${ACECODE_ALL_SOURCES})
list(FILTER ACECODE_TESTABLE_SOURCES EXCLUDE REGEX "/src/(tui|markdown)/")
set(ACECODE_TUI_SOURCES ${ACECODE_ALL_SOURCES})
list(FILTER ACECODE_TUI_SOURCES INCLUDE REGEX "/src/(tui|markdown)/")

# Most TUI files depend on FTXUI and stay out of acecode_testable. This helper
# is pure string logic used by main.cpp and unit tests, so compile it once in
# acecode_testable and remove it from the TUI-only source group.
set(ACECODE_TESTABLE_TUI_SOURCES
    ${CMAKE_SOURCE_DIR}/src/tui/text_truncation.cpp
)
list(APPEND ACECODE_TESTABLE_SOURCES ${ACECODE_TESTABLE_TUI_SOURCES})
list(REMOVE_ITEM ACECODE_TUI_SOURCES ${ACECODE_TESTABLE_TUI_SOURCES})

# Shared OBJECT library: one compile per source, consumed by both acecode
# (production binary) and acecode_unit_tests (test binary). PUBLIC link deps
# propagate to consumers so they get the right include dirs + libraries.
add_library(acecode_testable OBJECT
    ${ACECODE_TESTABLE_SOURCES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    ${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp
)
target_include_directories(acecode_testable PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/common
)
target_link_libraries(acecode_testable PUBLIC
    cpr::cpr
    nlohmann_json::nlohmann_json
    mcp
)
if(Crow_FOUND)
    target_link_libraries(acecode_testable PUBLIC Crow::Crow)
    # Crow 默认走 standalone asio，但只在它自己的 socket_adaptors.h 里 #define
    # ASIO_STANDALONE。如果某个 TU 先 include <asio.hpp> 再 include <crow.h>，
    # asio 头会先被以 boost-fallback 模式编译，触发 boost/asio.hpp not found。
    # PUBLIC 显式定义消除 include 顺序依赖。
    target_compile_definitions(acecode_testable PUBLIC ASIO_STANDALONE)
endif()
if(WIN32)
    target_link_libraries(acecode_testable PUBLIC bcrypt advapi32 winhttp)
endif()

add_executable(acecode
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_TUI_SOURCES}
)
target_link_libraries(acecode PRIVATE
    acecode_testable
    ftxui::screen
    ftxui::dom
    ftxui::component
)

source_group(TREE ${CMAKE_SOURCE_DIR} FILES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${ACECODE_ALL_SOURCES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
)

source_group("generated" FILES
    ${CMAKE_BINARY_DIR}/generated/version.hpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Bundled models.dev snapshot. Installed alongside the executable so
# find_models_dev_dir() can resolve <exe_dir>/../share/acecode/models_dev.
if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/api.json)
    install(FILES
        ${CMAKE_SOURCE_DIR}/assets/models_dev/api.json
        DESTINATION share/acecode/models_dev
    )
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/MANIFEST.json)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/models_dev/MANIFEST.json
            DESTINATION share/acecode/models_dev)
    endif()
    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/models_dev/LICENSE)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/models_dev/LICENSE
            DESTINATION share/acecode/models_dev)
    endif()
endif()

# Default first-run skill seed bundle. Installed alongside the executable so
# find_default_skill_seed_dir() can resolve <exe_dir>/../share/acecode/seed/skills.
if(EXISTS ${CMAKE_SOURCE_DIR}/assets/seed/skills)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/seed/
        DESTINATION share/acecode/seed
    )
endif()
