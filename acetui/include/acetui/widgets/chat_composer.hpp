// acetui/widgets/chat_composer.hpp — phase 1 最小聊天输入条 widget。
//
// 渲染为 viewport 区域内 3 行布局:上 separator + "> <buffer>" + 下 separator。
// 处理:
//   - 可见字符(KeyEvent.codepoint >= 0x20 且无 Ctrl 修饰):追加 buffer
//   - Backspace:从 buffer 末尾删一个 UTF-8 codepoint
//   - Enter:把当前 buffer 推进 viewport 上方(进 scrollback),清 buffer
//   - 其它:忽略
//
// phase 2 会把它扩为多行 / 历史 / 文件提及 / 命令补全 / 队列 / 流式 echo
// 等一系列子能力,届时拆成 chat_composer 子目录。

#pragma once

#include <functional>
#include <string>

#include "acetui/app.hpp"

namespace acetui::widgets {

class ChatComposer : public Widget {
 public:
    // ─── 外观配置(在 App::run 之前由 caller 设置) ────────────────

    // 输入行最左侧的 prompt 字符串。默认 "> ";若用 unicode 字符(如 "› ")
    // 必须同时把 prompt_display_width 设成对应的实际终端列宽。
    std::string prompt = "> ";
    int prompt_display_width = 2;

    // buffer 为空时显示的灰色提示文字。空 string 表示不显示 placeholder。
    std::string placeholder;

    // 状态栏文字:非空时占用 widget 最后一行,以 dim/灰色显示。
    std::string footer;

    // input 区与 footer 之间留几行空白。footer 为空时无效。
    int footer_top_padding = 1;

    // 旧风格:输入区上下加 ───── 横线。新风格(欢迎页风格)默认关闭。
    bool show_separators = false;

    // input 区最多占多少行;超过时只显示后 N 行(光标永远可见),
    // buffer 仍完整保留。
    int max_input_lines = 8;

    // 用户提交一行时的观察回调,在 ctx.insert_history 之前调用。
    std::function<void(const std::string& submitted)> on_submit;

    // 给 test 用:读当前 buffer 与光标 byte 偏移。
    const std::string& buffer() const { return buffer_; }
    size_t cursor_pos() const { return cursor_pos_; }

    int desired_height(int viewport_width) const override;
    EventResult on_event(const Event& e, AppContext& ctx) override;
    void render(AppContext& ctx) override;

 private:
    std::string buffer_;
    size_t      cursor_pos_ = 0;  // byte offset into buffer_
};

}  // namespace acetui::widgets
