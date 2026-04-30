#include "state_file.hpp"

#include "atomic_file.hpp"
#include "logger.hpp"
#include "paths.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

// 测试 hook:非空时绕过 resolve_data_dir() 路径,直接用这个绝对路径。
// 通过 set_state_file_path_for_test 设置/清除。
std::string& test_path_override() {
    static std::string p;
    return p;
}

std::string state_file_path() {
    const auto& override_path = test_path_override();
    if (!override_path.empty()) return override_path;
    return (fs::path(resolve_data_dir(get_run_mode())) / "state.json").string();
}

// 加载现有 state.json:解析失败 / 文件不存在 → 返回空 object。
// is_corrupted 在文件存在但解析失败时设为 true,让上层知道写回时要覆盖。
nlohmann::json load_state_or_empty(bool* is_corrupted = nullptr) {
    if (is_corrupted) *is_corrupted = false;

    std::string p = state_file_path();
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        return nlohmann::json::object();
    }
    std::ifstream ifs(p);
    if (!ifs.is_open()) {
        return nlohmann::json::object();
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string contents = buf.str();
    if (contents.empty()) {
        return nlohmann::json::object();
    }
    try {
        auto j = nlohmann::json::parse(contents);
        if (!j.is_object()) {
            if (is_corrupted) *is_corrupted = true;
            return nlohmann::json::object();
        }
        return j;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[state_file] state.json parse failed: ") + e.what() +
                 ", treating as corrupted");
        if (is_corrupted) *is_corrupted = true;
        return nlohmann::json::object();
    }
}

} // namespace

bool read_state_flag(const std::string& key) {
    auto j = load_state_or_empty();
    if (!j.contains(key)) return false;
    if (!j[key].is_boolean()) return false;
    return j[key].get<bool>();
}

void write_state_flag(const std::string& key, bool value) {
    bool corrupted = false;
    auto j = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    j[key] = value;

    std::string p = state_file_path();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);
    // 目录创建失败不致命 — atomic_write_file 失败时再处理。

    if (!atomic_write_file(p, j.dump(2))) {
        LOG_WARN("[state_file] failed to write " + p);
    }
}

void set_state_file_path_for_test(const std::string& path) {
    test_path_override() = path;
}

} // namespace acecode
