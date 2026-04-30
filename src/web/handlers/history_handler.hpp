#pragma once

// /api/history GET / POST 的纯逻辑(可单测)。Crow 路由注册在 server.cpp。
//
// 复用 src/history/input_history_store.hpp 的存储格式 —— 同一份 jsonl 与
// TUI 共享。daemon 与 TUI 同时跑同一 cwd 时,两边读写都走同一个文件,
// `InputHistoryStore::append` 已经做 atomic rename 不会撞。

#include "../../config/config.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode::web {

// 拉历史。max 为 0 / 负数 = 不限。返回 JSON array 字符串(最旧在前,最新在末尾)。
// `enabled=false` 时直接返回空数组(InputHistoryStore 不参与)。
nlohmann::json load_history(const std::string& cwd, int max,
                              const InputHistoryConfig& cfg);

// 追加一条。enabled=false 时直接 noop(静默丢弃,与 TUI 行为一致)。
// 失败被 InputHistoryStore::append 内部 swallow + 写日志,本函数不上抛。
void append_history(const std::string& cwd, const std::string& text,
                      const InputHistoryConfig& cfg);

} // namespace acecode::web
