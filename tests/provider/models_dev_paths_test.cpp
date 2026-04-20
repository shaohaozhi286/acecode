// 覆盖 src/provider/models_dev_paths.{hpp,cpp} 的查找顺序：
// ACECODE_MODELS_DEV_DIR > <argv0_dir>/../share/acecode/models_dev > /usr/share/...
// 测试用 setenv/unsetenv 切环境，构造合成 install 布局来精确控制命中情况。
//
// 注意：这些用例修改进程级环境变量，gtest 单线程执行 OK；如果将来切并行执行
// 模式，需要换用 fixture 隔离。

#include <gtest/gtest.h>

#include "provider/models_dev_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
void set_env(const char* k, const char* v) {
    if (v) ::setenv(k, v, 1);
    else ::unsetenv(k);
}
#endif

fs::path make_seed_dir(const fs::path& parent, const std::string& name) {
    fs::path p = parent / name;
    fs::create_directories(p);
    std::ofstream(p / "api.json") << "{}";
    return p;
}

fs::path tmp_root(const std::string& tag) {
    auto root = fs::temp_directory_path() / ("acecode_paths_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

} // namespace

// 场景：env 变量优先于其它路径，并且必须真的存在 api.json 才算命中。
TEST(ModelsDevPaths, EnvVarBeatsInstallLayout) {
    auto env_dir = tmp_root("env");
    auto good_seed = make_seed_dir(env_dir, "models_dev");

    auto install = tmp_root("install");
    auto exe_dir = install / "bin";
    fs::create_directories(exe_dir);
    make_seed_dir(install / "share" / "acecode", "models_dev");

    set_env("ACECODE_MODELS_DEV_DIR", good_seed.string().c_str());
    auto found = acecode::find_models_dev_dir(exe_dir.string());
    set_env("ACECODE_MODELS_DEV_DIR", nullptr);

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::canonical(*found), fs::canonical(good_seed));
}

// 场景：env 设置但目录不含 api.json 时应被忽略，回退到 install 布局。
TEST(ModelsDevPaths, EnvDirWithoutApiJsonFallsBack) {
    auto env_dir = tmp_root("env_empty");  // exists but no api.json

    auto install = tmp_root("install_fb");
    auto exe_dir = install / "bin";
    fs::create_directories(exe_dir);
    auto bundled = make_seed_dir(install / "share" / "acecode", "models_dev");

    set_env("ACECODE_MODELS_DEV_DIR", env_dir.string().c_str());
    auto found = acecode::find_models_dev_dir(exe_dir.string());
    set_env("ACECODE_MODELS_DEV_DIR", nullptr);

    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::canonical(*found), fs::canonical(bundled));
}

// 场景：argv0_dir 为空 + env 缺失 + /usr/share 大概率不存在 → 返回 nullopt。
TEST(ModelsDevPaths, AllMissingReturnsNullopt) {
    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
    auto found = acecode::find_models_dev_dir("");
    // /usr/share/acecode/models_dev 在 dev 机器上通常不存在；如果某天出现就跳过
    if (found.has_value()) {
        GTEST_SKIP() << "system seed dir present, can't assert nullopt";
    }
    EXPECT_FALSE(found.has_value());
}
