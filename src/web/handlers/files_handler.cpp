#include "files_handler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>

namespace acecode::web {

namespace fs = std::filesystem;

namespace {

// 把 path 字符串归一到比较友好的形式:
//   - lexically_normal + generic_string(forward slash)
//   - 剥 trailing '/'(若不是 root)
//   - Windows 上转 lowercase(NTFS / FAT 都 case-insensitive,且 `D:\acetest`
//     vs `d:\acetest` 是同一目录;current_path / desktop bridge 报告的盘符大小写
//     不一致是常见坑)
std::string normalize_path_for_compare(const std::string& s) {
    std::string out = fs::path(s).lexically_normal().generic_string();
    if (out.size() > 1 && out.back() == '/') out.pop_back();
#ifdef _WIN32
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return out;
}

// 噪音目录黑名单 — 始终过滤,与 show_hidden 无关。
// 选择标准:1) 体积大(node_modules 几万项)2) 内容用户不应通过 web UI 浏览(.git
// 元数据)3) 通用编译产物(dist/build/target)。.gitignore 解析延后,见 design.md。
constexpr std::array<const char*, 10> NOISE_DIRS = {
    ".git", "node_modules", "dist", "build", "__pycache__",
    ".venv", "venv", "target", ".next", ".cache",
};

constexpr std::uint64_t kMaxPreviewBytes = 5ull * 1024 * 1024; // 5 MiB
constexpr std::size_t kBinarySniffBytes = 512;

bool is_noise_dir(const std::string& name) {
    for (const auto* n : NOISE_DIRS) {
        if (name == n) return true;
    }
    return false;
}

bool is_hidden(const std::string& name) {
    return !name.empty() && name.front() == '.';
}

// 把 fs::path 归一成 forward-slash 字符串(跨平台,前端拼 URL 用)。
std::string to_forward_slash(const fs::path& p) {
    return p.lexically_normal().generic_string();
}

// fs::file_time_type → unix epoch ms。C++17 没有标准转换,用 chrono::clock_cast
// 不可移植,这里走 file_time_type::clock::now() + system_clock::now() 的偏移。
std::int64_t file_time_to_unix_ms(fs::file_time_type ftime) {
    using namespace std::chrono;
    auto ftime_now   = fs::file_time_type::clock::now();
    auto sysclock_now = system_clock::now();
    auto delta = ftime - ftime_now;
    auto sys_time = sysclock_now + duration_cast<system_clock::duration>(delta);
    return duration_cast<milliseconds>(sys_time.time_since_epoch()).count();
}

} // namespace

std::variant<fs::path, FileError>
validate_path_within(const std::string& cwd,
                     const std::string& path,
                     const std::vector<std::string>& allowed_cwds) {
    // 1) cwd 必须 ∈ allowed_cwds(归一比较,容错 trailing slash / 反斜杠 /
    //    Windows 盘符大小写)
    auto cwd_norm = normalize_path_for_compare(cwd);
    bool found = false;
    for (const auto& allow : allowed_cwds) {
        if (normalize_path_for_compare(allow) == cwd_norm) {
            found = true; break;
        }
    }
    if (!found) {
        return FileError{FileErrorKind::UnknownWorkspace, 0,
                         "cwd '" + cwd + "' not in allowed list"};
    }

    // 2) 拼接 + weakly_canonical(不要求路径存在,允许部分不存在前缀)
    fs::path abs_cwd;
    fs::path abs_target;
    {
        std::error_code ec;
        abs_cwd = fs::weakly_canonical(fs::path(cwd), ec);
        if (ec) abs_cwd = fs::path(cwd).lexically_normal();
    }
    {
        std::error_code ec;
        fs::path joined = path.empty() ? abs_cwd : (abs_cwd / fs::path(path));
        abs_target = fs::weakly_canonical(joined, ec);
        if (ec) abs_target = joined.lexically_normal();
    }

    // 3) prefix 比较 — 走同样的归一(forward slash + 大小写)
    auto cwd_cmp    = normalize_path_for_compare(abs_cwd.generic_string());
    auto target_cmp = normalize_path_for_compare(abs_target.generic_string());
    if (target_cmp == cwd_cmp) return abs_target;
    if (cwd_cmp.empty() || cwd_cmp.back() != '/') cwd_cmp += '/';
    if (target_cmp.rfind(cwd_cmp, 0) != 0) {
        return FileError{FileErrorKind::PathOutsideWorkspace, 0,
                         "path '" + path + "' escapes cwd"};
    }
    return abs_target;
}

std::variant<std::vector<FileEntry>, FileError>
list_directory(const fs::path& abs_dir,
               const fs::path& abs_cwd,
               bool show_hidden) {
    std::error_code ec;
    if (!fs::exists(abs_dir, ec) || ec) {
        return FileError{FileErrorKind::NotFound, 0, "directory not found"};
    }
    if (!fs::is_directory(abs_dir, ec) || ec) {
        return FileError{FileErrorKind::NotFound, 0, "not a directory"};
    }

    std::vector<FileEntry> out;
    out.reserve(64);

    fs::directory_iterator it(abs_dir, ec);
    if (ec) return FileError{FileErrorKind::IoError, 0, ec.message()};
    fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break; // 单项错误不致命,跳出循环返回已收集
        const auto& entry_path = it->path();
        std::string name = entry_path.filename().string();
        if (name.empty()) continue;

        // noise 黑名单永远过滤
        if (is_noise_dir(name)) continue;
        // 隐藏文件按 show_hidden 决定
        if (!show_hidden && is_hidden(name)) continue;

        FileEntry e;
        e.name = name;

        // 算相对 cwd 的路径(供前端拼回 URL)
        auto rel = fs::relative(entry_path, abs_cwd, ec);
        if (ec) {
            // relative 失败 → 用 lexically_relative fallback(不查盘)
            rel = entry_path.lexically_relative(abs_cwd);
        }
        e.path = to_forward_slash(rel);

        std::error_code stat_ec;
        bool is_dir = fs::is_directory(entry_path, stat_ec);
        e.kind = is_dir ? "dir" : "file";
        if (!is_dir) {
            auto sz = fs::file_size(entry_path, stat_ec);
            if (!stat_ec) e.size = static_cast<std::uint64_t>(sz);
        }
        auto ftime = fs::last_write_time(entry_path, stat_ec);
        if (!stat_ec) e.modified_ms = file_time_to_unix_ms(ftime);

        out.push_back(std::move(e));
    }

    // 排序:dir 优先,然后 name 字典序(case-sensitive,与文件系统一致)
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.kind != b.kind) return a.kind == "dir";
        return a.name < b.name;
    });

    return out;
}

std::variant<std::string, FileError>
read_file_content(const fs::path& abs_file) {
    std::error_code ec;
    if (!fs::exists(abs_file, ec) || ec) {
        return FileError{FileErrorKind::NotFound, 0, "file not found"};
    }
    if (fs::is_directory(abs_file, ec) || ec) {
        return FileError{FileErrorKind::NotFound, 0, "is a directory"};
    }

    auto sz = fs::file_size(abs_file, ec);
    if (ec) return FileError{FileErrorKind::IoError, 0, ec.message()};
    if (sz > kMaxPreviewBytes) {
        return FileError{FileErrorKind::TooLarge, static_cast<std::uint64_t>(sz),
                         "file exceeds 5 MB cap"};
    }

    std::ifstream ifs(abs_file, std::ios::binary);
    if (!ifs) return FileError{FileErrorKind::IoError, 0, "cannot open file"};

    // 二进制嗅探:读前 kBinarySniffBytes 字节(或文件尾),出现 \0 → Binary
    std::string head;
    head.resize(static_cast<std::size_t>(std::min<std::uint64_t>(sz, kBinarySniffBytes)));
    if (!head.empty()) {
        ifs.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(ifs.gcount()));
    }
    for (char c : head) {
        if (c == '\0') {
            return FileError{FileErrorKind::Binary, 0, "binary content detected"};
        }
    }

    // 嗅探通过,继续读完剩余内容
    std::string rest;
    if (sz > head.size()) {
        rest.resize(static_cast<std::size_t>(sz - head.size()));
        ifs.read(rest.data(), static_cast<std::streamsize>(rest.size()));
        rest.resize(static_cast<std::size_t>(ifs.gcount()));
    }
    return head + rest;
}

} // namespace acecode::web
