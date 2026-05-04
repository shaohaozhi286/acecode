#pragma once

// ChatMessage → 网络可传 JSON 的纯函数。从 server.cpp 抽出,目的:
//   - 让 GET /api/sessions/:id/messages 的每条 message 带稳定 `id` 字段
//   - WebSocket 上行 `message` 事件 payload 也用同一份序列化,字段一致
//   - 抽成纯函数后可单测(不依赖 Crow / 网络栈)
//
// id 字段来源(见 src/utils/sha1.hpp 设计注释 + openspec session-fork spec):
//   - user 消息:走 ChatMessage.uuid(rewind change 持久化的 UUID)
//   - 其它角色(assistant / tool / system):lazy 算 sha1(role + " " + content + " " + timestamp)
//     不回写 JSONL,只在序列化时计算

#include "../provider/llm_provider.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace acecode::web {

// 计算 ChatMessage 的稳定 ID。空 content/timestamp 也不会崩。
std::string compute_message_id(const ChatMessage& m);

// 把 ChatMessage 序列化为 web 协议 JSON,**总是带 id 字段**。
// 字段集合 = session_serializer 的输出 + 顶层 id。
nlohmann::json chat_message_to_payload_json(const ChatMessage& m);

} // namespace acecode::web
