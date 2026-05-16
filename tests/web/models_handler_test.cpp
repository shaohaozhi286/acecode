// 覆盖 src/web/handlers/models_handler.cpp。前端 model-picker 要靠这里
// 拼出 saved_models 行;一旦回归:
//   - list_models 漏 saved_models 行 → 用户无法切换模型
//   - find_model_by_name 大小写敏感配错 → POST 切换 400

#include <gtest/gtest.h>

#include "web/handlers/models_handler.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

using acecode::AppConfig;
using acecode::ModelProfile;
using acecode::SessionModelState;
using acecode::web::find_model_by_name;
using acecode::web::list_models;
using acecode::web::model_state_to_json;

namespace {

// 构造一个最小 cfg,带两个 saved_models 条目。
AppConfig make_cfg_with_two() {
    AppConfig cfg;

    ModelProfile a;
    a.name = "copilot-fast"; a.provider = "copilot"; a.model = "gpt-4o";
    cfg.saved_models.push_back(a);

    ModelProfile b;
    b.name = "local-lm"; b.provider = "openai"; b.model = "llama-3";
    b.base_url = "http://localhost:1234/v1"; b.api_key = "x";
    cfg.saved_models.push_back(b);

    return cfg;
}

} // namespace

// 场景: list_models 输出顺序 = saved_models 顺序。
TEST(ModelsHandler, ListIncludesAllSavedModels) {
    auto cfg = make_cfg_with_two();
    auto arr = list_models(cfg);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);

    EXPECT_EQ(arr[0]["name"], "copilot-fast");
    EXPECT_EQ(arr[1]["name"], "local-lm");
    EXPECT_TRUE(arr[1].contains("base_url"));
}

// 场景: 空 saved_models 时,list_models 返回空数组。
TEST(ModelsHandler, ListEmptySavedReturnsEmptyArray) {
    AppConfig cfg;
    auto arr = list_models(cfg);
    ASSERT_TRUE(arr.is_array());
    EXPECT_TRUE(arr.empty());
}

// 场景: find_model_by_name 命中 saved_models 条目。
TEST(ModelsHandler, FindBySavedName) {
    auto cfg = make_cfg_with_two();
    auto e = find_model_by_name(cfg, "local-lm");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->provider, "openai");
    EXPECT_EQ(e->model, "llama-3");
}

// 场景: 未命中 → nullopt。Caller 转 400。
TEST(ModelsHandler, FindUnknownReturnsNullopt) {
    auto cfg = make_cfg_with_two();
    EXPECT_FALSE(find_model_by_name(cfg, "nonexistent").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "").has_value());
}

// 场景: name 大小写敏感 — TUI /model 也是大小写敏感,必须保持一致。
TEST(ModelsHandler, FindIsCaseSensitive) {
    auto cfg = make_cfg_with_two();
    EXPECT_TRUE(find_model_by_name(cfg, "copilot-fast").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "COPILOT-FAST").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "Copilot-Fast").has_value());
}

// 场景: current session model state 序列化必须包含前端 footer/selector
// 需要的完整字段。
TEST(ModelsHandler, ModelStateToJsonIncludesCurrentSessionFields) {
    SessionModelState state;
    state.name = "copilot-fast";
    state.provider = "copilot";
    state.model = "gpt-5";
    state.context_window = 400000;

    auto j = model_state_to_json(state);
    EXPECT_EQ(j["name"], "copilot-fast");
    EXPECT_EQ(j["provider"], "copilot");
    EXPECT_EQ(j["model"], "gpt-5");
    EXPECT_EQ(j["context_window"], 400000);
}

// ------------------- 增删改 helper 测试 -------------------

#include "config/saved_models_editor.hpp"

using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::web::http_status_for_edit_error;
using acecode::web::parse_model_probe_request;
using acecode::web::parse_openai_model_ids;
using acecode::web::parse_model_draft;
using acecode::web::profile_to_safe_json;

// 触发场景:server.cpp 把 saved_models_editor 的错误码翻成 HTTP 状态;
// 这层映射与前端 toast 文案强相关 — NOT_FOUND→404 / NAME_TAKEN→409 /
// IN_USE_AS_DEFAULT→409,任一改错都会让"删默认"这种 UX 走偏(本来要 409
// 提示先改默认,变成 500 用户不知道为啥)。
// 期望行为:固定的状态码映射,新增其它枚举值默认 fallback 400(校验失败)。
TEST(ModelsHandler, ErrorToHttpStatusMapping) {
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NOT_FOUND), 404);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NAME_TAKEN), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::IN_USE_AS_DEFAULT), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_NAME), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_API_KEY), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::RESERVED_NAME), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::UNKNOWN_PROVIDER), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::MISSING_MODEL), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::MISSING_BASE_URL), 400);
}

// 触发场景:POST/PUT 成功后把 ModelProfile 序列化回去给前端。api_key 是
// 敏感字段 — spec 安全契约:api_key 永不出现在 HTTP response 里。
// 期望行为:profile_to_safe_json 输出永远不含 "api_key" key,即使输入有值。
// 回归就泄露 api_key 给浏览器(F12 看 Network 直接看到)。
TEST(ModelsHandler, ProfileToSafeJsonOmitsApiKey) {
    ModelProfile p;
    p.name = "local";
    p.provider = "openai";
    p.model = "llama-3";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    auto j = profile_to_safe_json(p);
    EXPECT_FALSE(j.contains("api_key"));
    EXPECT_EQ(j["base_url"], "http://localhost/v1");
    EXPECT_EQ(j["name"], "local");
}

// 触发场景:前端 POST /api/models 漏字段时,后端要给出明确的字段名,
// 否则前端只能笼统报"提交失败"。
// 期望行为:漏 provider 字段 → err 字符串里出现 "provider";返回 nullopt。
TEST(ModelsHandler, ParseDraftReportsMissingField) {
    nlohmann::json body = {{"name", "x"}};
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("provider"), std::string::npos);
}

// 触发场景:前端 form 经常把空 input 序列化成 null,而不是省略字段;同时
// 偶发情况下 base_url / api_key 可能是 number / bool 等非字符串类型(测试
// stub / 错配的 schema)。可选字段一旦因此整体拒绝,UX 上看就是"明明
// 没填也要报错"。这里和 models_dev_provider_id 的处理对齐:可选字段碰到
// null 或 非 string,静默跳过而不是设 err。
// 期望行为:body 含 base_url=null + api_key=42(int)→ parse 仍返回有效
// draft,err 留空,base_url / api_key 为空字符串(语义同字段缺省)。
// 回归表现:前端表单提交"只填必填项"被后端拒绝。
TEST(ModelsHandler, ParseDraftSkipsNullOptionalFields) {
    nlohmann::json body = {
        {"name", "lm"},
        {"provider", "copilot"},
        {"model", "gpt-4o"},
        {"base_url", nullptr},  // null
        {"api_key", 42},         // 非字符串
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    ASSERT_TRUE(d.has_value()) << "可选字段为 null/非 string 时应静默跳过";
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(d->base_url, "");
    EXPECT_EQ(d->api_key, "");
}

// 触发场景:必填字段是 null/非字符串(比如前端误把 name 写成数字)。这种
// 情况和可选字段不一样 —— 必须明确报错给前端,提示哪个字段类型错了。
// 期望行为:err 中应出现 "name",且 parse 返回 nullopt;否则前端会以为
// 提交成功但拿到一个 name 为空的 draft。
TEST(ModelsHandler, ParseDraftRejectsNonStringRequiredField) {
    nlohmann::json body = {
        {"name", 42},  // 非 string,name 是必填
        {"provider", "copilot"},
        {"model", "gpt-4o"},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("name"), std::string::npos);
}

// 触发场景:前端 POST /api/models body 完整(含 openai 必填的 base_url +
// api_key)。
// 期望行为:全部字段就绪,返回的 SavedModelDraft 字段值与输入一致;
// err 留空。
TEST(ModelsHandler, ParseDraftAcceptsFullBody) {
    nlohmann::json body = {
        {"name", "local"},
        {"provider", "openai"},
        {"model", "llama-3"},
        {"base_url", "http://localhost/v1"},
        {"api_key", "sk-x"},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->name, "local");
    EXPECT_EQ(d->provider, "openai");
    EXPECT_EQ(d->model, "llama-3");
    EXPECT_EQ(d->base_url, "http://localhost/v1");
    EXPECT_EQ(d->api_key, "sk-x");
    EXPECT_TRUE(err.empty());
}

TEST(ModelsHandler, ParseOpenAiModelIdsAcceptsStandardDataArray) {
    nlohmann::json body = {
        {"data", nlohmann::json::array({
            {{"id", "gpt-4o"}},
            {{"id", "gpt-4o-mini"}},
            {{"id", "gpt-4o"}},
            {{"object", "model"}},
        })},
    };
    auto ids = parse_openai_model_ids(body);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "gpt-4o");
    EXPECT_EQ(ids[1], "gpt-4o-mini");
}

TEST(ModelsHandler, ParseOpenAiModelIdsAcceptsFallbackShapes) {
    auto from_models = parse_openai_model_ids(nlohmann::json{
        {"models", nlohmann::json::array({"b-model", "a-model"})},
    });
    ASSERT_EQ(from_models.size(), 2u);
    EXPECT_EQ(from_models[0], "a-model");
    EXPECT_EQ(from_models[1], "b-model");

    auto from_array = parse_openai_model_ids(nlohmann::json::array({
        {{"id", "z-model"}},
        "manual-model",
    }));
    ASSERT_EQ(from_array.size(), 2u);
    EXPECT_EQ(from_array[0], "manual-model");
    EXPECT_EQ(from_array[1], "z-model");
}

TEST(ModelsHandler, ParseProbeRequestValidatesProviderAndBaseUrl) {
    std::string code;
    std::string err;
    auto unsupported = parse_model_probe_request(
        nlohmann::json{{"provider", "copilot"}, {"base_url", "http://x"}},
        code,
        err);
    EXPECT_FALSE(unsupported.has_value());
    EXPECT_EQ(code, "UNKNOWN_PROVIDER");

    code.clear();
    err.clear();
    auto missing_url = parse_model_probe_request(
        nlohmann::json{{"provider", "openai"}},
        code,
        err);
    EXPECT_FALSE(missing_url.has_value());
    EXPECT_EQ(code, "MISSING_BASE_URL");

    code.clear();
    err.clear();
    auto ok = parse_model_probe_request(
        nlohmann::json{{"provider", "openai"}, {"base_url", "http://localhost/v1"}, {"api_key", "sk"}},
        code,
        err);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->base_url, "http://localhost/v1");
    EXPECT_EQ(ok->api_key, "sk");
}
