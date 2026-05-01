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

namespace {

// 写入任意 JSON value,保留其它 key,原子写。供 web_search 缓存等结构化写入复用。
void write_state_value(const std::string& key, const nlohmann::json& value) {
    bool corrupted = false;
    auto j = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    j[key] = value;

    std::string p = state_file_path();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);

    if (!atomic_write_file(p, j.dump(2))) {
        LOG_WARN("[state_file] failed to write " + p);
    }
}

void erase_state_key(const std::string& key) {
    bool corrupted = false;
    auto j = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    if (!j.contains(key)) return; // 没有可删的就别动文件,避免无谓 I/O
    j.erase(key);

    std::string p = state_file_path();
    std::error_code ec;
    fs::create_directories(fs::path(p).parent_path(), ec);

    if (!atomic_write_file(p, j.dump(2))) {
        LOG_WARN("[state_file] failed to write " + p);
    }
}

} // namespace

std::optional<WebSearchRegionCache> read_web_search_region_cache() {
    auto j = load_state_or_empty();
    if (!j.contains("web_search") || !j["web_search"].is_object()) return std::nullopt;
    const auto& wsj = j["web_search"];
    if (!wsj.contains("region_detected") || !wsj["region_detected"].is_string()) {
        return std::nullopt;
    }
    std::string region = wsj["region_detected"].get<std::string>();
    if (region != "global" && region != "cn") return std::nullopt;
    WebSearchRegionCache c;
    c.region = std::move(region);
    if (wsj.contains("region_detected_at_ms") &&
        wsj["region_detected_at_ms"].is_number_integer()) {
        c.detected_at_ms = wsj["region_detected_at_ms"].get<long long>();
    }
    return c;
}

void write_web_search_region_cache(const WebSearchRegionCache& cache) {
    if (cache.region != "global" && cache.region != "cn") {
        LOG_WARN("[state_file] refusing to write web_search region '" +
                 cache.region + "' (must be global or cn)");
        return;
    }
    nlohmann::json wsj = nlohmann::json::object();
    wsj["region_detected"] = cache.region;
    wsj["region_detected_at_ms"] = cache.detected_at_ms;
    write_state_value("web_search", wsj);
}

void clear_web_search_region_cache() {
    erase_state_key("web_search");
}

} // namespace acecode
