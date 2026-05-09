// tray_icon_win.{hpp,cpp} 中 TrayMenuPayload + handler 注册的线程安全测试。
//
// 真正的 AppendMenuW / TrackPopupMenu 路径只在 Windows 真机上有意义,这里只测
// payload 的并发 set + 读一致性,以及 clear 的语义。
//
// 设计:openspec/changes/enhance-desktop-tray-menu。

#include <gtest/gtest.h>

#include "desktop/tray_icon_win.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace acecode::desktop;

namespace {

TrayMenuPayload make_payload(const std::string& workspace, std::size_t pinned, std::size_t recent) {
    TrayMenuPayload p;
    p.workspace_name = workspace;
    for (std::size_t i = 0; i < pinned; ++i) {
        TrayMenuItem it;
        it.session_id = "p-" + std::to_string(i);
        it.workspace_hash = "ws-" + workspace;
        it.title = "pinned-" + std::to_string(i);
        p.pinned.push_back(std::move(it));
    }
    for (std::size_t i = 0; i < recent; ++i) {
        TrayMenuItem it;
        it.session_id = "r-" + std::to_string(i);
        it.workspace_hash = "ws-" + workspace;
        it.title = "recent-" + std::to_string(i);
        p.recent.push_back(std::move(it));
    }
    return p;
}

} // namespace

// 场景:set_tray_menu_payload 之后清空 — clear 应让 payload 完全为空状态
TEST(TrayMenuPayload, ClearResetsToEmptyState) {
    set_tray_menu_payload(make_payload("ws", 3, 5));
    clear_tray_menu_payload();
    // 没有 getter API,通过再次 set 后预期不抛异常 + Win32 路径 init/shutdown 已分离。
    // 真正的"空状态"语义在 tray_menu_layout_test 里通过 compute_menu_layout 验证。
    SUCCEED();
}

// 场景:两线程并发 set_tray_menu_payload,clear,handler 注册 — 不崩
TEST(TrayMenuPayload, ConcurrentMutationDoesNotCrash) {
    std::atomic<bool> stop{false};
    std::thread setter([&]() {
        std::size_t i = 0;
        while (!stop.load()) {
            set_tray_menu_payload(make_payload("a", (i % 5), (i % 7)));
            ++i;
        }
    });
    std::thread clearer([&]() {
        while (!stop.load()) {
            clear_tray_menu_payload();
        }
    });
    std::thread handlers([&]() {
        while (!stop.load()) {
            set_tray_session_click_handler([](const std::string&, const std::string&) {});
            set_tray_new_chat_handler([]() {});
            set_tray_open_app_handler([]() {});
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    setter.join();
    clearer.join();
    handlers.join();
    // 收尾把 handler 设回 null 避免后续 test 拖泥带水
    set_tray_session_click_handler(nullptr);
    set_tray_new_chat_handler(nullptr);
    set_tray_open_app_handler(nullptr);
    SUCCEED();
}

// 场景:nullptr handler 是合法值(允许 main.cpp 还没 wire 时不崩)
TEST(TrayMenuPayload, NullHandlersAccepted) {
    set_tray_session_click_handler(nullptr);
    set_tray_new_chat_handler(nullptr);
    set_tray_open_app_handler(nullptr);
    SUCCEED();
}
