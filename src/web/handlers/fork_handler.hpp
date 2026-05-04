#pragma once

// POST /api/sessions/:id/fork 的纯函数 helper(命名规则 + 消息查找)。
// 真正调用 SessionManager / SessionRegistry 的胶水在 server.cpp 注册的
// route handler 里;这里只放无副作用的字符串/容器逻辑,方便单测。
//
// 命名规则(spec session-fork "Auto-generated fork titles"):
//   - 客户端没传 title:`分叉<N>:<source_title>`
//   - N = 同 forked_from == source_id 的 sibling 数 + 1
//   - source_title 取 source_meta.title;空时降级到 source_meta.summary
//   - 长度超 50 codepoints(UTF-8 字符)截断 + `…`

#include "../../session/session_storage.hpp"
#include "../../provider/llm_provider.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode::web {

// 算 fork 出来的新 session 的默认标题。caller 拿到 source 当前 meta + 同
// project_dir 下所有 sibling meta(SessionStorage::list_sessions 返回的全集)。
// 调用方提供 explicit_title 时直接返回(空字符串/全空白则忽略,走自动)。
std::string compute_fork_title(const SessionMeta& source_meta,
                                const std::vector<SessionMeta>& sibling_metas,
                                const std::string& explicit_title);

// 在 messages 列表里找第一条 id 等于 message_id 的消息,返回其 index。
// id 通过 web::compute_message_id 算(user uuid / 其它 sha1)。找不到返回
// nullopt。空 message_id 视作"找不到"(不要给回 0)。
std::optional<std::size_t> find_message_index_by_id(
    const std::vector<ChatMessage>& messages,
    const std::string& message_id);

} // namespace acecode::web
