# cmake/acetui.cmake — opt-in 集成 acetui C++17 TUI 库到 acecode 仓。
#
# acetui 是为了学习与复刻 codex (codex-rs/tui) 的 TUI 渲染能力而搭的
# 独立 C++ 库,完全自研,不依赖任何第三方 TUI 框架。
#
# -DACECODE_BUILD_ACETUI=ON 开启;默认 OFF,acecode 主构建链路完全不受影响。
# 与 cmake/acecode_desktop.cmake / cmake/acecode_tuitest.cmake 同级。

if(NOT ACECODE_BUILD_ACETUI)
    return()
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/acetui)
