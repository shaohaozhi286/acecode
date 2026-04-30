// 系统代理解析器 —— 平台无关核心。平台特定的 auto_detect 实现在
// proxy_resolver_posix.cpp 或 proxy_resolver_win.cpp。
//
// 设计要点见 openspec/changes/respect-system-proxy/design.md。

#include "proxy_resolver.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <sstream>

namespace acecode::network {

namespace {

std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 从 URL 抽出 scheme(小写)。"https://...".scheme = "https"。无 scheme → "".
std::string scheme_of(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return "";
    return to_lower(url.substr(0, pos));
}

// 抽出 host(去 scheme/port/path/auth)。"https://user:pw@a.b:443/path" → "a.b".
std::string host_of(const std::string& url) {
    std::string rest = url;
    auto sp = rest.find("://");
    if (sp != std::string::npos) rest = rest.substr(sp + 3);
    auto at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    auto end = rest.find_first_of("/:?#");
    if (end != std::string::npos) rest = rest.substr(0, end);
    return to_lower(rest);
}

// 从 URL 抽出 user:pass(若有)。返回 (username, password);
// 无凭据 → ("", "")。
std::pair<std::string, std::string> userinfo_of(const std::string& url) {
    auto sp = url.find("://");
    if (sp == std::string::npos) return {"", ""};
    auto rest = url.substr(sp + 3);
    auto at = rest.find('@');
    if (at == std::string::npos) return {"", ""};
    auto creds = rest.substr(0, at);
    auto colon = creds.find(':');
    if (colon == std::string::npos) return {creds, ""};
    return {creds.substr(0, colon), creds.substr(colon + 1)};
}

} // anonymous namespace

std::string redact_credentials(const std::string& url) {
    auto sp = url.find("://");
    if (sp == std::string::npos) return url;
    auto at = url.find('@', sp + 3);
    if (at == std::string::npos) return url;
    auto creds_start = sp + 3;
    auto colon = url.find(':', creds_start);
    if (colon == std::string::npos || colon >= at) {
        // user-only 凭据没有密码 → 不脱敏
        return url;
    }
    return url.substr(0, colon + 1) + "***" + url.substr(at);
}

std::string normalize_proxy_url(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return "";
    // 拒绝包含空格的 URL —— libcurl 会失败而且通常意味着用户填错了。
    if (s.find(' ') != std::string::npos) return "";

    if (s.find("://") == std::string::npos) {
        s = "http://" + s;
    }
    // 简单结构校验:scheme + host[:port],scheme 必须是 http/https/socks5 之一。
    auto sp = s.find("://");
    if (sp == 0) return ""; // "://host" 不接受
    std::string scheme = to_lower(s.substr(0, sp));
    static const std::vector<std::string> ok_schemes = {
        "http", "https", "socks4", "socks4a", "socks5", "socks5h"};
    if (std::find(ok_schemes.begin(), ok_schemes.end(), scheme) == ok_schemes.end()) {
        return "";
    }
    auto rest = s.substr(sp + 3);
    if (rest.empty()) return "";
    // 确保 host 不为空
    auto at = rest.find('@');
    auto host_start = (at != std::string::npos) ? at + 1 : 0;
    if (host_start >= rest.size()) return "";
    auto host_end = rest.find_first_of("/:?#", host_start);
    auto host = rest.substr(host_start, (host_end == std::string::npos) ? std::string::npos
                                                                         : host_end - host_start);
    if (trim(host).empty()) return "";
    return s;
}

std::vector<std::string> parse_no_proxy(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(to_lower(item));
    }
    return out;
}

bool host_matches_no_proxy(const std::string& host,
                           const std::vector<std::string>& list) {
    if (host.empty()) return false;
    std::string h = to_lower(host);
    for (const auto& raw : list) {
        if (raw == "*") return true;
        if (raw == h) return true;
        // `.foo.com` → 后缀匹配 `bar.foo.com`,但不匹配 `foo.com`
        if (!raw.empty() && raw.front() == '.') {
            if (h.size() > raw.size() && h.compare(h.size() - raw.size(), raw.size(), raw) == 0) {
                return true;
            }
            // 进一步:`.foo.com` 也接受 `foo.com` 自身(libcurl 兼容)
            std::string bare = raw.substr(1);
            if (h == bare) return true;
            continue;
        }
        // 裸名 `foo.com`:也匹配 `bar.foo.com`(libcurl 兼容)
        if (h.size() > raw.size() &&
            h[h.size() - raw.size() - 1] == '.' &&
            h.compare(h.size() - raw.size(), raw.size(), raw) == 0) {
            return true;
        }
    }
    return false;
}

void ProxyResolver::rebuild_no_proxy_list_unlocked() {
    no_proxy_list_ = parse_no_proxy(cfg_.proxy_no_proxy);
    // POSIX/Windows env 上的 NO_PROXY 也合并进来(只在 auto 模式有意义)。
    if (cfg_.proxy_mode == "auto") {
        for (const char* k : {"NO_PROXY", "no_proxy"}) {
            const char* v = std::getenv(k);
            if (v && *v) {
                auto extra = parse_no_proxy(v);
                no_proxy_list_.insert(no_proxy_list_.end(), extra.begin(), extra.end());
            }
        }
    }
}

void ProxyResolver::init(const NetworkConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    auto_cache_valid_ = false;
    session_override_ = SessionOverride::None;
    session_override_url_.clear();
    rebuild_no_proxy_list_unlocked();
}

void ProxyResolver::refresh() {
    std::lock_guard<std::mutex> lk(mu_);
    auto_cache_valid_ = false; // 下次 effective() 时重新探测
    rebuild_no_proxy_list_unlocked();
}

void ProxyResolver::set_session_override_off() {
    std::lock_guard<std::mutex> lk(mu_);
    session_override_ = SessionOverride::ForceOff;
    session_override_url_.clear();
}

void ProxyResolver::set_session_override_url(std::string url) {
    std::lock_guard<std::mutex> lk(mu_);
    session_override_ = SessionOverride::ForceManual;
    session_override_url_ = std::move(url);
}

void ProxyResolver::reset_session_override() {
    std::lock_guard<std::mutex> lk(mu_);
    session_override_ = SessionOverride::None;
    session_override_url_.clear();
}

NetworkConfig ProxyResolver::config_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

bool ProxyResolver::insecure_skip_verify() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_.proxy_insecure_skip_verify;
}

std::optional<std::string> ProxyResolver::ca_bundle() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.proxy_ca_bundle.empty()) return std::nullopt;
    return cfg_.proxy_ca_bundle;
}

ResolvedProxy ProxyResolver::resolve_auto_unlocked(const std::string& target_url) const {
    // auto_detect 是 platform-specific 实现;mu_ 必须已经 lock。
    // 这里不缓存 target_url,因为 host_of(target_url) 已被剥离 + 之后做 NO_PROXY 过滤,
    // detect 自身只看 scheme(有些平台代理是 per-scheme 的)。
    return auto_detect(cfg_, target_url);
}

ResolvedProxy ProxyResolver::effective(const std::string& target_url) const {
    std::lock_guard<std::mutex> lk(mu_);

    // 1. session override 最优先(/proxy off / /proxy set)
    if (session_override_ == SessionOverride::ForceOff) {
        return {"", "session-override:off"};
    }
    if (session_override_ == SessionOverride::ForceManual) {
        std::string norm = normalize_proxy_url(session_override_url_);
        if (norm.empty()) return {"", "none-after-error"};
        if (host_matches_no_proxy(host_of(target_url), no_proxy_list_)) {
            return {"", "no_proxy"};
        }
        return {norm, "session-override"};
    }

    // 2. config-level mode
    if (cfg_.proxy_mode == "off") {
        return {"", "mode=off"};
    }

    if (cfg_.proxy_mode == "manual") {
        std::string norm = normalize_proxy_url(cfg_.proxy_url);
        if (norm.empty()) {
            LOG_WARN("[proxy] manual proxy_url unparseable: " +
                     redact_credentials(cfg_.proxy_url));
            return {"", "none-after-error"};
        }
        if (host_matches_no_proxy(host_of(target_url), no_proxy_list_)) {
            return {"", "no_proxy"};
        }
        return {norm, "manual"};
    }

    // 3. auto: 平台代理探测(带缓存)
    // 注意 auto_detect 自身可能根据 target_url scheme 选择不同子代理,这意味着
    // 严格说 cache 应该按 (target_scheme) 分桶。当前实现简化为"探测一次,所有
    // scheme 共用同一个结果",auto_detect 内部会再根据 scheme 微调返回值。
    if (!auto_cache_valid_) {
        // const_cast 是 mutex 保护的设计权衡:effective() 是逻辑 const,
        // 但 cache 写入是物理 mutation。
        const_cast<ProxyResolver*>(this)->auto_cache_ = resolve_auto_unlocked(target_url);
        const_cast<ProxyResolver*>(this)->auto_cache_valid_ = true;
    }
    if (auto_cache_.url.empty()) {
        return auto_cache_;
    }
    if (host_matches_no_proxy(host_of(target_url), no_proxy_list_)) {
        return {"", "no_proxy"};
    }
    return auto_cache_;
}

ProxyOptions ProxyResolver::options_for(const std::string& target_url) const {
    ProxyOptions opts;
    opts.resolved = effective(target_url);

    if (opts.resolved.url.empty()) {
        // 直连:可选 ca_bundle 仍可生效(用户可能配了自定义企业 CA)
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_.proxy_ca_bundle.empty() &&
            std::filesystem::exists(cfg_.proxy_ca_bundle)) {
            opts.ca_bundle = cfg_.proxy_ca_bundle;
        }
        return opts;
    }

    // 给 cpr 同时挂上 http 和 https,libcurl 自己根据目标 scheme 选。
    std::string norm = opts.resolved.url;
    opts.proxies = cpr::Proxies{
        {"http", norm},
        {"https", norm},
    };

    auto [user, pass] = userinfo_of(norm);
    if (!user.empty()) {
        opts.auth = cpr::ProxyAuthentication{
            {"http", cpr::EncodedAuthentication{user, pass}},
            {"https", cpr::EncodedAuthentication{user, pass}},
        };
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cfg_.proxy_insecure_skip_verify) {
            opts.insecure = true;
        }
        if (!cfg_.proxy_ca_bundle.empty() &&
            std::filesystem::exists(cfg_.proxy_ca_bundle)) {
            opts.ca_bundle = cfg_.proxy_ca_bundle;
        } else if (!cfg_.proxy_ca_bundle.empty()) {
            LOG_WARN("[proxy] proxy_ca_bundle not found on disk: " + cfg_.proxy_ca_bundle);
        }
    }

    return opts;
}

ProxyResolver& proxy_resolver() {
    static ProxyResolver inst;
    return inst;
}

cpr::SslOptions build_ssl_options(const ProxyOptions& opts) {
    // 既有所有调用站点都用 cpr::Ssl(cpr::ssl::NoRevoke{true}) — 保留这个语义。
    // insecure=true(代理生效 + 用户开启逃生口) → 关闭 peer/host 校验。
    // ca_bundle 有值 → 注入 CaInfo,信任 Fiddler 等抓包工具的 root cert。
    if (opts.insecure) {
        if (opts.ca_bundle) {
            return cpr::Ssl(cpr::ssl::NoRevoke{true},
                            cpr::ssl::VerifyPeer{false},
                            cpr::ssl::VerifyHost{false},
                            cpr::ssl::CaInfo{*opts.ca_bundle});
        }
        return cpr::Ssl(cpr::ssl::NoRevoke{true},
                        cpr::ssl::VerifyPeer{false},
                        cpr::ssl::VerifyHost{false});
    }
    if (opts.ca_bundle) {
        return cpr::Ssl(cpr::ssl::NoRevoke{true},
                        cpr::ssl::CaInfo{*opts.ca_bundle});
    }
    return cpr::Ssl(cpr::ssl::NoRevoke{true});
}

std::string parse_winhttp_proxy_string(const std::string& raw,
                                       const std::string& scheme_in) {
    std::string s = trim(raw);
    if (s.empty()) return "";
    std::string scheme = to_lower(scheme_in.empty() ? "https" : scheme_in);

    // 检测是否是 `key=value;...` 格式
    if (s.find('=') == std::string::npos && s.find(';') == std::string::npos) {
        // 简单 host:port,所有 scheme 共用
        return normalize_proxy_url(s);
    }

    std::string fallback; // 没有匹配 scheme 时用第一个能用的
    std::stringstream ss(s);
    std::string entry;
    while (std::getline(ss, entry, ';')) {
        entry = trim(entry);
        if (entry.empty()) continue;
        auto eq = entry.find('=');
        if (eq == std::string::npos) {
            // 老式纯 host:port 形态 —— 当作 "都用"
            std::string n = normalize_proxy_url(entry);
            if (!n.empty() && fallback.empty()) fallback = n;
            continue;
        }
        std::string key = to_lower(trim(entry.substr(0, eq)));
        std::string val = trim(entry.substr(eq + 1));
        if (val.empty()) continue;
        std::string n = normalize_proxy_url(val);
        if (n.empty()) continue;
        if (key == scheme) return n;
        // socks key 现阶段忽略(不做隐式 socks5 升级)
        if (key == "http" || key == "https") {
            if (fallback.empty()) fallback = n;
        }
    }
    return fallback;
}

} // namespace acecode::network
