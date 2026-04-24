// saved_models: 命名模型注册表的 schema + 校验。
// 对应 openspec/changes/model-profiles 的 Section 1 —— ModelProfile 数据模型。
#pragma once

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

// 一个命名模型条目。自包含 —— 一个 entry 就够 create_provider_from_entry 构造
// 出一个可用 provider 实例。name 保留前缀 `"("` 给 ACECode 合成的特殊 name
// (例如 `(legacy)` / `(session:XXXX)`);user-defined name MUST NOT 以 `(` 开头。
struct ModelProfile {
    std::string name;
    std::string provider;  // "openai" | "copilot"
    std::string base_url;  // openai 必填;copilot 忽略
    std::string api_key;   // openai 必填
    std::string model;     // 模型标识,必填
    std::optional<std::string> models_dev_provider_id;  // 可选,给 context resolver 的 hint
};

// 解析失败时的描述。line_hint = -1 表示无具体行号信息。
struct SavedModelsValidationError {
    std::string message;
    int line_hint = -1;
};

// 纯函数:把 `saved_models` JSON 数组解析为 ModelProfile vector。
// 成功返回 vector(可为空);失败返回 nullopt 并向 err 写入详细原因。
std::optional<std::vector<ModelProfile>> parse_saved_models(const nlohmann::json& node,
                                                          std::string& err);

// 纯函数:校验 entry 列表 + default_name 的合法性。
// 校验点:
//  - 各 entry 的 name 非空、不以 `(` 开头(保留前缀)
//  - 列表内 name 唯一(大小写敏感比较)
//  - provider == "openai" 时 base_url / api_key 必须非空(api_key 允许为"<空字符串>"
//    用于 local LM Studio 等无认证的场景 —— TODO 若后续严格化可改)
//  - 若 default_name 非空,MUST 指向列表中某 entry 的 name
bool validate_saved_models(const std::vector<ModelProfile>& entries,
                           const std::string& default_name,
                           std::string& err);

} // namespace acecode
