#pragma once

// /api/files 的纯函数业务逻辑 — Web UI SidePanel 的"文件 / 预览" tab 后端。
//
// 三个核心函数都不依赖 Crow,handlers/server.cpp 只做 HTTP 解析 + 序列化:
//   - validate_path_within: cwd + 用户给的相对/绝对 path → 解析后必须落在 cwd 子树
//   - list_directory: 返回指定目录的直接子项(不递归),按 dir 优先 + name 字典序排序
//   - read_file_content: 读文件原文,5MB cap + 二进制嗅探(前 512 字节出现 \0)
//
// 安全:
//   - cwd 必须 ∈ allowed_cwds(由 caller 从 daemon deps 构造,通常 = {deps.cwd}),
//     防止用户拿任意路径把 daemon 当通用 file server。
//   - path 用 `weakly_canonical(cwd / path)` 解析后做 prefix 比较,自动处理
//     `..`、绝对路径、符号链接、Windows 反斜杠。
//
// 噪音目录硬编码黑名单(NOISE_DIRS),始终过滤,与 show_hidden 无关。
// 隐藏文件(以 . 开头)默认过滤,show_hidden=true 时透出 — 但 NOISE_DIRS 黑名单
// 优先级更高(.git 永远不出)。
// 符号链接 / Windows reparse point 目录默认不展示,避免点击后解析到 workspace 外
// 而在前端表现为 400 错误。
//
// 5MB 上限 / 二进制嗅探的具体阈值在 .cpp 里以 constexpr 定义。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace acecode::web {

struct FileEntry {
    std::string name;        // 仅 basename,不含路径
    std::string path;        // 相对 cwd 的 forward-slash 路径(供前端拼回 URL)
    std::string kind;        // "file" | "dir"
    std::optional<std::uint64_t> size;        // 仅 kind="file" 才填
    std::optional<std::int64_t>  modified_ms; // unix epoch ms
};

enum class FileErrorKind {
    UnknownWorkspace,    // cwd 不在 allowed_cwds → HTTP 400
    PathOutsideWorkspace,// 越权 → HTTP 400
    NotFound,            // path 不存在或非目标 kind → HTTP 404
    TooLarge,            // 文件超 5 MB → HTTP 415
    Binary,              // 二进制(前 512 字节出现 \0) → HTTP 415
    IoError,             // 读盘失败 → HTTP 500
};

struct FileError {
    FileErrorKind kind;
    std::uint64_t size = 0; // 仅 TooLarge 时填,供前端展示给用户看
    std::string message;     // 给日志,不一定上抛给前端
};

// cwd:用户提供的 workspace 绝对路径(URL query 原样传入)。
// path:用户提供的相对路径(可空 = cwd 根本身)。
// allowed_cwds:caller 维护的白名单(daemon 进程的 cwd / 已注册 workspace cwd)。
//
// 成功:返回解析后的绝对 fs::path(已 weakly_canonical + lexically_normal)。
// 失败:返回 FileError(UnknownWorkspace / PathOutsideWorkspace)。
//
// 不验证 path 是否真实存在 — 由 caller(list_directory / read_file_content)处理。
std::variant<std::filesystem::path, FileError>
validate_path_within(const std::string& cwd,
                     const std::string& path,
                     const std::vector<std::string>& allowed_cwds);

// 列出 abs_dir 下的直接子项(不递归)。abs_dir 必须先经 validate_path_within。
//
// 行为:
//   - abs_dir 不存在或非目录 → FileError{NotFound}
//   - 默认过滤名称以 `.` 开头的项(show_hidden=true 时透出)
//   - 始终过滤名称在 NOISE_DIRS 内的项(.git / node_modules / dist / build /
//     __pycache__ / .venv / venv / target / .next / .cache),不受 show_hidden 影响
//   - 始终过滤符号链接 / Windows reparse point 目录,不递归跟随软连接
//   - 排序:kind=dir 优先,然后按 name 字典序(case-sensitive)
//   - FileEntry::path 用 cwd 的相对路径(forward-slash 归一,跨平台)
std::variant<std::vector<FileEntry>, FileError>
list_directory(const std::filesystem::path& abs_dir,
               const std::filesystem::path& abs_cwd,
               bool show_hidden);

// 读 abs_file 原文。abs_file 必须先经 validate_path_within。
//
// 行为:
//   - abs_file 不存在或为目录 → FileError{NotFound}
//   - file_size > 5 MB → FileError{TooLarge, size=N}
//   - 前 512 字节(或文件尾,取小)中出现 \0 → FileError{Binary}
//   - 否则一次性 read 完整文件,返回 std::string(原始字节,不强转编码)
std::variant<std::string, FileError>
read_file_content(const std::filesystem::path& abs_file);

} // namespace acecode::web
