// /history 命令实现：list / clear 两种子动作。
// 文件路径解析沿用 SessionStorage::get_project_dir(cwd)，与启动加载路径一致。
#include "commands/history_command.hpp"

#include "history/input_history_store.hpp"
#include "session/session_storage.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>

namespace acecode {

namespace {

// 将前后空白裁掉，供 /history 的子命令参数判断使用。不用 boost::trim 是为了
// 保持 commands 目录零额外依赖。
std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string history_file_path_for_cwd() {
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    if (ec) cwd = ".";
    return InputHistoryStore::file_path(SessionStorage::get_project_dir(cwd));
}

void cmd_history(CommandContext& ctx, const std::string& args) {
    std::string sub = trim(args);
    if (sub == "clear") {
        std::vector<std::string> pending_msgs;
        {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.input_history.clear();
            ctx.state.history_index = -1;
            ctx.state.saved_input.clear();

            // 同步删盘：即使 config.input_history.enabled=false（磁盘本就不更新），
            // 也顺手把历史残留文件一并清掉，语义更直观。
            InputHistoryStore::clear(history_file_path_for_cwd());

            // 用已有 status_line_saved / status_line_clear_at 机制：2 秒后自动恢复。
            if (ctx.state.status_line_clear_at.time_since_epoch().count() == 0) {
                ctx.state.status_line_saved = ctx.state.status_line;
            }
            ctx.state.status_line = "Input history cleared";
            ctx.state.status_line_clear_at =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        }
        ctx.state.conversation.push_back({"system", "Input history cleared.", false});
        ctx.state.chat_follow_tail = true;
        if (ctx.post_event) ctx.post_event();
        return;
    }

    // 默认 = list（"" 或 "list" 或其他未知参数）
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        snapshot = ctx.state.input_history;
    }
    std::ostringstream oss;
    if (snapshot.empty()) {
        oss << "Input history is empty";
    } else {
        oss << "Input history (" << snapshot.size() << " entr"
            << (snapshot.size() == 1 ? "y" : "ies") << ", oldest first):";
        for (size_t i = 0; i < snapshot.size(); ++i) {
            oss << "\n  " << (i + 1) << ". " << snapshot[i];
        }
    }
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
    if (ctx.post_event) ctx.post_event();
}

} // namespace

void register_history_command(CommandRegistry& registry) {
    registry.register_command({
        "history",
        "List or clear the per-working-directory input history",
        cmd_history,
    });
}

} // namespace acecode
