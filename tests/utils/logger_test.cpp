// 覆盖 src/utils/logger.hpp 的 daemon 模式滚动日志(spec Section 12)。
// 单元测试这层主要验证三件事:
//   1. 老 init(file) API 行为不变 — TUI 仍然写 acecode.log,不滚动,不镜像 stderr
//   2. init_with_rotation() 创建 logs/ 目录并写到 daemon-{今日}.log
//   3. 跨日滚动: 注入假时钟,把"今天"推到下一天,下一条日志要落到新文件
//   4. foreground 模式: 每条日志同时落文件 + stderr
//
// Logger 是单例 — 每个 TEST 跑完必须把它重置回干净状态(SetUp/TearDown 用
// 临时目录隔离),否则后跑的 logger_xxx_test 会读到上一条的尾巴。

#include <gtest/gtest.h>

#include "utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// 把整个文件读成 string,测试断言里好直接 substr 查找。
std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

class LoggerRotationTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        // 给每个测试唯一的临时目录,避免单例 Logger 在测试间相互踩
        tmp_dir_ = fs::temp_directory_path() /
                   ("acecode_logger_test_" +
                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                    "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        // 测试收尾必须清空 logger 单例的 clock 注入 + 把句柄指到一个一次性
        // 文件,免得下一个 TEST 的 init 路径还拿着旧 ofs
        acecode::Logger::instance().set_clock_for_test({});
        acecode::Logger::instance().init((tmp_dir_ / "tearcleaner.log").string());

        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }
};

} // namespace

// 场景: 老 API init(file) 维持 TUI 行为 — 写到指定单文件,不创建带日期的文件,
// 也不输出 stderr。这是兼容性硬保证(main.cpp:770 一直这么调)。
TEST_F(LoggerRotationTest, SingleFileModeWritesToTargetFileOnly) {
    auto log_path = tmp_dir_ / "acecode.log";
    acecode::Logger::instance().init(log_path.string());

    // 重定向 stderr → 验证单文件模式不应镜像
    std::ostringstream captured;
    auto* old_buf = std::cerr.rdbuf(captured.rdbuf());
    LOG_INFO("hello-single-file");
    std::cerr.rdbuf(old_buf);

    auto content = read_file(log_path);
    EXPECT_NE(content.find("hello-single-file"), std::string::npos)
        << "单文件模式应写入指定文件";
    EXPECT_EQ(captured.str(), "")
        << "单文件模式不应镜像到 stderr";
}

// 场景: init_with_rotation 应该自动创建 logs 目录并打开 daemon-{今日}.log。
// 不依赖任何 LOG_* 调用 — 文件应该在 init 阶段就生成。
TEST_F(LoggerRotationTest, InitWithRotationCreatesDirAndInitialFile) {
    auto logs_dir = tmp_dir_ / "logs";
    ASSERT_FALSE(fs::exists(logs_dir));

    acecode::Logger::instance().init_with_rotation(
        logs_dir.string(), "daemon", /*mirror_stderr=*/false);

    EXPECT_TRUE(fs::exists(logs_dir)) << "init_with_rotation 应创建 logs/";

    LOG_INFO("first-line");

    // 该目录里应只有一个 daemon-YYYY-MM-DD.log,且包含 first-line
    int matched = 0;
    for (auto& e : fs::directory_iterator(logs_dir)) {
        auto fn = e.path().filename().string();
        if (fn.rfind("daemon-", 0) == 0 &&
            fn.size() > 14 /* daemon-YYYY-MM-DD.log */) {
            matched++;
            auto body = read_file(e.path());
            EXPECT_NE(body.find("first-line"), std::string::npos);
        }
    }
    EXPECT_EQ(matched, 1) << "应有且只有一个 daemon-{date}.log 被创建";
}

// 场景: 跨日滚动 — 用 set_clock_for_test 强制把"今天"推到 2099-01-02,
// 下一条 LOG 应该落在 daemon-2099-01-02.log,前一条仍留在原文件。
// 这是 spec Section 12.2 的核心断言。
TEST_F(LoggerRotationTest, RotationCreatesNewFileWhenDateAdvances) {
    auto logs_dir = tmp_dir_ / "logs";
    acecode::Logger::instance().init_with_rotation(
        logs_dir.string(), "daemon", false);

    // 第一条用注入的"昨天"日期
    acecode::Logger::instance().set_clock_for_test([] { return std::string("2099-01-01"); });
    LOG_INFO("yesterday-line");

    // 然后跳到"今天"
    acecode::Logger::instance().set_clock_for_test([] { return std::string("2099-01-02"); });
    LOG_INFO("today-line");

    auto yesterday_path = logs_dir / "daemon-2099-01-01.log";
    auto today_path     = logs_dir / "daemon-2099-01-02.log";

    ASSERT_TRUE(fs::exists(yesterday_path)) << "昨天的文件应该存在";
    ASSERT_TRUE(fs::exists(today_path))     << "今天的文件应该存在";

    auto y = read_file(yesterday_path);
    auto t = read_file(today_path);
    EXPECT_NE(y.find("yesterday-line"), std::string::npos);
    EXPECT_EQ(y.find("today-line"), std::string::npos)
        << "今天的日志不应回流到昨天的文件";
    EXPECT_NE(t.find("today-line"), std::string::npos);
    EXPECT_EQ(t.find("yesterday-line"), std::string::npos);
}

// 场景: foreground 模式同时输出 stderr + 文件(spec 12.3)。
TEST_F(LoggerRotationTest, ForegroundModeMirrorsToStderr) {
    auto logs_dir = tmp_dir_ / "logs";
    acecode::Logger::instance().init_with_rotation(
        logs_dir.string(), "daemon", /*mirror_stderr=*/true);

    std::ostringstream captured;
    auto* old_buf = std::cerr.rdbuf(captured.rdbuf());
    LOG_WARN("mirrored-line");
    std::cerr.rdbuf(old_buf);

    EXPECT_NE(captured.str().find("mirrored-line"), std::string::npos)
        << "foreground 模式应镜像到 stderr";

    // 文件那一份也应该有
    bool found = false;
    for (auto& e : fs::directory_iterator(logs_dir)) {
        if (read_file(e.path()).find("mirrored-line") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "foreground 模式同样要写文件";
}

// 场景: 单文件模式下注入 clock 不应触发任何滚动行为(因 rotation_enabled_=false)。
// 这是回归保护 — 万一有人在测试代码里漏掉 set_clock_for_test({}) 重置,也不应
// 误改单文件路径行为。
TEST_F(LoggerRotationTest, SingleFileModeIgnoresClockOverride) {
    auto log_path = tmp_dir_ / "acecode.log";
    acecode::Logger::instance().init(log_path.string());
    acecode::Logger::instance().set_clock_for_test([] { return std::string("2099-01-01"); });
    LOG_INFO("still-here");

    EXPECT_TRUE(fs::exists(log_path));
    EXPECT_NE(read_file(log_path).find("still-here"), std::string::npos);

    // 不应在 tmp_dir_ 下出现 daemon-*.log 这种产物
    for (auto& e : fs::directory_iterator(tmp_dir_)) {
        auto fn = e.path().filename().string();
        EXPECT_EQ(fn.rfind("daemon-", 0), std::string::npos)
            << "单文件模式不应被 clock 注入误产 daemon-*.log: " << fn;
    }
}
