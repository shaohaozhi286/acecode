#pragma once

// 系统代理解析器(openspec/changes/respect-system-proxy)。
//
// 职责:把 NetworkConfig + 平台代理来源(Windows: WinHTTP/注册表/env;
// POSIX: env)折叠成 cpr::Proxies / cpr::ProxyAuthentication / SslOptions,
// 让所有 cpr 调用站点一行代码就能让 Fiddler/Charles/mitmproxy 拦截到流量。
//
// 调用约定:
//   network::proxy_resolver().init(cfg.network)   // 启动一次
//   auto opts = network::proxy_options_for(url);  // 每次请求
//   cpr::Post(cpr::Url{url}, opts.proxies, opts.auth, build_ssl_options(opts), ...)

#include "../config/config.hpp"

#include <cpr/cprtypes.h>
#include <cpr/proxies.h>
#include <cpr/proxyauth.h>
#include <cpr/ssl_options.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::network {

// 一次解析的结果。url 为空 = 直连。source 描述来源(用于 /proxy 命令显示):
//   "windows-system" / "windows-registry" / "env:HTTPS_PROXY" / "manual" /
//   "mode=off" / "session-override" / "none" / "none-after-error"
struct ResolvedProxy {
    std::string url;
    std::string source;
};

// cpr 一次性 API 不接受动态参数包,所以打包成结构让调用点 splat。
struct ProxyOptions {
    cpr::Proxies proxies;             // 空 = 直连
    cpr::ProxyAuthentication auth;    // 空 = 无 user:pass
    bool insecure = false;             // 仅在代理生效且配置开启时为 true
    std::optional<std::string> ca_bundle; // 可选 PEM 路径
    ResolvedProxy resolved;            // 调试用:实际生效的代理
};

// 凭据脱敏:`http://user:pass@host` → `http://user:***@host`。
// 用于横幅 / 日志 / `/proxy` 输出。空 / 不可解析 → 原样返回。
std::string redact_credentials(const std::string& url);

// 规范化代理 URL:补 `http://` scheme;失败返回空字符串。
// 不接受空白 / 含空格 / 不可解析的 host:port。
std::string normalize_proxy_url(const std::string& raw);

// 把逗号分隔的 NO_PROXY 字符串切成 trim 过的非空 token 列表。
std::vector<std::string> parse_no_proxy(const std::string& csv);

// 对 host(目标主机名,不带 scheme/port)做 NO_PROXY 匹配。规则:
//   - 完全匹配:`foo.com`  匹配 `foo.com`
//   - 后缀匹配:`.foo.com` 匹配 `bar.foo.com`(不匹配 `foo.com` 自身)
//   - 裸名后缀:`foo.com`  也匹配 `bar.foo.com`(libcurl 行为)
//   - 通配符  :`*` 单独存在 = 全部 host 都绕过代理
bool host_matches_no_proxy(const std::string& host,
                           const std::vector<std::string>& list);

class ProxyResolver {
public:
    ProxyResolver() = default;

    // 启动时调一次。线程安全(整个生命周期内 NetworkConfig 视为不可变)。
    void init(const NetworkConfig& cfg);

    // 重新探测 auto 模式下的系统代理。manual / off 模式下是 no-op。
    void refresh();

    // 解析当前生效代理。target_url 用于:(a) 选择 http/https 子代理、
    // (b) 跑 NO_PROXY 匹配。空 URL 视作 https。
    ResolvedProxy effective(const std::string& target_url) const;

    // 把 effective() 翻译为 cpr 参数。failed normalization 等错误已在
    // effective() 阶段处理完,此函数不会 throw。
    ProxyOptions options_for(const std::string& target_url) const;

    // 会话级 override —— 不持久化,进程退出即丢。
    // /proxy off / /proxy set <url> / /proxy reset 各对应一个。
    void set_session_override_off();
    void set_session_override_url(std::string url);
    void reset_session_override();

    // 调试 / `/proxy` 命令使用的快照。
    NetworkConfig config_snapshot() const;
    bool insecure_skip_verify() const;
    std::optional<std::string> ca_bundle() const;

private:
    enum class SessionOverride { None, ForceOff, ForceManual };

    mutable std::mutex mu_;
    NetworkConfig cfg_;
    // auto 模式下缓存的系统代理探测结果。manual / off 不写这个字段。
    ResolvedProxy auto_cache_;
    bool auto_cache_valid_ = false;

    SessionOverride session_override_ = SessionOverride::None;
    std::string session_override_url_;
    std::vector<std::string> no_proxy_list_;

    void rebuild_no_proxy_list_unlocked();
    ResolvedProxy resolve_auto_unlocked(const std::string& target_url) const;
};

// 进程级单例。Meyers singleton + 内部 mutex,跨线程安全。
ProxyResolver& proxy_resolver();

// 调用方便函数:`auto opts = network::proxy_options_for(url);`
inline ProxyOptions proxy_options_for(const std::string& url) {
    return proxy_resolver().options_for(url);
}

// 把 ProxyOptions 翻译成 cpr::SslOptions。保留既有 NoRevoke{true};
// insecure=true → 关闭 VerifyPeer/VerifyHost;ca_bundle 有值 → 注入 CaInfo。
// 注意:insecure 的 verify-skip 仅在 opts.proxies 非空时生效(由调用方保证)。
cpr::SslOptions build_ssl_options(const ProxyOptions& opts);

// --- 仅为 Windows 平台暴露,Linux/macOS 上看不见也用不到 ---
// 解析 `ProxyServer` 的 WinHTTP/IE 字符串格式:
//   `host:port`                    → http+https 都用同一个
//   `http=p1:80;https=p2:443`      → 按 target scheme 匹配
//   `socks=p:1080;https=p2:443`    → socks key 现阶段忽略
// scheme 入参为 `"http"` 或 `"https"`。返回空 = 不可解析 / 该 scheme 无配置。
std::string parse_winhttp_proxy_string(const std::string& raw,
                                       const std::string& scheme);

// --- 平台特定 auto 探测 ---
// posix 实现走 env;windows 实现走 winhttp → registry → env。两个函数都
// 只读全局状态(env 变量 / 注册表),不写。失败 → ResolvedProxy{"", "none"}。
ResolvedProxy auto_detect(const NetworkConfig& cfg, const std::string& target_url);

} // namespace acecode::network
