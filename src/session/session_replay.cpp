// 见 session_replay.hpp 头注释。本文件实现 replay_session_messages 纯函数。

#include "session_replay.hpp"

#include "session_rewind.hpp"
#include "tool_metadata_codec.hpp"
#include "../tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

// 从 ChatMessage.tool_calls(数组形态)取第 i 项的 function.name / arguments。
// 失败(类型错或缺字段)时把对应输出参数留空 —— 调用方据此降级到 legacy 显示。
void extract_tool_call_function(
    const nlohmann::json& tool_calls,
    size_t i,
    std::string& name,
    std::string& args)
{
    if (!tool_calls.is_array() || i >= tool_calls.size()) return;
    const auto& tc = tool_calls[i];
    if (!tc.is_object() || !tc.contains("function")) return;
    const auto& fn = tc["function"];
    if (!fn.is_object()) return;
    if (fn.contains("name") && fn["name"].is_string()) {
        name = fn["name"].get<std::string>();
    }
    if (fn.contains("arguments") && fn["arguments"].is_string()) {
        args = fn["arguments"].get<std::string>();
    }
}

} // namespace

std::vector<TuiState::Message> replay_session_messages(
    const std::vector<ChatMessage>& messages,
    const ToolExecutor& tools)
{
    std::vector<TuiState::Message> out;
    out.reserve(messages.size());

    for (const auto& msg : messages) {
        if (is_file_checkpoint_message(msg)) {
            continue;
        }

        if (msg.role == "user" || msg.role == "system") {
            // 规范角色,文本承载所有信息,直接推入。
            out.push_back({msg.role, msg.content, /*is_tool=*/false});
            continue;
        }

        if (msg.role == "assistant") {
            // 文本前奏(若有)先 push,顺序与运行时 on_delta+on_message 累积一致。
            if (!msg.content.empty()) {
                out.push_back({"assistant", msg.content, /*is_tool=*/false});
            }
            // 每个 tool_call 单独成一行。display_override 用 build_tool_call_preview
            // 现算,失败(非法 JSON)时返回空,TUI 会回退到 legacy 显示。
            if (msg.tool_calls.is_array()) {
                for (size_t i = 0; i < msg.tool_calls.size(); ++i) {
                    std::string name, args;
                    extract_tool_call_function(msg.tool_calls, i, name, args);

                    TuiState::Message tc_row;
                    tc_row.role = "tool_call";
                    tc_row.content = "[Tool: " + name + "] " + args;
                    tc_row.is_tool = true;
                    tc_row.display_override =
                        ToolExecutor::build_tool_call_preview(name, args);
                    out.push_back(std::move(tc_row));
                }
            }
            continue;
        }

        if (msg.role == "tool") {
            // 规范工具结果:role 改名为 tool_result,is_tool=true。
            // 视觉字段(summary / hunks)从 metadata 子键还原;缺失或解码失败 →
            // 字段留空,渲染走 fold 降级。
            TuiState::Message tr_row;
            tr_row.role = "tool_result";
            tr_row.content = msg.content;
            tr_row.is_tool = true;

            if (msg.metadata.is_object()) {
                if (msg.metadata.contains("tool_summary")) {
                    auto s = decode_tool_summary(msg.metadata["tool_summary"]);
                    if (s.has_value()) {
                        tr_row.summary = std::move(*s);
                    }
                }
                if (msg.metadata.contains("tool_hunks")) {
                    auto h = decode_tool_hunks(msg.metadata["tool_hunks"]);
                    if (h.has_value()) {
                        tr_row.hunks = std::move(*h);
                    }
                }
            }

            out.push_back(std::move(tr_row));
            continue;
        }

        // 未知 role(forward-compat):原样推入,is_tool 默认 false。
        // shell-mode 的 "tool_result" 伪角色如果没被 main.cpp 配对识别消化,
        // 会落进这里 —— 也是正确的降级,TUI 渲染端有 "tool_result" 分支会接住。
        TuiState::Message m;
        m.role = msg.role;
        m.content = msg.content;
        m.is_tool = (msg.role == "tool_result");  // 让 shell-mode 落盘的伪角色保持 is_tool=true
        out.push_back(std::move(m));
    }

    return out;
}

} // namespace acecode
