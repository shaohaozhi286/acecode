#pragma once

// 静态资源服务的统一接口。
//
// 两种 source:
//   - EmbeddedAssetSource: 走 build 期生成的 static_assets_data.cpp 字节数组
//   - FileSystemAssetSource: 走 web.static_dir(开发模式,改文件即生效)
//
// MIME map 在 .cpp 内硬编码,只覆盖前端实际用到的扩展名(html/css/js/json/
// svg/png/ico/woff2/woff/ttf/md)。未知扩展名 fallback 到
// application/octet-stream。

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace acecode::web {

struct AssetLookupResult {
    std::string                 content_type;
    const unsigned char*        data = nullptr;
    std::size_t                 size = 0;
    // 当 source = FileSystem 时,持有一份从磁盘读出来的 buffer 用于保活。
    // EmbeddedAssetSource 不需要,data 直接指向静态数组。
    std::shared_ptr<std::string> owned_buffer;
};

class AssetSource {
public:
    virtual ~AssetSource() = default;
    // path 是相对路径(如 "index.html" / "components/ace-app.js"),
    // 不带 leading "/"。Caller 调用前已剥掉 "/static/" 前缀。
    virtual std::optional<AssetLookupResult> lookup(const std::string& path) const = 0;
};

class EmbeddedAssetSource : public AssetSource {
public:
    std::optional<AssetLookupResult> lookup(const std::string& path) const override;
};

class FileSystemAssetSource : public AssetSource {
public:
    explicit FileSystemAssetSource(std::string root) : root_(std::move(root)) {}
    std::optional<AssetLookupResult> lookup(const std::string& path) const override;
private:
    std::string root_;
};

// 由 build 期生成的 static_assets_data.cpp 提供。tests 也可以引用它直接验证
// 嵌入完整性。
const std::map<std::string, std::pair<const unsigned char*, std::size_t>>&
    embedded_asset_map();

// 工厂:根据 web.static_dir 决定走嵌入还是磁盘。空 = 嵌入。non-empty +
// 路径不存在 → 抛 std::runtime_error,worker.cpp 启动期捕获后 fail-fast。
std::unique_ptr<AssetSource> make_asset_source(const std::string& static_dir);

// 根据 path 后缀拿 Content-Type。pure helper,易测。
std::string mime_type_for_path(const std::string& path);

} // namespace acecode::web
