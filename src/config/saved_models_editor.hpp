// src/config/saved_models_editor.hpp
// saved_models 注册表的 add/update/remove 纯逻辑。HTTP handler / TUI 命令
// 都调用这里,失败返错误码,不写盘。
#pragma once

#include "config.hpp"
#include "saved_models.hpp"

#include <optional>
#include <string>

namespace acecode {

struct SavedModelDraft {
    std::string name;
    std::string provider;  // "openai" | "copilot"; legacy "codex" is disabled
    std::string model;
    std::string base_url;  // openai 必填
    std::string api_key;   // openai 必填
    std::optional<std::string> models_dev_provider_id;
    // 可选手动上下文窗口(token 数)。unset = 不覆盖;0 = 清除旧 override。
    std::optional<int> context_window;
    // 可选 OpenAI stream timeout(ms)。unset = 不覆盖;0 = 清除旧 override。
    std::optional<int> stream_timeout_ms;
};

enum class SavedModelEditError {
    OK,
    INVALID_NAME,         // 空字符串、含控制字符等
    RESERVED_NAME,        // 以 ( 开头(系统占用)
    NAME_TAKEN,           // 新增/改名时撞已有 name
    UNKNOWN_PROVIDER,     // 不是 openai/copilot
    PROVIDER_DISABLED,    // provider 已知但当前被屏蔽
    MISSING_MODEL,
    MISSING_BASE_URL,     // openai 必填
    INVALID_API_KEY,      // openai 必填(空字符串触发)
    INVALID_CONTEXT_WINDOW, // context_window < 0
    INVALID_STREAM_TIMEOUT, // stream_timeout_ms < 0
    NOT_FOUND,            // update/remove 时 name 不存在
    IN_USE_AS_DEFAULT,    // remove/改名时该 name 是 cfg.default_model_name
};

const char* to_string(SavedModelEditError e);

// 校验 + 把新条目追加到 cfg.saved_models。OK 时 cfg 已修改(caller 负责
// save_config);非 OK 时 cfg 未变。
SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d);

// 替换 cfg.saved_models 里 name == old_name 的条目为 d。改名(d.name !=
// old_name)走 delete + add 语义;若 old_name 是 default,返 IN_USE_AS_DEFAULT。
SavedModelEditError update_saved_model(AppConfig& cfg,
                                        const std::string& old_name,
                                        const SavedModelDraft& d);

// 删除 cfg.saved_models 里 name == name 的条目。若是 default 返
// IN_USE_AS_DEFAULT。
SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name);

} // namespace acecode
