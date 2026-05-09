# acetui/CMakeLists.txt — C++17 TUI library, codex codex-rs/tui 的 C++ 复刻
# 学习项目。完全自研 ANSI escape + 平台 raw mode,不依赖 FTXUI / ncurses /
# termcap 等任何第三方 TUI 库。
#
# 模块结构对照 codex 的命名(Terminal / Viewport / InsertHistory / App /
# widgets::ChatComposer)便于对照学习;C++ 代码从头写,不翻译 Rust 源码。

cmake_minimum_required(VERSION 3.20)

set(_ACETUI_SOURCES
    src/viewport.cpp
    src/insert_history.cpp
    src/app.cpp
    src/widgets/chat_composer.cpp
)

if(WIN32)
    list(APPEND _ACETUI_SOURCES src/terminal_win.cpp)
else()
    list(APPEND _ACETUI_SOURCES src/terminal_posix.cpp)
endif()

add_library(acetui STATIC ${_ACETUI_SOURCES})

target_include_directories(acetui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(acetui PUBLIC cxx_std_17)

if(MSVC)
    set_target_properties(acetui PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(acetui PRIVATE /utf-8 /W4)
    target_compile_definitions(acetui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# 端到端 example:用 acetui API 实现一个最小聊天输入条 demo。
add_executable(hello_chat examples/hello_chat.cpp)
target_link_libraries(hello_chat PRIVATE acetui)

if(MSVC)
    set_target_properties(hello_chat PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(hello_chat PRIVATE /utf-8)
    target_compile_definitions(hello_chat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
