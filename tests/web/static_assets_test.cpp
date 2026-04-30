// 覆盖 src/web/static_assets.cpp。一旦回归:
//   - MIME map 错配 → 浏览器拒载 .js / .woff2
//   - FileSystemAssetSource 路径穿越 → 服务器读到 ".." 之外的文件
//   - EmbeddedAssetSource 找不到 index.html → 整个前端起不来

#include <gtest/gtest.h>

#include "web/static_assets.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using acecode::web::EmbeddedAssetSource;
using acecode::web::FileSystemAssetSource;
using acecode::web::embedded_asset_map;
using acecode::web::mime_type_for_path;

// 场景: 各扩展名的 MIME 类型必须正确映射 — 浏览器靠 Content-Type 决定 parse 策略
TEST(StaticAssets, MimeTypeMap) {
    EXPECT_EQ(mime_type_for_path("foo.html"), "text/html; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("a/b/foo.css"), "text/css; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("foo.js"), "application/javascript; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("foo.json"), "application/json; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("icon.svg"), "image/svg+xml");
    EXPECT_EQ(mime_type_for_path("icon.png"), "image/png");
    EXPECT_EQ(mime_type_for_path("icon.ico"), "image/x-icon");
    EXPECT_EQ(mime_type_for_path("font.woff2"), "font/woff2");
    EXPECT_EQ(mime_type_for_path("doc.md"), "text/markdown; charset=utf-8");
}

// 场景: 未知扩展名 fallback octet-stream(让浏览器决定下载/打开)
TEST(StaticAssets, UnknownExtFallback) {
    EXPECT_EQ(mime_type_for_path("noext"), "application/octet-stream");
    EXPECT_EQ(mime_type_for_path("foo.xyz"), "application/octet-stream");
}

// 场景: build 期生成的嵌入 map 必须含 index.html(否则前端起不来)
// 注: 此测试假设 web/index.html 已写入(本仓库 add-web-chat-ui 任务 4.1 已落地)
TEST(StaticAssets, EmbeddedMapContainsIndexHtml) {
    const auto& m = embedded_asset_map();
    auto it = m.find("index.html");
    ASSERT_NE(it, m.end()) << "index.html 必须被嵌入 — 检查 web/index.html 与 acecode_embed_assets";
    EXPECT_GT(it->second.second, 0u);
}

// 场景: EmbeddedAssetSource 命中 index.html
TEST(StaticAssets, EmbeddedSourceLookup) {
    EmbeddedAssetSource src;
    auto r = src.lookup("index.html");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content_type, "text/html; charset=utf-8");
    EXPECT_GT(r->size, 0u);
    EXPECT_NE(r->data, nullptr);
}

// 场景: 不存在的 path → nullopt(404)
TEST(StaticAssets, EmbeddedSourceMisses) {
    EmbeddedAssetSource src;
    EXPECT_FALSE(src.lookup("does-not-exist.js").has_value());
    EXPECT_FALSE(src.lookup("").has_value());
}

namespace {

class FsSourceTest : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        root = fs::temp_directory_path() / ("acecode_static_test_" + std::to_string(gen()));
        fs::create_directories(root);
        write_file(root / "foo.txt", "hello");
        fs::create_directories(root / "sub");
        write_file(root / "sub" / "bar.css", "body{}");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void write_file(const fs::path& p, const std::string& content) {
        std::ofstream(p, std::ios::binary).write(content.data(), content.size());
    }
};

} // namespace

// 场景: FileSystemAssetSource 命中扁平 + 嵌套文件
TEST_F(FsSourceTest, LookupFiles) {
    FileSystemAssetSource src(root.string());
    auto r1 = src.lookup("foo.txt");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r1->data), r1->size), "hello");

    auto r2 = src.lookup("sub/bar.css");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->content_type, "text/css; charset=utf-8");
}

// 场景: 不存在的文件 → nullopt
TEST_F(FsSourceTest, Missing) {
    FileSystemAssetSource src(root.string());
    EXPECT_FALSE(src.lookup("nope.js").has_value());
}

// 场景: 路径穿越尝试(.. /etc/passwd 之类)→ nullopt
TEST_F(FsSourceTest, PathTraversalRejected) {
    FileSystemAssetSource src(root.string());
    EXPECT_FALSE(src.lookup("../foo.txt").has_value());
    EXPECT_FALSE(src.lookup("../../etc/passwd").has_value());
}
