#include "history_handler.hpp"

#include "../../history/input_history_store.hpp"
#include "../../session/session_storage.hpp"

namespace acecode::web {

namespace {

std::string history_path_for_cwd(const std::string& cwd) {
    return InputHistoryStore::file_path(SessionStorage::get_project_dir(cwd));
}

} // namespace

nlohmann::json load_history(const std::string& cwd, int max,
                              const InputHistoryConfig& cfg) {
    if (!cfg.enabled) return nlohmann::json::array();

    auto entries = InputHistoryStore::load(history_path_for_cwd(cwd));
    // max>0 时尾部截断 — 保留最新 max 条(InputHistoryStore::load 返回的顺序
    // 是 旧→新,所以从末尾开始保留 max 条 = 从前面砍掉)。
    if (max > 0 && static_cast<int>(entries.size()) > max) {
        entries.erase(entries.begin(),
                       entries.begin() + (entries.size() - max));
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) arr.push_back(e);
    return arr;
}

void append_history(const std::string& cwd, const std::string& text,
                      const InputHistoryConfig& cfg) {
    if (!cfg.enabled) return;
    InputHistoryStore::append(history_path_for_cwd(cwd), text, cfg.max_entries);
}

} // namespace acecode::web
