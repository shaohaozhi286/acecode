// 覆盖 src/lsp/lsp_uri.{hpp,cpp}:路径 <-> file URI 转换与缓存 key 规范化。
//
// server 回推的 publishDiagnostics URI 必须能匹配回我们 touch 时的路径 key,
// 否则诊断永远等不到 —— 这是编辑后诊断链路里最容易碎的一环(Windows 盘符
// 大小写、分隔符、百分号编码三处都可能不一致)。
//
// 覆盖项:
//   - Windows 盘符路径 → file:///c:/...(盘符小写、反斜杠转正、空格编码)
//   - POSIX 绝对路径 → file:///...
//   - file URI → 路径(百分号解码、去前导斜杠、非 file scheme 拒绝)
//   - 大小写/分隔符不同的同一路径 → 同一个 normalize key(Windows)
//   - to_uri → to_path round-trip

#include <gtest/gtest.h>

#include "lsp/lsp_uri.hpp"

using acecode::lsp::file_uri_to_path;
using acecode::lsp::normalize_path_key;
using acecode::lsp::path_to_file_uri;

// 场景:Windows 路径转 URI —— 盘符统一小写(clangd 等 server 的回推形式),
// 反斜杠转 '/',空格百分号编码。
TEST(LspUri, WindowsPathToUri) {
    EXPECT_EQ(path_to_file_uri("C:\\proj\\a b\\main.cpp"),
              "file:///c:/proj/a%20b/main.cpp");
}

// 场景:POSIX 绝对路径转 URI。
TEST(LspUri, PosixPathToUri) {
    EXPECT_EQ(path_to_file_uri("/home/user/main.cpp"), "file:///home/user/main.cpp");
}

// 场景:URI 转回本地路径 —— 百分号解码 + Windows 下去掉盘符前导斜杠。
TEST(LspUri, UriToPath) {
    auto path = file_uri_to_path("file:///c:/proj/a%20b/main.cpp");
    ASSERT_TRUE(path.has_value());
#ifdef _WIN32
    EXPECT_EQ(*path, "c:\\proj\\a b\\main.cpp");
#else
    EXPECT_EQ(*path, "c:/proj/a b/main.cpp");
#endif
}

// 场景:非 file scheme(http 等)一律拒绝 —— 诊断只关心本地文件。
TEST(LspUri, NonFileSchemeRejected) {
    EXPECT_FALSE(file_uri_to_path("http://example.com/a.cpp").has_value());
    EXPECT_FALSE(file_uri_to_path("untitled:Untitled-1").has_value());
}

// 场景:round-trip —— 我们发出的 URI 被 server 原样回推时必须能还原路径,
// 且 normalize 后与原路径 key 相等(诊断缓存命中的最小保证)。
TEST(LspUri, RoundTripKeyStable) {
#ifdef _WIN32
    const std::string original = "N:\\Users\\shao\\proj\\src\\main.cpp";
#else
    const std::string original = "/users/shao/proj/src/main.cpp";
#endif
    auto back = file_uri_to_path(path_to_file_uri(original));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(normalize_path_key(*back), normalize_path_key(original));
}

// 场景(Windows 特有):同一文件的大小写/分隔符变体 → 同一个缓存 key。
// clangd 会把 `N:` 改写成 `n:` 回推,NTFS 大小写不敏感,key 必须归一。
TEST(LspUri, NormalizeKeyFoldsCaseAndSeparators) {
#ifdef _WIN32
    EXPECT_EQ(normalize_path_key("N:\\Proj\\Main.CPP"),
              normalize_path_key("n:/proj/main.cpp"));
#else
    // POSIX 大小写敏感:仅分隔符归一,大小写保留。
    EXPECT_EQ(normalize_path_key("/a\\b/c"), "/a/b/c");
    EXPECT_NE(normalize_path_key("/a/B"), normalize_path_key("/a/b"));
#endif
}
