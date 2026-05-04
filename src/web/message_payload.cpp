#include "message_payload.hpp"

#include "../session/session_serializer.hpp"
#include "../utils/sha1.hpp"

namespace acecode::web {

std::string compute_message_id(const ChatMessage& m) {
    if (m.role == "user" && !m.uuid.empty()) {
        return m.uuid;
    }
    // 非 user 消息一律走 sha1(role + " " + content + " " + timestamp)。
    // user 消息没有 uuid 时也走这条路径(老 session 兼容)。
    std::string buf;
    buf.reserve(m.role.size() + m.content.size() + m.timestamp.size() + 2);
    buf.append(m.role);
    buf.push_back(' ');
    buf.append(m.content);
    buf.push_back(' ');
    buf.append(m.timestamp);
    return sha1_hex(buf);
}

nlohmann::json chat_message_to_payload_json(const ChatMessage& m) {
    // 复用 session_serializer 的字段集合 + 顶层 id。
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(serialize_message(m));
    } catch (...) {
        j = nlohmann::json{{"role", m.role}, {"content", m.content}};
    }
    j["id"] = compute_message_id(m);
    return j;
}

} // namespace acecode::web
