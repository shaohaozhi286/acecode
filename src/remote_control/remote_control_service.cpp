#include "remote_control_service.hpp"

#include "network/tcp_probe.hpp"
#include "utils/logger.hpp"

// Crow 头必须在 ASIO_STANDALONE 定义之后 include(CMakeLists 已在
// acecode_testable 上 PUBLIC 定义,与 src/web/server.cpp 同款约束)。
#include <crow.h>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <thread>

namespace acecode::rc {

namespace {

// OutboundSender 默认实现:HTTP POST 到 channel bridge 暴露的 webhook。
// 显式禁用代理(CURLOPT_PROXY=""):channel bridge 在本机 loopback,走系统代理只会把
// 消息送出内网或撞上代理不可达;这里不复用 ProxyResolver。
class WebhookSender : public OutboundSender {
public:
    explicit WebhookSender(std::string url) : url_(std::move(url)) {}

    bool send(const OutboundMessage& msg, std::string* error) override {
        auto r = cpr::Post(
            cpr::Url{url_},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{outbound_message_to_json(msg).dump()},
            cpr::Proxies{{"http", ""}, {"https", ""}},
            cpr::Timeout{3000});
        if (r.error.code != cpr::ErrorCode::OK) {
            if (error) *error = r.error.message;
            return false;
        }
        if (r.status_code < 200 || r.status_code >= 300) {
            if (error) *error = "webhook returned HTTP " + std::to_string(r.status_code);
            return false;
        }
        return true;
    }

private:
    std::string url_;
};

} // namespace

std::string generate_remote_control_token() {
    std::random_device rd;
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 4; ++i) {
        oss.width(8);
        oss.fill('0');
        oss << rd();
    }
    return oss.str();
}

struct RemoteControlService::Impl {
    crow::SimpleApp app;
    std::thread server_thread;
    std::atomic<bool> server_failed{false};
    std::string server_error;
};

RemoteControlService::RemoteControlService() = default;

RemoteControlService::~RemoteControlService() {
    stop();
}

bool RemoteControlService::start(const RemoteControlOptions& opts, std::string* error) {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (running_) {
        if (error) *error = "remote control is already running";
        return false;
    }
    if (opts.token.empty()) {
        if (error) *error = "token must not be empty";
        return false;
    }
    if (opts.port <= 0 || opts.port > 65535) {
        if (error) *error = "invalid port " + std::to_string(opts.port);
        return false;
    }

    // 端口预检:已有进程在监听则立即失败,不进 Crow(Crow bind 失败只在
    // run() 里抛异常,后台线程的异常无法同步反馈给调用方)。
    auto probe = network::tcp_probe("127.0.0.1", opts.port, 300);
    if (probe.reason == network::TcpProbeReason::Ok) {
        if (error) {
            *error = "port " + std::to_string(opts.port) +
                     " is already in use — change remote_control.port in config.json";
        }
        return false;
    }

    std::shared_ptr<OutboundSender> sender;
    if (!opts.outbound_url.empty()) {
        sender = std::make_shared<WebhookSender>(opts.outbound_url);
    }
    hub_.enable(opts.token, opts.session_id, std::move(sender));

    impl_ = std::make_unique<Impl>();
    auto* impl = impl_.get();
    auto* hub = &hub_;

    CROW_ROUTE(impl->app, "/rc/health")
    ([hub]() {
        nlohmann::json j{{"ok", true}, {"enabled", hub->enabled()}};
        crow::response res(200, j.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    });

    CROW_ROUTE(impl->app, "/rc/send").methods(crow::HTTPMethod::POST)
    ([hub](const crow::request& req) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("text") || !body["text"].is_string()) {
            return crow::response(400, R"({"error":"body must be {\"text\": \"...\"}"})");
        }
        std::string token = req.get_header_value("X-ACECode-RC-Token");
        if (token.empty()) {
            if (const char* q = req.url_params.get("token")) token = q;
        }
        auto result = hub->handle_inbound(body["text"].get<std::string>(), token);
        if (result.ok()) {
            return crow::response(200, R"({"ok":true})");
        }
        int status = 400;
        switch (result.code) {
        case InboundResult::Code::BadToken: status = 401; break;
        case InboundResult::Code::Disabled:
        case InboundResult::Code::NoSession: status = 503; break;
        default: break;
        }
        nlohmann::json j{{"error", result.message}};
        return crow::response(status, j.dump());
    });

    impl->app.loglevel(crow::LogLevel::Warning);
    impl->server_thread = std::thread([impl, port = opts.port] {
        try {
            impl->app.bindaddr("127.0.0.1")
                .port(static_cast<std::uint16_t>(port))
                .multithreaded()
                .run();
        } catch (const std::exception& e) {
            impl->server_error = e.what();
            impl->server_failed = true;
        }
    });

    // 等 listener 真正可连接(预检通过后 bind 失败概率极低,这里兜底处理
    // 预检与 bind 之间的竞态)。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool reachable = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (impl_->server_failed) break;
        auto p = network::tcp_probe("127.0.0.1", opts.port, 200);
        if (p.reason == network::TcpProbeReason::Ok) {
            reachable = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reachable) {
        std::string detail = impl_->server_failed
            ? impl_->server_error
            : std::string("listener did not become reachable in time");
        impl_->app.stop();
        if (impl_->server_thread.joinable()) impl_->server_thread.join();
        impl_.reset();
        hub_.disable();
        if (error) *error = "failed to start listener on 127.0.0.1:" +
                            std::to_string(opts.port) + ": " + detail;
        return false;
    }

    running_ = true;
    port_ = opts.port;
    outbound_url_ = opts.outbound_url;
    LOG_INFO("[remote-control] listening on 127.0.0.1:" + std::to_string(opts.port));
    return true;
}

void RemoteControlService::stop() {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (impl_) {
        impl_->app.stop();
        if (impl_->server_thread.joinable()) impl_->server_thread.join();
        impl_.reset();
    }
    hub_.disable();
    running_ = false;
    port_ = 0;
    outbound_url_.clear();
}

bool RemoteControlService::running() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return running_;
}

void RemoteControlService::set_outbound_url(const std::string& url) {
    std::lock_guard<std::mutex> lk(state_mu_);
    outbound_url_ = url;
    if (!running_) return;
    if (url.empty()) {
        hub_.set_outbound_sender(nullptr);
    } else {
        hub_.set_outbound_sender(std::make_shared<WebhookSender>(url));
    }
}

RemoteControlStatusSnapshot RemoteControlService::status() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    RemoteControlStatusSnapshot snap;
    snap.running = running_;
    snap.port = port_;
    snap.token = hub_.token();
    snap.outbound_url = outbound_url_;
    snap.stats = hub_.stats();
    return snap;
}

RemoteControlService& remote_control_service() {
    static RemoteControlService instance;
    return instance;
}

} // namespace acecode::rc
