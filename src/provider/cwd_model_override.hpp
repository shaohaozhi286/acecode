// cwd_model_override: 每工作目录 model override 文件的读写。
// 对应 openspec/changes/model-profiles 的 Section 3。
// 文件位置:`~/.acecode/projects/<cwd_hash>/model_override.json`
// schema:`{"model_name": "<name>"}`
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acecode {

// 读 override。文件不存在 → nullopt;malformed / 缺字段 / 类型错误 →
// nullopt + log warning(MUST NOT 抛异常,不阻塞启动)。
std::optional<std::string> load_cwd_model_override(const std::filesystem::path& cwd);

// 原子写(tmp + rename,Windows 下 rename 失败回退 remove+rename)。
// 复用 `session_storage.cpp::compute_project_hash(cwd)` 得到 <cwd_hash>。
void save_cwd_model_override(const std::filesystem::path& cwd, const std::string& name);

// 删除 override 文件;文件不存在为 no-op。
void remove_cwd_model_override(const std::filesystem::path& cwd);

// 便捷函数:返回 override 文件的绝对路径。测试可用。不保证父目录存在。
std::string cwd_model_override_path(const std::filesystem::path& cwd);

} // namespace acecode
