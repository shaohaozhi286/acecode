#include "package.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <zip.h>

namespace fs = std::filesystem;

namespace acecode::upgrade {
namespace {

bool is_within_root(const fs::path& root, const fs::path& child) {
    const fs::path root_norm = fs::absolute(root).lexically_normal();
    const fs::path child_norm = fs::absolute(child).lexically_normal();
    auto r = root_norm.begin();
    auto c = child_norm.begin();
    for (; r != root_norm.end(); ++r, ++c) {
        if (c == child_norm.end() || *r != *c) return false;
    }
    return true;
}

std::string trim_trailing_slashes(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
    return s;
}

} // namespace

bool is_safe_zip_entry_path(const std::string& entry, std::string* error) {
    std::string clean = trim_trailing_slashes(entry);
    if (clean.empty()) {
        if (error) *error = "zip entry path is empty";
        return false;
    }
    if (clean.front() == '/' || clean.front() == '\\') {
        if (error) *error = "zip entry path is absolute";
        return false;
    }
    if (clean.find('\\') != std::string::npos) {
        if (error) *error = "zip entry path must use forward slashes";
        return false;
    }
    if (clean.size() >= 2 && clean[1] == ':') {
        if (error) *error = "zip entry path is drive-qualified";
        return false;
    }

    size_t start = 0;
    while (start <= clean.size()) {
        size_t end = clean.find('/', start);
        std::string part = (end == std::string::npos)
            ? clean.substr(start)
            : clean.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            if (error) *error = "zip entry path contains unsafe segment";
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

fs::path expected_executable_name_for_target(const std::string& target) {
    return target.rfind("windows-", 0) == 0 ? fs::path("acecode.exe") : fs::path("acecode");
}

bool extract_zip_to_staging(const fs::path& zip_path,
                            const fs::path& staging_dir,
                            std::string* error) {
    std::error_code ec;
    fs::create_directories(staging_dir, ec);
    if (ec) {
        if (error) *error = "failed to create staging directory: " + ec.message();
        return false;
    }

    int zip_error = 0;
    zip_t* archive = zip_open(zip_path.string().c_str(), ZIP_RDONLY, &zip_error);
    if (!archive) {
        if (error) *error = "failed to open zip package";
        return false;
    }

    const zip_int64_t count = zip_get_num_entries(archive, 0);
    std::array<char, 64 * 1024> buf{};
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, i, 0, &st) != 0 || !st.name) {
            if (error) *error = "failed to read zip entry metadata";
            zip_close(archive);
            return false;
        }

        const std::string name = st.name;
        std::string path_error;
        if (!is_safe_zip_entry_path(name, &path_error)) {
            if (error) *error = "unsafe zip entry '" + name + "': " + path_error;
            zip_close(archive);
            return false;
        }

        fs::path dest = staging_dir / fs::path(trim_trailing_slashes(name));
        if (!is_within_root(staging_dir, dest)) {
            if (error) *error = "zip entry escapes staging directory: " + name;
            zip_close(archive);
            return false;
        }

        const bool is_dir = !name.empty() && (name.back() == '/' || name.back() == '\\');
        if (is_dir) {
            fs::create_directories(dest, ec);
            if (ec) {
                if (error) *error = "failed to create directory from zip: " + ec.message();
                zip_close(archive);
                return false;
            }
            continue;
        }

        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
            if (error) *error = "failed to create parent directory from zip: " + ec.message();
            zip_close(archive);
            return false;
        }

        zip_file_t* zf = zip_fopen_index(archive, i, 0);
        if (!zf) {
            if (error) *error = "failed to open zip entry: " + name;
            zip_close(archive);
            return false;
        }
        std::ofstream ofs(dest, std::ios::binary);
        if (!ofs) {
            if (error) *error = "failed to create staged file: " + dest.string();
            zip_fclose(zf);
            zip_close(archive);
            return false;
        }
        zip_int64_t n = 0;
        while ((n = zip_fread(zf, buf.data(), buf.size())) > 0) {
            ofs.write(buf.data(), static_cast<std::streamsize>(n));
            if (!ofs) {
                if (error) *error = "failed to write staged file: " + dest.string();
                zip_fclose(zf);
                zip_close(archive);
                return false;
            }
        }
        if (n < 0) {
            if (error) *error = "failed to read zip entry: " + name;
            zip_fclose(zf);
            zip_close(archive);
            return false;
        }
        zip_fclose(zf);
    }

    if (zip_close(archive) != 0) {
        if (error) *error = "failed to close zip package";
        return false;
    }
    return true;
}

std::optional<StagedPackage> validate_staged_package(const fs::path& staging_dir,
                                                     const std::string& target,
                                                     std::string* error) {
    const fs::path exe_name = expected_executable_name_for_target(target);
    const fs::path root_exe = staging_dir / exe_name;
    if (fs::is_regular_file(root_exe)) {
        return StagedPackage{staging_dir, root_exe};
    }

    std::set<fs::path> top_dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(staging_dir, ec)) {
        if (entry.is_directory()) {
            top_dirs.insert(entry.path());
        }
    }
    if (ec) {
        if (error) *error = "failed to inspect staged package: " + ec.message();
        return std::nullopt;
    }
    if (top_dirs.size() == 1) {
        const fs::path dir = *top_dirs.begin();
        const fs::path nested_exe = dir / exe_name;
        if (fs::is_regular_file(nested_exe)) {
            return StagedPackage{dir, nested_exe};
        }
    }

    if (error) {
        *error = "staged package does not contain expected executable: " +
                 exe_name.string();
    }
    return std::nullopt;
}

} // namespace acecode::upgrade
