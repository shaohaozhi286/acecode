// InputHistoryStore 实现：append-first + 超阈值原子 rewrite。
// 并发策略：last-writer-wins，不加锁；实际数据量 ≤ 10 行，丢最新一条可接受。
// 见 openspec/changes/persistent-input-history/design.md D2 / D5。
#include "history/input_history_store.hpp"

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace acecode {

namespace {

bool trim_empty(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

// 读全部非空行（保留顺序）。坏 JSON 行会被上层 load() 过滤；这里只负责
// 原始分行，供 rewrite_atomic 等内部函数复用。
std::vector<std::string> read_all_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream ifs(path);
    if (!ifs.is_open()) return out;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
        line.clear();
    }
    return out;
}

// 把 entries 原子写入 path：先写 path+".tmp"，再 rename 覆盖。
// rename 失败回退为删除-重命名组合（Windows 上 rename 到已存在文件会失败）。
void rewrite_atomic(const std::string& path, const std::vector<std::string>& entries) {
    std::string tmp = path + ".tmp";
    try {
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                LOG_ERROR(std::string("[input_history] cannot open tmp file for rewrite: ") + tmp);
                return;
            }
            for (const auto& entry : entries) {
                nlohmann::json j;
                j["text"] = entry;
                ofs << j.dump() << '\n';
            }
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            // Windows 下 rename 到已存在文件会失败，降级为 remove + rename
            fs::remove(path, ec);
            ec.clear();
            fs::rename(tmp, path, ec);
            if (ec) {
                LOG_ERROR(std::string("[input_history] rewrite rename failed: ") + ec.message());
                fs::remove(tmp, ec); // 不遗留 .tmp
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[input_history] rewrite_atomic exception: ") + e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

} // namespace

std::string InputHistoryStore::file_path(const std::string& project_dir) {
    return (fs::path(project_dir) / "input_history.jsonl").string();
}

std::vector<std::string> InputHistoryStore::load(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return out;
    }
    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            LOG_ERROR(std::string("[input_history] cannot open for read: ") + path);
            return out;
        }
        std::string line;
        int lineno = 0;
        while (std::getline(ifs, line)) {
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                if (!j.is_object() || !j.contains("text") || !j["text"].is_string()) {
                    LOG_WARN(std::string("[input_history] skipping malformed line ") +
                             std::to_string(lineno) + " in " + path);
                    continue;
                }
                out.push_back(j["text"].get<std::string>());
            } catch (const std::exception& e) {
                LOG_WARN(std::string("[input_history] skipping bad JSON at ") +
                         path + ":" + std::to_string(lineno) + ": " + e.what());
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[input_history] load exception: ") + e.what());
    }
    return out;
}

void InputHistoryStore::append(const std::string& path,
                               const std::string& entry,
                               int max_entries) {
    if (trim_empty(entry)) return;
    if (max_entries <= 0) return;

    try {
        // 目录按需创建（首次在该工作目录写入时必要）。
        fs::path p(path);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (ec) {
                LOG_ERROR(std::string("[input_history] create_directories failed: ") + ec.message());
                return;
            }
        }

        // append-first：一次短 IO。
        {
            std::ofstream ofs(path, std::ios::binary | std::ios::app);
            if (!ofs.is_open()) {
                LOG_ERROR(std::string("[input_history] cannot open for append: ") + path);
                return;
            }
            nlohmann::json j;
            j["text"] = entry;
            ofs << j.dump() << '\n';
        }

        // 超阈值再 rewrite。正常情况下每次 append 最多超 1 行。
        auto lines = read_all_lines(path);
        if (static_cast<int>(lines.size()) > max_entries) {
            // 解析各行，保留合法条目的 text 字段；对于坏行直接丢弃（load 时也会
            // 跳过），rewrite 之后文件就彻底 canonical 了。
            std::vector<std::string> parsed;
            parsed.reserve(lines.size());
            for (const auto& line : lines) {
                try {
                    auto j = nlohmann::json::parse(line);
                    if (j.is_object() && j.contains("text") && j["text"].is_string()) {
                        parsed.push_back(j["text"].get<std::string>());
                    }
                } catch (...) {
                    // 丢弃坏行
                }
            }
            if (static_cast<int>(parsed.size()) > max_entries) {
                parsed.erase(parsed.begin(),
                             parsed.begin() + (parsed.size() - static_cast<size_t>(max_entries)));
            }
            rewrite_atomic(path, parsed);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[input_history] append exception: ") + e.what());
    }
}

void InputHistoryStore::clear(const std::string& path) {
    try {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            LOG_ERROR(std::string("[input_history] clear failed: ") + ec.message());
        }
        // .tmp 也顺手清掉，避免上一次崩溃残留
        fs::remove(path + ".tmp", ec);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[input_history] clear exception: ") + e.what());
    }
}

} // namespace acecode
