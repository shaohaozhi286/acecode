# cmake/acecode_desktop.cmake — desktop shell 子目标。
#
# 通过 -DACECODE_BUILD_DESKTOP=ON 开启。默认 OFF 保持现有 CI(TUI + daemon)
# 不变。
#
# 依赖外部库 webview/webview(MIT,C++ header-style,封装 WebView2/WKWebView/
# WebKitGTK)。用 FetchContent 拉源码,不进 vcpkg.json — vcpkg 索引相对上游
# 滞后,FetchContent 直接钉 GIT_TAG 更可控。
#
# Windows 平台上 webview 的 CMake 会自己处理 WebView2 SDK 的 NuGet 解析。
# 终端用户运行时仍需 WebView2 Runtime(Win11 默认装,Win10 1803+ 可装 Edge
# Evergreen Bootstrapper)。

if(NOT ACECODE_BUILD_DESKTOP)
    return()
endif()

include(FetchContent)

# webview/webview 0.12.0 是 2024 年的 release tag。如果将来需要更新版本,
# 钉到 commit hash 优先(release tag 通常对应一个 commit)。
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG        0.12.0
    GIT_SHALLOW    TRUE
)

# 关掉 webview 自带的样例 / 文档 / 测试,只编核心库。
set(WEBVIEW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_STATIC_BUILD ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(webview)

# desktop 目标自身的源文件 — 跨平台部分 + Windows-only 实现。
# 其它纯逻辑(workspace_registry / daemon_pool / url_builder / pick_active /
# daemon_supervisor / folder_picker_win)都在 acecode_testable 里,这里不重复列。
set(ACECODE_DESKTOP_SOURCES
    ${CMAKE_SOURCE_DIR}/src/desktop/main.cpp
    ${CMAKE_SOURCE_DIR}/src/desktop/web_host.cpp
)

# Windows 上,acecode-desktop 用 WIN32 子系统(无 console 黑窗)。
if(WIN32)
    add_executable(acecode-desktop WIN32 ${ACECODE_DESKTOP_SOURCES})
else()
    add_executable(acecode-desktop ${ACECODE_DESKTOP_SOURCES})
endif()

target_include_directories(acecode-desktop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# 链 acecode_testable 共用 url_builder / daemon_supervisor / utils 等。
# webview::core_static 提供 WebView2 wrapper(Windows 路径)。
# ftxui 三件套: 即使 desktop 自身不用 FTXUI,acecode_testable 里 ask_user_question_tool
# 等 TU 引用了 ftxui::Event,linker 仍需要解析这些符号(参考 tests/CMakeLists.txt
# 里 concurrent_session_writer 的相同处理)。
target_link_libraries(acecode-desktop PRIVATE
    acecode_testable
    webview::core
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# ole32 / shell32: folder_picker_win.cpp 用 IFileOpenDialog 的 COM 路径需要它们。
# folder_picker_win.cpp 在 acecode_testable 里,所以这里不必单独 link;但为了让
# acecode_unit_tests 也能链通,再在 acecode_testable 一侧添加(见根 CMakeLists)。
if(WIN32)
    target_link_libraries(acecode-desktop PRIVATE ole32 shell32)
endif()

if(MSVC)
    # MultiThreaded 与 acecode_testable 的运行时一致(MT/MTd)。
    set_target_properties(acecode-desktop PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
