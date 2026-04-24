// cwd_model_override 实现。原子写策略参考 input_history_store.cpp。
#include "cwd_model_override.hpp"

#include "../session/session_storage.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;

namespace acecode {

std::string cwd_model_override_path(const fs::path& cwd) {
    std::string project_dir = SessionStorage::get_project_dir(cwd.string());
    return (fs::path(project_dir) / "model_override.json").string();
}

std::optional<std::string> load_cwd_model_override(const fs::path& cwd) {
    std::string path = cwd_model_override_path(cwd);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;

    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            LOG_WARN(std::string("[cwd_model_override] cannot open ") + path);
            return std::nullopt;
        }
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (!j.is_object() || !j.contains("model_name") || !j["model_name"].is_string()) {
            LOG_WARN(std::string("[cwd_model_override] malformed or missing model_name in ") + path);
            return std::nullopt;
        }
        std::string name = j["model_name"].get<std::string>();
        if (name.empty()) return std::nullopt;
        return name;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[cwd_model_override] parse failure: ") + e.what() + " (" + path + ")");
        return std::nullopt;
    }
}

void save_cwd_model_override(const fs::path& cwd, const std::string& name) {
    std::string path = cwd_model_override_path(cwd);
    std::string tmp = path + ".tmp";

    try {
        fs::path p(path);
        std::error_code ec;
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
            if (ec) {
                LOG_ERROR(std::string("[cwd_model_override] create_directories failed: ") + ec.message());
                return;
            }
        }

        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                LOG_ERROR(std::string("[cwd_model_override] cannot open tmp: ") + tmp);
                return;
            }
            nlohmann::json j;
            j["model_name"] = name;
            ofs << j.dump(2) << '\n';
        }

        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            // Windows: rename 到已存在文件会失败,回退 remove + rename。
            fs::remove(path, ec);
            ec.clear();
            fs::rename(tmp, path, ec);
            if (ec) {
                LOG_ERROR(std::string("[cwd_model_override] rename failed: ") + ec.message());
                fs::remove(tmp, ec);  // 别遗留 .tmp
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[cwd_model_override] save exception: ") + e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

void remove_cwd_model_override(const fs::path& cwd) {
    std::string path = cwd_model_override_path(cwd);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        LOG_WARN(std::string("[cwd_model_override] remove failed: ") + ec.message());
    }
}

} // namespace acecode
