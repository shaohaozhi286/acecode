#include "git_context_core.hpp"

#include <sstream>

namespace acecode::gitinfo {

bool is_safe_ref_name(const std::string& name) {
    if (name.empty() || name.front() == '-' || name.front() == '/') return false;
    if (name.find("..") != std::string::npos) return false;

    // 按 '/' 分段:空段("a//b"、尾 '/')与 '.' 段都拒绝 ——
    // git check-ref-format 同样拒绝,且 '.' 段在路径 join 时会被归一掉,
    // 让校验与实际访问的路径产生歧义。
    std::size_t seg_start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            std::size_t len = i - seg_start;
            if (len == 0) return false;
            if (len == 1 && name[seg_start] == '.') return false;
            seg_start = i + 1;
        }
    }

    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                  c == '_' || c == '+' || c == '@' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_valid_git_sha(const std::string& s) {
    if (s.size() != 40 && s.size() != 64) return false;
    for (unsigned char c : s) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

std::string truncate_status_for_snapshot(const std::string& status,
                                         std::size_t max_chars) {
    if (status.size() <= max_chars) return status;
    // 回退到 UTF-8 序列边界,不在多字节字符中间切断。
    std::size_t cut = max_chars;
    while (cut > 0 &&
           (static_cast<unsigned char>(status[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return status.substr(0, cut) +
           "\n... (truncated because it exceeds 2k characters. If you need "
           "more information, run \"git status\" using the bash tool)";
}

std::string format_git_status_snapshot(const SnapshotParts& parts) {
    std::ostringstream oss;
    oss << "This is the git status at the start of the conversation. Note that "
           "this status is a snapshot in time, and will not update during the "
           "conversation.\n\n";
    oss << "Current branch: "
        << (parts.branch.empty() ? std::string("HEAD") : parts.branch) << "\n\n";
    oss << "Main branch (you will usually use this for PRs): "
        << (parts.default_branch.empty() ? std::string("main")
                                         : parts.default_branch)
        << "\n\n";
    if (!parts.user_name.empty()) {
        oss << "Git user: " << parts.user_name << "\n\n";
    }
    const std::string status = truncate_status_for_snapshot(parts.status_short);
    oss << "Status:\n" << (status.empty() ? std::string("(clean)") : status)
        << "\n\n";
    oss << "Recent commits:\n" << parts.log_oneline;
    return oss.str();
}

} // namespace acecode::gitinfo
