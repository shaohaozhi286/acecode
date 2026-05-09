// acetui/examples/hello_chat.cpp — 欢迎页风格的最小聊天 demo。
//
// 启动:屏幕底部出现单行 "› " 输入条,buffer 为空时显示灰色 placeholder
// "这里输入内容";输入条下方一行空白,再下一行是 dim 状态栏。
// scrollback 顶端预先 push 一张欢迎卡片 + tip + 链接行,模拟启动首屏。
//
// Enter 提交内容自然进 scrollback;Esc / Ctrl+C 干净退出。

#include <string>
#include <vector>

#include <acetui/app.hpp>
#include <acetui/widgets/chat_composer.hpp>

int main() {
    acetui::widgets::ChatComposer composer;
    composer.prompt               = "› ";
    composer.prompt_display_width = 2;
    composer.placeholder          = "这里输入内容";
    composer.footer               = "gpt-5.5 xhigh · C:\\test · 5h 98% · weekly";
    composer.footer_top_padding   = 1;
    composer.show_separators      = false;

    // 启动时一次性 push 进 scrollback 的欢迎内容(每行不超过终端宽度时
    // 显示完整;窄终端会被终端 auto-wrap)。
    std::vector<std::string> welcome = {
        "┌──────────────────────────────────────────────────┐",
        "│ ACECode  (v0.1.9)                                │",
        "│                                                  │",
        "│ model:     gpt-5.5 xhigh   /model to change      │",
        "│ directory: C:\\test                               │",
        "└──────────────────────────────────────────────────┘",
        "",
        "  Tip: ACECode 已经推出 desktop 版本,点击这里了解更多.",
        "",
        "  Learn more: https://acecode.com/introducing",
        "",
    };

    acetui::App app;
    return app.run(composer, welcome);
}
