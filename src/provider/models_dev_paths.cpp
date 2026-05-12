#include "models_dev_paths.hpp"

#include "../utils/utf8_path.hpp"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace acecode {

namespace {

bool dir_has_api_json(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    return fs::is_regular_file(dir / "api.json", ec);
}

} // namespace

std::optional<std::string> find_models_dev_dir(const std::string& argv0_dir) {
    std::string env = getenv_utf8("ACECODE_MODELS_DEV_DIR");
    if (!env.empty()) {
        fs::path p = path_from_utf8(env);
        if (dir_has_api_json(p)) {
            return path_to_utf8(p);
        }
    }

    if (!argv0_dir.empty()) {
        // Production install layout: <prefix>/bin/acecode → <prefix>/share/...
        fs::path install_candidate = path_from_utf8(argv0_dir) / ".." / "share" / "acecode" / "models_dev";
        std::error_code ec;
        fs::path normalized = fs::weakly_canonical(install_candidate, ec);
        if (ec) normalized = install_candidate.lexically_normal();
        if (dir_has_api_json(normalized)) return path_to_utf8(normalized);

        // Dev build layout: build/Release/acecode.exe → repo/assets/models_dev.
        // Walk up to 4 levels looking for assets/models_dev so cmake/MSVC/Ninja
        // generators with deeper output trees still resolve in-tree.
        fs::path probe = path_from_utf8(argv0_dir);
        for (int i = 0; i < 5; ++i) {
            fs::path dev = probe / "assets" / "models_dev";
            std::error_code dec;
            fs::path dev_norm = fs::weakly_canonical(dev, dec);
            if (dec) dev_norm = dev.lexically_normal();
            if (dir_has_api_json(dev_norm)) return path_to_utf8(dev_norm);
            fs::path parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
    }

#ifndef _WIN32
    {
        fs::path system_path("/usr/share/acecode/models_dev");
        if (dir_has_api_json(system_path)) {
            return path_to_utf8(system_path);
        }
    }
#endif

    return std::nullopt;
}

} // namespace acecode
