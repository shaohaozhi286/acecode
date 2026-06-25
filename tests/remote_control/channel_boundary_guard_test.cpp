#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path repo_root_from_this_file() {
    fs::path cur = fs::absolute(fs::path(__FILE__));
    if (cur.has_filename()) cur = cur.parent_path();
    while (!cur.empty()) {
        if (fs::exists(cur / "src" / "remote_control") &&
            fs::exists(cur / "openspec")) {
            return cur;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return fs::current_path();
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream out;
    out << ifs.rdbuf();
    return out.str();
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<fs::path> files_under(const fs::path& path) {
    std::vector<fs::path> files;
    if (!fs::exists(path)) return files;
    if (fs::is_regular_file(path)) {
        files.push_back(path);
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        files.push_back(entry.path());
    }
    return files;
}

std::string forbidden_identifier() {
    std::string value;
    for (char ch : {'k', 'l', 'p', 'a'}) value.push_back(ch);
    return value;
}

} // namespace

TEST(ChannelBoundaryGuard, ProductSpecificIdentifierDoesNotEnterCoreSurfaces) {
    const fs::path root = repo_root_from_this_file();
    const std::vector<fs::path> scan_roots = {
        root / "src" / "remote_control",
        root / "src" / "commands" / "remote_control_command.cpp",
        root / "src" / "commands" / "remote_control_command.hpp",
        root / "src" / "config" / "config.cpp",
        root / "src" / "config" / "config.hpp",
        root / "tests" / "remote_control",
        root / "tests" / "commands" / "remote_control_command_test.cpp",
        root / "tests" / "config",
        root / "README.md",
        root / "README_CN.md",
        root / "ARCHITECTURE.md",
        root / "docs",
        root / "openspec" / "changes" / "add-remote-control",
    };
    const std::string needle = forbidden_identifier();

    std::vector<fs::path> offenders;
    for (const auto& scan_root : scan_roots) {
        for (const auto& file : files_under(scan_root)) {
            const std::string content = lower_ascii(read_file(file));
            if (content.find(needle) != std::string::npos) {
                offenders.push_back(fs::relative(file, root));
            }
        }
    }

    EXPECT_TRUE(offenders.empty()) << [&] {
        std::ostringstream out;
        out << "product-specific identifier found in:";
        for (const auto& file : offenders) out << "\n  " << file.string();
        return out.str();
    }();
}
