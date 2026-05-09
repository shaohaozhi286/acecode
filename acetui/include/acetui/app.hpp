// acetui/app.hpp — 主循环 + Widget 接口。
//
// App 是 phase 1 唯一的 "驱动器":拉取事件 → 分发给 widget → 决定重画。
// 不开后台线程,不开异步,纯 blocking 单线程。phase 4 引入 ASCII 动画
// 时再考虑加 timeout-based redraw 调度。
//
// Widget 是简单的纯虚接口,phase 1 只有 ChatComposer 一个实现。
// 多 widget 组合 / Layout / Constraint 等留给 phase 2 BottomPane 一起做。

#pragma once

#include <string>
#include <vector>

#include "acetui/event.hpp"
#include "acetui/viewport.hpp"

namespace acetui {

class AppContext;

enum class EventResult {
    // widget 没什么要做的;主循环继续等下一事件,不触发重画。
    Continue,
    // widget 状态变了,App 应当调用 render。
    Redraw,
    // widget 主动请求退出 App::run,run 干净返回 0。
    ExitRequested,
};

class Widget {
 public:
    virtual ~Widget() = default;

    // widget 期望占据 viewport 的高度(行数),给定可用宽度。每个 redraw
    // 前由 App 重新调用一次 — 返回值变化会触发 viewport 上下伸缩
    // (上方 scrollback 会自然让出 / 收回空间)。默认 3 行,与宽度无关。
    virtual int desired_height(int viewport_width) const {
        (void)viewport_width;
        return 3;
    }

    // 处理一个事件,返回下一步动作。
    virtual EventResult on_event(const Event& e, AppContext& ctx) = 0;

    // 把当前状态画到 viewport(由 ctx.viewport 给出)。完成后 cursor 应当
    // 落在 widget 内部一个对用户友好的位置(比如输入光标位置)。
    virtual void render(AppContext& ctx) = 0;
};

class AppContext {
 public:
    // 当前 widget 的渲染区域。resize 时 App 会更新这个值再调 render。
    Viewport viewport;

    // 把这些行推进 viewport 上方,自然进入 scrollback。widget 通常在
    // 处理 Enter 之类 "提交" 事件时调用此函数。
    void insert_history(const std::vector<std::string>& lines) const;

    // 主动请求退出主循环。等价于 on_event 返回 ExitRequested,但允许
    // widget 在 render 路径上请求退出(罕见用法)。
    void request_exit();

    // 内部状态,由 App 维护。
    bool exit_requested = false;
};

class App {
 public:
    // 阻塞运行主循环。返回 0 = 干净退出;返回非 0 = 启动失败
    // (例如 Terminal::enable_raw 失败)。run 返回前必然恢复 console mode。
    //
    // initial_history:viewport 建立 + 撞屏底之后、首次 widget render 之前
    // 一次性 push 到 viewport 上方的内容,直接进入 scrollback。常见用途:
    // 显示启动欢迎卡片 / tip / 提示链接等首屏内容。
    int run(Widget& widget,
            const std::vector<std::string>& initial_history = {});
};

}  // namespace acetui
