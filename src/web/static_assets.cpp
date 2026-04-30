#include "static_assets.hpp"

#include "../utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace acecode::web {

namespace fs = std::filesystem;

namespace {

// 后缀 → content-type 映射。覆盖前端会用到的全部扩展名。未列出 → octet-stream。
struct MimeRow {
    const char* ext;
    const char* mime;
};

constexpr MimeRow kMimeTable[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".mjs",  "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".map",  "application/json; charset=utf-8"},
    {".svg",  "image/svg+xml"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".ico",  "image/x-icon"},
    {".woff2","font/woff2"},
    {".woff", "font/woff"},
    {".ttf",  "font/ttf"},
    {".otf",  "font/otf"},
    {".md",   "text/markdown; charset=utf-8"},
    {".txt",  "text/plain; charset=utf-8"},
};

// 简易 ends_with(C++17 没 string.ends_with)
bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

} // namespace

std::string mime_type_for_path(const std::string& path) {
    for (const auto& row : kMimeTable) {
        if (ends_with(path, row.ext)) return row.mime;
    }
    return "application/octet-stream";
}

std::optional<AssetLookupResult> EmbeddedAssetSource::lookup(const std::string& path) const {
    const auto& m = embedded_asset_map();
    auto it = m.find(path);
    if (it == m.end()) return std::nullopt;
    AssetLookupResult r;
    r.content_type = mime_type_for_path(path);
    r.data = it->second.first;
    r.size = it->second.second;
    return r;
}

std::optional<AssetLookupResult> FileSystemAssetSource::lookup(const std::string& path) const {
    fs::path full = fs::path(root_) / path;

    // 安全:确保 full 在 root_ 之下(防 ".." 穿越)。weakly_canonical 可能解析失败,
    // 用 lexically_normal 也行。
    std::error_code ec;
    auto canon = fs::weakly_canonical(full, ec);
    if (ec) return std::nullopt;
    auto root_canon = fs::weakly_canonical(fs::path(root_), ec);
    if (ec) return std::nullopt;
    auto rel = canon.lexically_relative(root_canon).generic_string();
    if (rel.rfind("..", 0) == 0) return std::nullopt; // 越界

    if (!fs::exists(canon, ec) || !fs::is_regular_file(canon, ec)) return std::nullopt;

    std::ifstream ifs(canon, std::ios::binary);
    if (!ifs) return std::nullopt;
    auto buf = std::make_shared<std::string>(
        (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    AssetLookupResult r;
    r.content_type   = mime_type_for_path(path);
    r.data           = reinterpret_cast<const unsigned char*>(buf->data());
    r.size           = buf->size();
    r.owned_buffer   = buf;
    return r;
}

std::unique_ptr<AssetSource> make_asset_source(const std::string& static_dir) {
    if (static_dir.empty()) {
        return std::make_unique<EmbeddedAssetSource>();
    }
    if (!fs::exists(static_dir)) {
        throw std::runtime_error("web.static_dir path does not exist: " + static_dir);
    }
    LOG_INFO("[web] using FileSystem asset source: " + static_dir);
    return std::make_unique<FileSystemAssetSource>(static_dir);
}

} // namespace acecode::web
