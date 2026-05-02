#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace acecode {

class PathValidator {
public:
    PathValidator(const std::string& working_dir, bool dangerous_mode)
        : dangerous_mode_(dangerous_mode)
    {
        try {
            working_dir_ = normalize(std::filesystem::weakly_canonical(working_dir).string());
        } catch (...) {
            working_dir_ = normalize(working_dir);
        }
    }

    // Returns empty string if path is safe, otherwise a rejection reason
    std::string validate(const std::string& path) const {
        if (dangerous_mode_) return "";

        std::string resolved;
        try {
            resolved = normalize(std::filesystem::weakly_canonical(resolve_input_path(path)).string());
        } catch (...) {
            resolved = normalize(resolve_input_path(path).string());
        }

        // CWD boundary check
        if (!is_inside_working_dir(resolved)) {
            return "Path outside working directory: " + path +
                   " (resolved: " + resolved + ", cwd: " + working_dir_ + ")";
        }

        return "";
    }

    // Check if a path points to a dangerous/sensitive file
    bool is_dangerous_path(const std::string& path) const {
        if (dangerous_mode_) return false;

        std::string resolved;
        try {
            resolved = normalize(std::filesystem::weakly_canonical(resolve_input_path(path)).string());
        } catch (...) {
            resolved = normalize(resolve_input_path(path).string());
        }

        // Check filename against dangerous files list
        std::string filename = get_filename(resolved);
        std::string filename_lower = to_lower(filename);
        for (const auto& df : dangerous_files()) {
            if (df[0] == '*') {
                // Wildcard extension match: *.pem, *.key
                std::string ext = df.substr(1); // ".pem", ".key"
                if (ends_with_ci(filename_lower, ext)) return true;
            } else {
                if (filename_lower == df) return true;
            }
        }

        // Check path segments against dangerous directories
        for (const auto& dd : dangerous_directories()) {
            std::string dd_segment = "/" + dd + "/";
            std::string resolved_lower = to_lower(resolved);
            if (resolved_lower.find(dd_segment) != std::string::npos) return true;
            // Also check if path ends with the dangerous dir
            if (ends_with_ci(resolved_lower, "/" + dd)) return true;
        }

        return false;
    }

private:
    static std::string normalize(const std::string& path) {
        std::string result = path;
        for (auto& c : result) {
            if (c == '\\') c = '/';
        }
        // Remove trailing slash (but keep root "/")
        while (result.size() > 1 && result.back() == '/') {
            result.pop_back();
        }
        return result;
    }

    static std::string to_lower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    static std::string get_filename(const std::string& path) {
        auto pos = path.rfind('/');
        if (pos != std::string::npos) return path.substr(pos + 1);
        return path;
    }

    std::filesystem::path resolve_input_path(const std::string& path) const {
        std::filesystem::path p(path);
        if (p.is_relative() && !working_dir_.empty()) {
            return std::filesystem::path(working_dir_) / p;
        }
        return p;
    }

    bool is_inside_working_dir(const std::string& resolved) const {
        if (!starts_with_ci(resolved, working_dir_)) return false;
        if (resolved.size() == working_dir_.size()) return true;
        char next = resolved[working_dir_.size()];
        return next == '/' || next == '\\';
    }

    static bool ends_with_ci(const std::string& str, const std::string& suffix) {
        if (suffix.size() > str.size()) return false;
        return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
    }

    // Case-insensitive prefix check (for Windows path comparison)
    static bool starts_with_ci(const std::string& str, const std::string& prefix) {
        if (prefix.size() > str.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(str[i])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
            if (a != b) return false;
        }
        // After matching the prefix, the next char must be / or end of string
        if (str.size() > prefix.size() && str[prefix.size()] != '/') return false;
        return true;
    }

    static const std::vector<std::string>& dangerous_files() {
        static const std::vector<std::string> files = {
            ".env", ".env.local", ".env.production",
            ".gitconfig", ".bashrc", ".bash_profile", ".zshrc",
            ".npmrc", ".yarnrc",
            "id_rsa", "id_ed25519", "id_ecdsa", "id_dsa",
            "*.pem", "*.key", "*.p12", "*.pfx",
            "authorized_keys", "known_hosts",
            ".netrc", ".pgpass",
        };
        return files;
    }

    static const std::vector<std::string>& dangerous_directories() {
        static const std::vector<std::string> dirs = {
            ".git", ".ssh", ".gnupg", ".vscode",
            ".aws", ".azure", ".kube",
        };
        return dirs;
    }

    std::string working_dir_;
    bool dangerous_mode_;
};

} // namespace acecode
