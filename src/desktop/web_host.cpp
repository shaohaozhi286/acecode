#include "web_host.hpp"

#include <webview/webview.h>

#include <memory>
#include <mutex>

namespace acecode::desktop {

struct WebHost::Impl {
    explicit Impl(bool debug) : w(debug, nullptr) {}
    webview::webview w;
};

WebHost::WebHost(bool debug)  : impl_(new Impl(debug)) {}
WebHost::~WebHost() { delete impl_; }

void WebHost::set_title(const std::string& title) {
    impl_->w.set_title(title);
}
void WebHost::set_size(int width, int height) {
    impl_->w.set_size(width, height, WEBVIEW_HINT_NONE);
}
void WebHost::navigate(const std::string& url) {
    impl_->w.navigate(url);
}
void WebHost::init_script(const std::string& js) {
    impl_->w.init(js);
}
void WebHost::eval(const std::string& js) {
    // webview C++ 的 eval 内部最终走平台 InvokeScript 调用,不一定保证跨线程
    // 安全。dispatch 把 eval 调度到 webview 主循环线程,跨线程 caller 安全。
    auto js_copy = js;
    auto* w_ptr = &impl_->w;
    impl_->w.dispatch([w_ptr, js_copy] {
        w_ptr->eval(js_copy);
    });
}
void WebHost::bind(const std::string& name, SyncHandler fn) {
    impl_->w.bind(name, [fn](const std::string& req) -> std::string {
        return fn(req);
    });
}
void WebHost::run() {
    impl_->w.run();
}

void* WebHost::native_window() const {
    // basic_result<void*>: ok()/value() 接口。失败时返回 nullptr 让 folder picker
    // 用 NULL parent(对 IFileOpenDialog 是合法的,弹窗 modal 关系会缺失)。
    auto r = impl_->w.window();
    return r.ok() ? r.value() : nullptr;
}

} // namespace acecode::desktop
