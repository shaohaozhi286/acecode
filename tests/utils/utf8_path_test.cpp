#include <gtest/gtest.h>

#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>

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
