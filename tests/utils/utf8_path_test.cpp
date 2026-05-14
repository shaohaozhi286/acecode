#include <gtest/gtest.h>

#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string_view>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace {

void append_utf16le(std::string& out, char16_t ch) {
    out.push_back(static_cast<char>(ch & 0xFF));
    out.push_back(static_cast<char>((ch >> 8) & 0xFF));
}

struct TempTree {
    fs::path path;
    TempTree() {
        path = fs::temp_directory_path() /
               ("acecode_utf8_path_test_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST(Utf8PathTest, RoundTripsNonAsciiPathThroughFilesystemBoundary) {
    TempTree tmp;
    const std::string leaf = u8"中文目录";
    fs::path dir = tmp.path / acecode::path_from_utf8(leaf);
    fs::create_directories(dir);

    std::string text = acecode::path_to_utf8(dir.filename());
    EXPECT_EQ(text, leaf);
    EXPECT_TRUE(acecode::is_valid_utf8(text));

    std::string generic = acecode::path_to_utf8_generic(dir);
    EXPECT_NE(generic.find(leaf), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(generic));
}

TEST(Utf8PathTest, CurrentPathIsUtf8) {
    std::string cwd = acecode::current_path_utf8();
    EXPECT_FALSE(cwd.empty());
    EXPECT_TRUE(acecode::is_valid_utf8(cwd));
}

TEST(Utf8PathTest, EnvironmentValuesAreUtf8Internally) {
    constexpr const char* kName = "ACECODE_UTF8_ENV_TEST";
    const std::string value = u8"值-中文";

#ifdef _WIN32
    SetEnvironmentVariableW(acecode::utf8_to_wide(kName).c_str(),
                            acecode::utf8_to_wide(value).c_str());
#else
    setenv(kName, value.c_str(), 1);
#endif

    EXPECT_EQ(acecode::getenv_utf8(kName), value);
    EXPECT_TRUE(acecode::is_valid_utf8(acecode::getenv_utf8(kName)));

#ifdef _WIN32
    SetEnvironmentVariableW(acecode::utf8_to_wide(kName).c_str(), nullptr);
#else
    unsetenv(kName);
#endif
}

TEST(EncodingTest, EnsureUtf8ConvertsUtf16LeBomText) {
    std::string bytes;
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (char16_t ch : std::u16string_view(u"name: 中文\n")) {
        append_utf16le(bytes, ch);
    }

    EXPECT_EQ(acecode::ensure_utf8(bytes), u8"name: 中文\n");
    EXPECT_TRUE(acecode::is_valid_utf8(acecode::ensure_utf8(bytes)));
}
