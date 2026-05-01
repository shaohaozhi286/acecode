// 覆盖 src/config/config.{hpp,cpp} 中 web_search 段的解析与默认值。
//
// 与 config_tui_test 一致 —— load_config 在非法值时会 std::exit(1),
// 直接调 load_config 不便覆盖错误路径。这里把 from_json 的核心校验逻辑
// 复刻到 apply_web_search_section() 以便正常用例 + 错误用例同表测试。
//
// 覆盖项:
//   - WebSearchConfig 默认值(enabled=true / backend="auto" / max_results=5 /
//     timeout_ms=8000 / api_key="")
//   - 缺失段 → 默认值
//   - 5 个合法 backend 字符串都被接受
//   - 非法 backend 名报错并列出可选值
//   - max_results 越界(0 / 11)报错
//   - timeout_ms 越界(999 / 30001)报错
//   - 边界值合法(max_results=1, 10;timeout_ms=1000, 30000)
//   - 非对象段被忽略

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>
#include <string>

using namespace acecode;

namespace {

// 复刻 load_config() 里 web_search 段的解析 + 校验逻辑。
// 错误路径返回 error message(非空 = 解析失败 = 真实 load_config 会 std::exit);
// 成功返回空字符串。模拟 std::exit 不会真正退出测试进程。
std::string apply_web_search_section(const nlohmann::json& j_with_web_search,
                                     WebSearchConfig& out) {
    if (!j_with_web_search.contains("web_search")) return {};
    const auto& wsj = j_with_web_search["web_search"];
    if (!wsj.is_object()) return {};

    if (wsj.contains("enabled") && wsj["enabled"].is_boolean())
        out.enabled = wsj["enabled"].get<bool>();
    if (wsj.contains("backend") && wsj["backend"].is_string()) {
        std::string b = wsj["backend"].get<std::string>();
        if (b != "auto" && b != "duckduckgo" && b != "bing_cn" &&
            b != "bochaai" && b != "tavily") {
            return "web_search.backend='" + b +
                   "' invalid; expected one of: auto, duckduckgo, bing_cn, bochaai, tavily";
        }
        out.backend = std::move(b);
    }
    if (wsj.contains("api_key") && wsj["api_key"].is_string())
        out.api_key = wsj["api_key"].get<std::string>();
    if (wsj.contains("max_results") && wsj["max_results"].is_number_integer()) {
        int v = wsj["max_results"].get<int>();
        if (v < 1 || v > 10) {
            return "web_search.max_results=" + std::to_string(v) + " out of range (1..10)";
        }
        out.max_results = v;
    }
    if (wsj.contains("timeout_ms") && wsj["timeout_ms"].is_number_integer()) {
        int v = wsj["timeout_ms"].get<int>();
        if (v < 1000 || v > 30000) {
            return "web_search.timeout_ms=" + std::to_string(v) +
                   " out of range (1000..30000)";
        }
        out.timeout_ms = v;
    }
    return {};
}

} // namespace

// 场景:WebSearchConfig 结构体的默认值符合 spec
TEST(ConfigWebSearchDefaults, StructDefault) {
    WebSearchConfig c;
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.backend, "auto");
    EXPECT_EQ(c.api_key, "");
    EXPECT_EQ(c.max_results, 5);
    EXPECT_EQ(c.timeout_ms, 8000);
}

// 场景:AppConfig 自带 web_search,默认值同上
TEST(ConfigWebSearchDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_TRUE(cfg.web_search.enabled);
    EXPECT_EQ(cfg.web_search.backend, "auto");
    EXPECT_EQ(cfg.web_search.max_results, 5);
    EXPECT_EQ(cfg.web_search.timeout_ms, 8000);
}

// 场景:config.json 缺少 web_search 段 → 默认值,无错误
TEST(ConfigWebSearchLoader, MissingBlockKeepsDefault) {
    WebSearchConfig c;
    nlohmann::json j = nlohmann::json::object();
    EXPECT_EQ(apply_web_search_section(j, c), "");
    EXPECT_EQ(c.backend, "auto");
}

// 场景:5 个合法 backend 名都被原样接受
TEST(ConfigWebSearchLoader, AcceptsAllValidBackends) {
    for (const std::string& v : {"auto", "duckduckgo", "bing_cn", "bochaai", "tavily"}) {
        WebSearchConfig c;
        nlohmann::json j = {{"web_search", {{"backend", v}}}};
        EXPECT_EQ(apply_web_search_section(j, c), "") << "backend=" << v;
        EXPECT_EQ(c.backend, v);
    }
}

// 场景:非法 backend 名报错,错误信息列出全部 5 个可选值
TEST(ConfigWebSearchLoader, InvalidBackendRejected) {
    WebSearchConfig c;
    nlohmann::json j = {{"web_search", {{"backend", "google"}}}};
    auto err = apply_web_search_section(j, c);
    EXPECT_NE(err.find("google"), std::string::npos);
    EXPECT_NE(err.find("auto"), std::string::npos);
    EXPECT_NE(err.find("duckduckgo"), std::string::npos);
    EXPECT_NE(err.find("bing_cn"), std::string::npos);
    EXPECT_NE(err.find("bochaai"), std::string::npos);
    EXPECT_NE(err.find("tavily"), std::string::npos);
}

// 场景:大小写敏感 — "Auto" 被视为非法
TEST(ConfigWebSearchLoader, BackendCaseSensitive) {
    WebSearchConfig c;
    nlohmann::json j = {{"web_search", {{"backend", "Auto"}}}};
    EXPECT_NE(apply_web_search_section(j, c), "");
}

// 场景:max_results 越界报错(0 < 1, 11 > 10)
TEST(ConfigWebSearchLoader, MaxResultsOutOfRangeRejected) {
    for (int bad : {0, -1, 11, 100}) {
        WebSearchConfig c;
        nlohmann::json j = {{"web_search", {{"max_results", bad}}}};
        auto err = apply_web_search_section(j, c);
        EXPECT_NE(err, "") << "bad value=" << bad;
        EXPECT_NE(err.find("max_results"), std::string::npos);
        EXPECT_NE(err.find("1..10"), std::string::npos);
    }
}

// 场景:max_results 边界值合法(1 / 10)
TEST(ConfigWebSearchLoader, MaxResultsBoundariesAccepted) {
    for (int good : {1, 5, 10}) {
        WebSearchConfig c;
        nlohmann::json j = {{"web_search", {{"max_results", good}}}};
        EXPECT_EQ(apply_web_search_section(j, c), "") << "good value=" << good;
        EXPECT_EQ(c.max_results, good);
    }
}

// 场景:timeout_ms 越界报错(< 1000 或 > 30000)
TEST(ConfigWebSearchLoader, TimeoutOutOfRangeRejected) {
    for (int bad : {0, 999, 30001, 60000}) {
        WebSearchConfig c;
        nlohmann::json j = {{"web_search", {{"timeout_ms", bad}}}};
        auto err = apply_web_search_section(j, c);
        EXPECT_NE(err, "") << "bad value=" << bad;
        EXPECT_NE(err.find("timeout_ms"), std::string::npos);
    }
}

// 场景:timeout_ms 边界值合法(1000 / 30000)
TEST(ConfigWebSearchLoader, TimeoutBoundariesAccepted) {
    for (int good : {1000, 8000, 30000}) {
        WebSearchConfig c;
        nlohmann::json j = {{"web_search", {{"timeout_ms", good}}}};
        EXPECT_EQ(apply_web_search_section(j, c), "") << "good value=" << good;
        EXPECT_EQ(c.timeout_ms, good);
    }
}

// 场景:enabled 字段同时被 from_json + to_json 正确处理
TEST(ConfigWebSearchLoader, EnabledFalseAccepted) {
    WebSearchConfig c;
    nlohmann::json j = {{"web_search", {{"enabled", false}}}};
    EXPECT_EQ(apply_web_search_section(j, c), "");
    EXPECT_FALSE(c.enabled);
}

// 场景:api_key 字段透传
TEST(ConfigWebSearchLoader, ApiKeyAccepted) {
    WebSearchConfig c;
    nlohmann::json j = {{"web_search", {{"api_key", "secret-token-12345"}}}};
    EXPECT_EQ(apply_web_search_section(j, c), "");
    EXPECT_EQ(c.api_key, "secret-token-12345");
}

// 场景:web_search 不是对象(数组 / 字符串 / 数字)→ 静默忽略,默认值不变
TEST(ConfigWebSearchLoader, NonObjectSectionIgnored) {
    WebSearchConfig c;
    {
        nlohmann::json j = {{"web_search", "auto"}};
        EXPECT_EQ(apply_web_search_section(j, c), "");
        EXPECT_EQ(c.backend, "auto");
    }
    {
        nlohmann::json j = {{"web_search", 42}};
        EXPECT_EQ(apply_web_search_section(j, c), "");
    }
    {
        nlohmann::json j = {{"web_search", nlohmann::json::array({"x"})}};
        EXPECT_EQ(apply_web_search_section(j, c), "");
    }
}
