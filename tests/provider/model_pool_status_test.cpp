#include <gtest/gtest.h>

#include "provider/model_pool_status.hpp"

#include <string>

using acecode::effective_context_window;
using acecode::is_pub_model;
using acecode::model_load_tier;
using acecode::ModelLoadTier;
using acecode::ModelPoolFetchResult;
using acecode::ModelPoolStatusService;
using acecode::parse_model_pool_status;

namespace {

// 图1接口的真实返回样本(截取两个 PUB 池)。注意每项有两个 maxWindowTokens:
// 顶层的 150000 是池窗口(我们要的),generateParam 里的 32000/14000 是生成参数,
// 解析必须取顶层那个。
const char* kSampleResponse = R"JSON(
{"ret":0,"data":[
  {"modelPoolName":"PUB-DeepSeek-V4-Flash","usageRate":60,"modelName":"DeepSeek-V4-Flash","maxWindowTokens":150000,
   "generateParam":{"top_p":1.0,"top_k":40,"temperature":1.0,"min_p":0,"maxWindowTokens":32000,"maxChatTokens":24000}},
  {"modelPoolName":"PUB-Qwen3.6-35B-A3B-FP8","usageRate":48,"modelName":"Qwen3.6-35B-A3B-FP8","maxWindowTokens":150000,
   "generateParam":{"top_p":0.8,"top_k":20,"temperature":0.7,"min_p":0,"maxWindowTokens":14000,"maxChatTokens":14000}}
],"errorMessage":null,"succ":true,"status":"ok","errorDetail":null}
)JSON";

} // namespace

// 场景:解析图1的真实样本。
// 期望:两个池都被解析出来;usageRate / 顶层 maxWindowTokens 正确。
// 回归点:必须取每项顶层的 maxWindowTokens(150000),不能误取 generateParam 内
// 的嵌套值(32000) —— 后者会让 0.8 系数算出错误的上下文窗口。
TEST(ModelPoolStatus, ParsesPoolsFromSampleResponse) {
    auto pools = parse_model_pool_status(kSampleResponse);
    ASSERT_EQ(pools.size(), 2u);

    ASSERT_TRUE(pools.count("PUB-DeepSeek-V4-Flash"));
    EXPECT_EQ(pools["PUB-DeepSeek-V4-Flash"].usage_rate, 60);
    EXPECT_EQ(pools["PUB-DeepSeek-V4-Flash"].max_window_tokens, 150000);  // 不是 32000

    ASSERT_TRUE(pools.count("PUB-Qwen3.6-35B-A3B-FP8"));
    EXPECT_EQ(pools["PUB-Qwen3.6-35B-A3B-FP8"].usage_rate, 48);
    EXPECT_EQ(pools["PUB-Qwen3.6-35B-A3B-FP8"].max_window_tokens, 150000);  // 不是 14000
}

// 场景:各种异常输入(网络抖动 / 接口变更 / 空 body)。
// 期望:全部安全返回空 map,绝不抛异常 —— 监控失败不能拖垮主流程。
TEST(ModelPoolStatus, ParseToleratesGarbage) {
    EXPECT_TRUE(parse_model_pool_status("").empty());
    EXPECT_TRUE(parse_model_pool_status("not json").empty());
    EXPECT_TRUE(parse_model_pool_status("[]").empty());          // 顶层非对象
    EXPECT_TRUE(parse_model_pool_status("{\"ret\":0}").empty()); // 缺 data
    EXPECT_TRUE(parse_model_pool_status("{\"data\":\"x\"}").empty()); // data 非数组
    EXPECT_TRUE(parse_model_pool_status("{\"data\":[{\"usageRate\":5}]}").empty()); // 缺 modelPoolName
}

// 场景:负载色阶分档。阈值由用户定义,边界容易写错,逐个钉死。
// 期望:<70 绿 / 70..90 黄 / >90 红 / <0 未知。图2 的 93% 落红。
TEST(ModelPoolStatus, LoadTierThresholds) {
    EXPECT_EQ(model_load_tier(-1), ModelLoadTier::Unknown);
    EXPECT_EQ(model_load_tier(0), ModelLoadTier::Green);
    EXPECT_EQ(model_load_tier(69), ModelLoadTier::Green);
    EXPECT_EQ(model_load_tier(70), ModelLoadTier::Yellow);  // 70 不算 "70以下"
    EXPECT_EQ(model_load_tier(90), ModelLoadTier::Yellow);  // "90以下" 含 90
    EXPECT_EQ(model_load_tier(91), ModelLoadTier::Red);
    EXPECT_EQ(model_load_tier(93), ModelLoadTier::Red);     // 图2
    EXPECT_EQ(model_load_tier(100), ModelLoadTier::Red);
}

// 场景:把池窗口换算成有效上下文窗口。
// 期望:0.8 系数 + 四舍五入;150000 → 120000(图算结果);非正数 → 0。
TEST(ModelPoolStatus, EffectiveContextWindowAppliesPointEight) {
    EXPECT_EQ(effective_context_window(150000), 120000);
    EXPECT_EQ(effective_context_window(14000), 11200);
    EXPECT_EQ(effective_context_window(0), 0);
    EXPECT_EQ(effective_context_window(-5), 0);
}

// 场景:判断模型名是否为 PUB 池。
// 期望:前缀 "PUB" 大小写不敏感;非 PUB / 过短 / 空 → false。
TEST(ModelPoolStatus, IsPubModel) {
    EXPECT_TRUE(is_pub_model("PUB-DeepSeek-V4-Flash"));
    EXPECT_TRUE(is_pub_model("pub-something"));  // 大小写不敏感
    EXPECT_FALSE(is_pub_model("gpt-4o"));
    EXPECT_FALSE(is_pub_model("deepseek-v4-flash"));
    EXPECT_FALSE(is_pub_model("PU"));  // 过短
    EXPECT_FALSE(is_pub_model(""));
}

// 场景:服务用注入的 mock fetch 刷新一次(不打真实网络),再查负载/有效窗口。
// 期望:refresh 成功;精确匹配 modelPoolName 能查到;有效窗口 = 0.8x;
//       未知模型 → nullopt;非 PUB / 未命中 → 有效窗口 0(调用方回退默认)。
TEST(ModelPoolStatus, ServiceRefreshAndLookupWithMockFetch) {
    ModelPoolStatusService svc(
        [](const std::string&) {
            ModelPoolFetchResult r;
            r.status_code = 200;
            r.body = kSampleResponse;
            return r;
        });

    EXPECT_TRUE(svc.refresh_once());

    auto pool = svc.get("PUB-DeepSeek-V4-Flash");
    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(pool->usage_rate, 60);

    EXPECT_EQ(svc.effective_context_window_for("PUB-DeepSeek-V4-Flash"), 120000);

    EXPECT_FALSE(svc.get("does-not-exist").has_value());
    EXPECT_EQ(svc.effective_context_window_for("gpt-4o"), 0);
}

// 场景:接口返回 HTTP 错误码或传输层错误。
// 期望:refresh_once 返回 false,缓存保持为空(不会用脏数据污染 UI)。
TEST(ModelPoolStatus, ServiceRefreshFailsOnHttpError) {
    ModelPoolStatusService http_err(
        [](const std::string&) {
            ModelPoolFetchResult r;
            r.status_code = 500;
            r.body = "internal error";
            return r;
        });
    EXPECT_FALSE(http_err.refresh_once());
    EXPECT_TRUE(http_err.snapshot().empty());

    ModelPoolStatusService transport_err(
        [](const std::string&) {
            ModelPoolFetchResult r;
            r.status_code = 0;
            r.error = "Couldn't resolve host";
            return r;
        });
    EXPECT_FALSE(transport_err.refresh_once());
    EXPECT_TRUE(transport_err.snapshot().empty());
}
