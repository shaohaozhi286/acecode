# cmake/acecode_tuitest.cmake — raw-mode + DECSTBM scroll-region demo。
#
# 通过 -DACECODE_BUILD_TUITEST=ON 开启。默认 OFF,正常构建链路不受影响。
#
# 这个 demo 不依赖 FTXUI / ratatui 等 TUI 框架。目的是把 codex
# (codex-rs/tui/src/insert_history.rs) 的 "scroll region + reverse-index"
# 渲染策略在 Win32 + ANSI 上验证一遍:viewport(底部 3 行 widget)永远
# 贴底,Enter 提交时通过 DECSTBM 让 viewport 上方那一片向上滚一行,在
# 底端腾出位置写新历史。widget 字符不被任何 escape 序列覆盖 —— 没有
# erase + redraw 抖动,鼠标滚轮 / 选中复制全部由终端原生处理。
#
# 实现完全在 tuitest/main.cpp 内,不需要任何外部库 link。

if(NOT ACECODE_BUILD_TUITEST)
    return()
endif()

add_executable(tuitest
    ${CMAKE_SOURCE_DIR}/tuitest/main.cpp
)

if(MSVC)
    set_target_properties(tuitest PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
