cmake_minimum_required(VERSION 3.20)

# Keep MSVC runtime mode consistent with vcpkg dependencies. If the user
# didn't specify a triplet explicitly, default Windows builds to the static
# triplet so dependencies are built with MT/MTd as well.
if(CMAKE_HOST_WIN32 AND DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)
endif()

project(acecode VERSION 0.7.4 LANGUAGES C CXX)

option(ACECODE_TUI_INPUT_TRACE "Enable verbose TUI input event trace logging." OFF)

# CTest integration: defines BUILD_TESTING (default ON). Release packaging can
# disable it with -DBUILD_TESTING=OFF to skip the test binary and the gtest
# lookup that `tests/` performs.
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: static runtime (MT/MTd), UTF-8, PDB in Release
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # The codebase intentionally uses exceptions for JSON parsing, filesystem
    # adapters, provider recovery, and daemon route validation. Without /EHsc,
    # MSVC skips standard stack-unwind semantics and those guarded paths abort
    # instead of reaching their catch blocks.
    add_compile_options(/EHsc /utf-8 /Zi /FS /bigobj)
    # /MP: 单 project 内多个 .cpp 并行编(默认关闭,串行)。Ninja 不需要也会
    # 忽略;只对 MSBuild / VS IDE 生成器有意义。不传数字 = 用全部核心,VS 里
    # 也可在命令行覆盖:cmake --build ... -- /p:CL_MPCount=28
    add_compile_options(/MP)
    add_link_options(
        /DEBUG
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>"
    )
endif()

if(NOT MSVC)
    add_compile_options(
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<CONFIG:Debug>>>:-ffunction-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<CONFIG:Debug>>>:-fdata-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-ffunction-sections>"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-fdata-sections>"
    )
    if(APPLE)
        add_link_options("$<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>")
    elseif(UNIX)
        add_link_options("$<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>")
    endif()
endif()

# Generate version header from CMake project version
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

if(WIN32)
    enable_language(RC)
    set(ACECODE_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/windows/acecode.ico")
    configure_file(
        ${CMAKE_SOURCE_DIR}/assets/windows/acecode.rc.in
        ${CMAKE_BINARY_DIR}/generated/acecode.rc
        @ONLY
    )
    set(ACECODE_WINDOWS_RESOURCES ${CMAKE_BINARY_DIR}/generated/acecode.rc)
endif()

# Embed web/ assets into acecode_testable. Generates a single .cpp with byte
# arrays + a name → span lookup map. Re-runs on every cmake re-configure when
# files in web/dist/ change (CONFIGURE_DEPENDS in the cmake function).
#
# web/ is now a Vite + React + Tailwind project; the runtime assets are the
# `web/dist/` build artifact, NOT the JSX source. If `web/dist/` is missing
# the embed function emits an empty asset map (daemon will 404 the SPA but
# keep the API working) — devs need to run `pnpm install && pnpm build`
# inside web/ once before configuring cmake. CI/release flow runs the same
# command before invoking cmake.
include(${CMAKE_SOURCE_DIR}/cmake/acecode_embed_assets.cmake)
acecode_compute_build_version(ACECODE_BUILD_VERSION)
set(ACECODE_WEB_DIST "${CMAKE_SOURCE_DIR}/web/dist")
set(ACECODE_WEB_EMBED_DIR "${ACECODE_WEB_DIST}")
if(NOT EXISTS "${ACECODE_WEB_DIST}")
    message(WARNING
        "[acecode] web/dist/ not found — embedding a minimal fallback page. "
        "Run `pnpm install && pnpm build` inside web/ before re-configuring for the full UI.")
    set(ACECODE_WEB_EMBED_DIR "${CMAKE_BINARY_DIR}/generated/web-dist-fallback")
    file(MAKE_DIRECTORY "${ACECODE_WEB_EMBED_DIR}")
    file(WRITE "${ACECODE_WEB_EMBED_DIR}/index.html"
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>ACECode</title></head>"
        "<body><main style=\"font-family: sans-serif; padding: 24px;\">"
        "<h1>ACECode Web UI assets were not built</h1>"
        "<p>Run <code>pnpm install && pnpm build</code> inside <code>web/</code>, then reconfigure CMake.</p>"
        "</main></body></html>")
endif()
acecode_generate_embedded_assets(
    ${ACECODE_WEB_EMBED_DIR}
    ${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp
    ${ACECODE_BUILD_VERSION}
)

find_package(cpr CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
if(WIN32)
    find_package(unofficial-wintoast CONFIG REQUIRED)
endif()

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

# winpty(仅 WIN32 内部生效): Win10 < 1809 无 ConPTY 时的 PTY 兜底。
# 定义 winpty_static(链进 acecode_testable)与 winpty-agent(嵌入释放)。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_winpty.cmake)

# Gather all sources under src/ once, then split them into focused build
# boundaries. Desktop and daemon share a few native bridge helpers, but the
# desktop shell must not link the broad acecode_testable object set.
file(GLOB_RECURSE ACECODE_ALL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE ACECODE_HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)

set(ACECODE_TESTABLE_SOURCES ${ACECODE_ALL_SOURCES})
list(REMOVE_ITEM ACECODE_TESTABLE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp)
list(FILTER ACECODE_TESTABLE_SOURCES EXCLUDE REGEX "/src/(tui|markdown)/")

# winpty_agent_location.cpp 属于 winpty_static(需要 winpty 内部 include 路径),
# 不能同时进 acecode_testable,否则 findAgentProgram 符号重复。
list(REMOVE_ITEM ACECODE_TESTABLE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/web/pty/winpty_agent_location.cpp)
set(ACECODE_TUI_SOURCES ${ACECODE_ALL_SOURCES})
list(FILTER ACECODE_TUI_SOURCES INCLUDE REGEX "/src/(tui|markdown)/")

set(ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/config/config.cpp
    ${CMAKE_SOURCE_DIR}/src/config/model_provider_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/config/request_headers.cpp
    ${CMAKE_SOURCE_DIR}/src/config/saved_models.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/platform_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/platform_windows.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/runtime_files.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/folder_picker_win.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/notifications_win.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/open_in_explorer.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/workspace_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/clipboard.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/cwd_hash.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/encoding.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/paths.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/state_file.cpp
)

set(ACECODE_DESKTOP_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/desktop/context_picker.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/daemon_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/daemon_supervisor.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/desktop_about.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/desktop_restart.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/dpi_win.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/edge_app_launcher.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/external_url.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/pick_active.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/single_instance_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/single_instance_win.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/tray_icon_win.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/url_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/webview2_runtime_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/window_background.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/window_chrome.cpp
)

# desktop 二进制专属(main.cpp / web_host.cpp 用 webview/webview)从 testable
# 里剔除。desktop/native helper 源文件改由 focused support target 拥有,
# 这样 acecode-desktop 不需要链接 acecode_testable。
set(ACECODE_DESKTOP_BINARY_ONLY_SOURCES
    ${CMAKE_SOURCE_DIR}/src/desktop/main.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/splash_screen.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/web_host.cpp
)
foreach(_f IN LISTS ACECODE_DESKTOP_BINARY_ONLY_SOURCES)
    list(REMOVE_ITEM ACECODE_TESTABLE_SOURCES ${_f})
endforeach()
foreach(_f IN LISTS ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES ACECODE_DESKTOP_SUPPORT_SOURCES)
    list(REMOVE_ITEM ACECODE_TESTABLE_SOURCES ${_f})
endforeach()

# Most TUI files stay out of acecode_testable. These focused helpers have no
# renderer state and are used by main.cpp plus unit tests, so compile them once
# in acecode_testable and remove them from the TUI-only source group.
set(ACECODE_TESTABLE_TUI_SOURCES
    ${CMAKE_SOURCE_DIR}/src/tui/ask_question_overlay.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/text_truncation.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/paste_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/pending_attachment_selection.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/path_reference_input.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/confirm_question.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/diff_line_wrap.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/model_picker.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/mode_picker.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/input_history_navigation.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/sidebar_model.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/subagent_host.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/theme_palette.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/thinking_heartbeat.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/tool_result_fold.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/tool_row_format.cpp
)
list(APPEND ACECODE_TESTABLE_SOURCES ${ACECODE_TESTABLE_TUI_SOURCES})
list(REMOVE_ITEM ACECODE_TUI_SOURCES ${ACECODE_TESTABLE_TUI_SOURCES})

add_library(acecode_native_bridge_support STATIC
    ${ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES}
)
target_include_directories(acecode_native_bridge_support PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acecode_native_bridge_support PUBLIC
    nlohmann_json::nlohmann_json
)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(acecode_native_bridge_support PRIVATE
        ${CMAKE_SOURCE_DIR}/src/desktop/folder_picker_mac.mm
    )
    target_link_libraries(acecode_native_bridge_support PUBLIC "-framework AppKit")
endif()
if(WIN32)
    # advapi32: restrictive ACL writes in utils/atomic_file.hpp.
    # ws2_32: daemon/runtime_files.cpp port probes.
    # ole32/shell32/user32: native folder picker and clipboard integration.
    target_link_libraries(acecode_native_bridge_support PUBLIC
        advapi32 ws2_32 ole32 shell32 user32
        unofficial::wintoast::wintoast
    )
endif()

add_library(acecode_desktop_support STATIC
    ${ACECODE_DESKTOP_SUPPORT_SOURCES}
)
target_include_directories(acecode_desktop_support PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(acecode_desktop_support PUBLIC
    acecode_native_bridge_support
)
if(APPLE)
    enable_language(OBJCXX)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/desktop/tray_icon_win.cpp
        ${CMAKE_SOURCE_DIR}/src/desktop/web_host.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/desktop/web_host.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-auto-var-id"
    )
    target_link_libraries(acecode_desktop_support PUBLIC "-framework AppKit")
elseif(UNIX)
    target_link_libraries(acecode_desktop_support PUBLIC ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    # bcrypt: desktop/daemon_supervisor.cpp generates daemon auth tokens.
    # ws2_32: readiness probes; shell/user/gdi: Edge/tray/window helpers.
    target_link_libraries(acecode_desktop_support PUBLIC
        bcrypt ws2_32 ole32 shell32 user32 gdi32 comctl32
    )
endif()

# Shared OBJECT library: one compile per source, consumed by both acecode
# (production binary) and acecode_unit_tests (test binary). PUBLIC link deps
# propagate to consumers so they get the right include dirs + libraries.
add_library(acecode_testable OBJECT
    ${ACECODE_TESTABLE_SOURCES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    ${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp
)
if(APPLE)
    enable_language(OBJCXX)
endif()
target_include_directories(acecode_testable PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/include
    ${CMAKE_SOURCE_DIR}/external/cpp-mcp/common
)
target_link_libraries(acecode_testable PUBLIC
    cpr::cpr
    ftxui::screen
    ftxui::dom
    ftxui::component
    libzip::zip
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
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
    # ws2_32: src/desktop/daemon_supervisor.cpp 用 winsock 探测端口连通性。
    # ole32/shell32/user32: src/desktop/folder_picker_win.cpp 的 COM + IFileOpenDialog + owner/foreground handling。
    target_link_libraries(acecode_testable PUBLIC bcrypt advapi32 winhttp ws2_32 ole32 shell32 user32)
    # winpty 客户端库(< 1809 的 PTY 兜底,cmake/acecode_winpty.cmake)。
    target_link_libraries(acecode_testable PUBLIC winpty_static)
endif()
if(UNIX AND NOT APPLE)
    # forkpty(src/web/pty/pty_backend_posix.cpp)。glibc 2.34+ 已并入 libc,
    # 老 glibc / musl 仍需显式链 util;find_library 找不到就交给 libc。
    find_library(ACECODE_LIBUTIL util)
    if(ACECODE_LIBUTIL)
        target_link_libraries(acecode_testable PUBLIC ${ACECODE_LIBUTIL})
    endif()
endif()

add_executable(acecode
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${ACECODE_TUI_SOURCES}
    ${ACECODE_WINDOWS_RESOURCES}
)
target_link_libraries(acecode PRIVATE
    acecode_testable
    acecode_native_bridge_support
    ftxui::screen
    ftxui::dom
    ftxui::component
)
target_compile_definitions(acecode PRIVATE
    $<$<BOOL:${ACECODE_TUI_INPUT_TRACE}>:ACECODE_TUI_INPUT_TRACE=1>
)

source_group(TREE ${CMAKE_SOURCE_DIR} FILES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${ACECODE_ALL_SOURCES}
    ${ACECODE_HEADER_FILES}
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
)

source_group("generated" FILES
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    ${ACECODE_WINDOWS_RESOURCES}
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

add_subdirectory(ace-browser-host)

# desktop shell 子目标(opt-in,默认 OFF)。include 后只有当 ACECODE_BUILD_DESKTOP=ON
# 才会真添加 acecode-desktop 这个 target,否则文件本身做无操作 return。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_desktop.cmake)

# tuitest helloworld 子目标(opt-in,默认 OFF)。-DACECODE_BUILD_TUITEST=ON 开启。
include(${CMAKE_SOURCE_DIR}/cmake/acecode_tuitest.cmake)

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
