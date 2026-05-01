// 覆盖 src/commands/websearch_command.{hpp,cpp} 的纯函数路径。
//
// dispatch_websearch_subcommand 依赖 web_search::runtime() 单例,这里通过
// init/shutdown 配合 set_state_file_path_for_test 把状态隔离到测试临时目录,
// 用注入式 RegionDetector 跳过真实网络。
//
// 覆盖项:
//   - bare 命令显示状态行(active backend / region / config backend)
//   - refresh 重新探测后输出 before/after 对比
//   - use 切到合法 backend(成功)
//   - use 拒绝未实现 backend(bochaai/tavily)
//   - use 拒绝未知 backend
//   - reset 回 cfg 推导
//   - 未初始化运行时 → 提示而不 crash
//   - format_websearch_status 渲染基本字段

#include <gtest/gtest.h>

#include "commands/websearch_command.hpp"
#include "tool/web_search/backend.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/web_search/runtime.hpp"
#include "config/config.hpp"
#include "utils/state_file.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace acecode;
using namespace acecode::web_search;

namespace {

class MockBackend : public WebSearchBackend {
public:
    explicit MockBackend(std::string n) : name_(std::move(n)) {}
    std::string name() const override { return name_; }
    bool requires_api_key() const override { return false; }
    std::variant<SearchResponse, SearchError>
    search(std::string_view, int,
           const std::atomic<bool>*) override {
        SearchResponse r;
        r.backend_name = name_;
        return r;
    }
    std::string name_;
};

class WebSearchCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto tmp = fs::temp_directory_path() /
                   ("acecode_ws_cmd_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        set_state_file_path_for_test(path_);
        // 干净起点:确保未初始化
        web_search::shutdown();
    }
    void TearDown() override {
        web_search::shutdown();
        set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

// 把 detector 替换为脚本化探针,并把 ddg/bing mock backend 注入 router。
void install_runtime_with_mocks(const WebSearchConfig& cfg,
                                std::vector<ProbeResult> probe_script) {
    web_search::init(cfg);
    auto& rt = web_search::runtime();
    rt.router().register_backend(std::make_unique<MockBackend>("duckduckgo"));
    rt.router().register_backend(std::make_unique<MockBackend>("bing_cn"));
    auto idx = std::make_shared<std::size_t>(0);
    auto sc = std::make_shared<std::vector<ProbeResult>>(std::move(probe_script));
    rt.detector().set_probe_for_test(
        [sc, idx](const std::string&, const std::string&, int) -> ProbeResult {
            if (*idx >= sc->size()) return ProbeResult{0, "exhausted"};
            return (*sc)[(*idx)++];
        });
}

} // namespace

// === pure-function tests ===

TEST(WebSearchFormat, StatusContainsAllFields) {
    WebSearchDisplaySnapshot snap;
    snap.enabled = true;
    snap.active_backend = "duckduckgo";
    snap.config_backend = "auto";
    snap.region = "global";
    snap.detected_at_ms = 1714521600000LL;
    snap.registered = {"bing_cn", "duckduckgo"};
    auto out = format_websearch_status(snap);
    EXPECT_NE(out.find("Web Search"), std::string::npos);
    EXPECT_NE(out.find("Enabled         : yes"), std::string::npos);
    EXPECT_NE(out.find("Active backend  : duckduckgo"), std::string::npos);
    EXPECT_NE(out.find("Config backend  : auto"), std::string::npos);
    EXPECT_NE(out.find("Region          : global"), std::string::npos);
    EXPECT_NE(out.find("detected"), std::string::npos);
    EXPECT_NE(out.find("bing_cn"), std::string::npos);
}

TEST(WebSearchFormat, StatusWithoutDetectionTime) {
    WebSearchDisplaySnapshot snap;
    snap.config_backend = "auto";
    snap.region = "unknown";
    auto out = format_websearch_status(snap);
    EXPECT_NE(out.find("(none)"), std::string::npos); // active_backend 空
    EXPECT_EQ(out.find("(detected"), std::string::npos);
}

// === dispatch tests ===

TEST_F(WebSearchCommandTest, UninitializedRuntimeReturnsHint) {
    auto out = dispatch_websearch_subcommand("");
    EXPECT_NE(out.find("not initialized"), std::string::npos);
}

TEST_F(WebSearchCommandTest, BareCommandShowsStatus) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto& rt = web_search::runtime();
    rt.detector().detect_now(); // 写入缓存 = global
    rt.router().resolve_active(Region::Global);

    auto out = dispatch_websearch_subcommand("");
    EXPECT_NE(out.find("Active backend  : duckduckgo"), std::string::npos);
    EXPECT_NE(out.find("Region          : global"), std::string::npos);
}

TEST_F(WebSearchCommandTest, RefreshReDetectsAndShowsBeforeAfter) {
    WebSearchConfig cfg;
    // 1st probe: 200 (global);2nd probe (refresh): 0 (cn)
    install_runtime_with_mocks(cfg, {{200, ""}, {0, "blocked"}});
    auto& rt = web_search::runtime();
    rt.detector().detect_now();
    rt.router().resolve_active(Region::Global);

    auto out = dispatch_websearch_subcommand("refresh");
    EXPECT_NE(out.find("re-detected"), std::string::npos);
    EXPECT_NE(out.find("Before:"), std::string::npos);
    EXPECT_NE(out.find("After"), std::string::npos);
    // 重测后 region = cn → active = bing_cn
    EXPECT_EQ(rt.router().active_name(), "bing_cn");
}

TEST_F(WebSearchCommandTest, UseValidBackendSucceeds) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto& rt = web_search::runtime();
    rt.detector().detect_now();
    rt.router().resolve_active(Region::Global);

    auto out = dispatch_websearch_subcommand("use bing_cn");
    EXPECT_NE(out.find("switched to bing_cn"), std::string::npos);
    EXPECT_EQ(rt.router().active_name(), "bing_cn");
}

TEST_F(WebSearchCommandTest, UseUnknownBackendRejected) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto& rt = web_search::runtime();
    rt.router().resolve_active(Region::Global);
    auto out = dispatch_websearch_subcommand("use yandex");
    EXPECT_NE(out.find("Unknown backend"), std::string::npos);
    EXPECT_NE(out.find("yandex"), std::string::npos);
    EXPECT_EQ(rt.router().active_name(), "duckduckgo"); // 未变
}

TEST_F(WebSearchCommandTest, UseUnimplementedBackendRejected) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto& rt = web_search::runtime();
    rt.router().resolve_active(Region::Global);
    auto out = dispatch_websearch_subcommand("use tavily");
    EXPECT_NE(out.find("not implemented"), std::string::npos);
    EXPECT_EQ(rt.router().active_name(), "duckduckgo");
}

TEST_F(WebSearchCommandTest, UseWithoutArgShowsUsage) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto out = dispatch_websearch_subcommand("use");
    EXPECT_NE(out.find("Usage:"), std::string::npos);
}

TEST_F(WebSearchCommandTest, ResetRevertsToConfig) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto& rt = web_search::runtime();
    rt.detector().detect_now();
    rt.router().resolve_active(Region::Global);

    rt.router().set_active("bing_cn"); // /websearch use bing_cn
    EXPECT_EQ(rt.router().active_name(), "bing_cn");

    auto out = dispatch_websearch_subcommand("reset");
    EXPECT_NE(out.find("reset to config"), std::string::npos);
    EXPECT_EQ(rt.router().active_name(), "duckduckgo"); // 回到 region=global 推导
}

TEST_F(WebSearchCommandTest, UnknownSubcommandShowsHelp) {
    WebSearchConfig cfg;
    install_runtime_with_mocks(cfg, {{200, ""}});
    auto out = dispatch_websearch_subcommand("explode");
    EXPECT_NE(out.find("Unknown subcommand"), std::string::npos);
    EXPECT_NE(out.find("/websearch refresh"), std::string::npos);
}
