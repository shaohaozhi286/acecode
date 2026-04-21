// 覆盖 src/history/input_history_store.cpp 的持久化契约：
//   - load 在文件缺失 / 坏行 / 字段缺失场景下的容错行为（绝不抛异常）
//   - append 的父目录自动创建、空串抑制、超阈值原子 rewrite 行为
//   - clear 的幂等删除
//   - 与输入模式前缀（`!`）的互相独立性（持久化层不解释前缀）
// 这是「下次启动按 ↑ 能找回上次命令」特性的核心契约；一旦破坏，用户
// 期待的 cross-session 输入复用会直接失效。

#include <gtest/gtest.h>

#include "history/input_history_store.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::InputHistoryStore;

namespace {

// 每个 TEST 独立的临时目录。用行号拼名避免跨用例冲突。
fs::path make_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_ih_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                                    ->current_test_info()->line()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    return base;
}

} // namespace

// 场景：目标文件根本不存在时，load 应返回空 vector 而非抛异常。
// 这是「首次在新工作目录启动 acecode」的默认路径，任何抛异常都会让
// TUI 初始化挂掉。
TEST(InputHistoryStore, LoadReturnsEmptyWhenFileMissing) {
    auto dir = make_tmp_dir("missing");
    auto path = (dir / "input_history.jsonl").string();
    auto entries = InputHistoryStore::load(path);
    EXPECT_TRUE(entries.empty());
}

// 场景：向一个尚不存在的父目录 append，store 应自动 create_directories
// 并成功写入。覆盖 `.acecode/projects/<hash>/` 首次被用到的路径。
TEST(InputHistoryStore, AppendCreatesFileAndParentDir) {
    auto dir = make_tmp_dir("autocreate");
    auto nested = dir / "nested" / "deeper";
    auto path = (nested / "input_history.jsonl").string();

    InputHistoryStore::append(path, "hello", 10);

    ASSERT_TRUE(fs::exists(path)) << "append must create the file on first write";
    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0], "hello");
}

// 场景：连续提交同一条命令，磁盘文件应只有 1 行，而不是叠加多份。
// 这是「对标 bash HISTCONTROL=ignoredups」的 D4 决策；用户肌肉记忆
// 依赖的「按 ↑ 一次就是上一条」不能被重复提交刷掉。
//
// 注意：相邻去重在 main.cpp 的 record_history lambda 里执行；这里只
// 验证 append 本身对同内容调用两次时不会炸（它自身不做去重，上层责任）。
// 所以测试方式是：直接以文件行数验证 append 行为确实是原样写两行，
// 然后 load 回来能看到两行——这就验证了「去重是上层职责」的边界。
TEST(InputHistoryStore, AppendStoresDuplicateWhenCallerAllowsIt) {
    auto dir = make_tmp_dir("duphonest");
    auto path = (dir / "input_history.jsonl").string();

    InputHistoryStore::append(path, "git status", 10);
    InputHistoryStore::append(path, "git status", 10);

    auto entries = InputHistoryStore::load(path);
    EXPECT_EQ(entries.size(), 2u)
        << "store 层不去重；相邻去重是 record_history lambda 的职责";
}

// 场景：向一个 max_entries=10 的 store 追加 15 条（全部不同），最终
// 加载回来应恰好是最近 10 条，且顺序与追加顺序一致（最老的 5 条被丢）。
TEST(InputHistoryStore, AppendTruncatesToMaxEntries) {
    auto dir = make_tmp_dir("trunc");
    auto path = (dir / "input_history.jsonl").string();

    for (int i = 1; i <= 15; ++i) {
        InputHistoryStore::append(path, std::string("cmd-") + std::to_string(i), 10);
    }

    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), 10u);
    for (size_t i = 0; i < 10; ++i) {
        // 最新 10 条对应 cmd-6 .. cmd-15
        EXPECT_EQ(entries[i], std::string("cmd-") + std::to_string(6 + static_cast<int>(i)));
    }
    // .tmp 不得残留
    EXPECT_FALSE(fs::exists(path + ".tmp"));
}

// 场景：手工构造一个含非法 JSON 行的文件，load 应跳过坏行并返回合法
// 行的原始顺序，绝不抛异常。这是 D6 的容错承诺。
TEST(InputHistoryStore, LoadSkipsMalformedLines) {
    auto dir = make_tmp_dir("malformed");
    auto path = (dir / "input_history.jsonl").string();

    {
        std::ofstream ofs(path);
        ofs << R"({"text":"alpha"})" << '\n';
        ofs << R"(this is not JSON)" << '\n';   // 坏行
        ofs << R"({"text":"beta"})" << '\n';
        ofs << R"({"text":)" << '\n';           // 半截 JSON
        ofs << R"({"text":"gamma"})" << '\n';
    }

    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0], "alpha");
    EXPECT_EQ(entries[1], "beta");
    EXPECT_EQ(entries[2], "gamma");
}

// 场景：JSON 对象合法但缺少 `text` 字段（或 text 非字符串），整行应被
// 跳过。这防止其他工具日后误把其它键写到同一个文件而污染历史恢复。
TEST(InputHistoryStore, LoadSkipsMissingTextField) {
    auto dir = make_tmp_dir("notext");
    auto path = (dir / "input_history.jsonl").string();

    {
        std::ofstream ofs(path);
        ofs << R"({"foo":"bar"})" << '\n';       // 缺 text
        ofs << R"({"text":42})" << '\n';          // text 非 string
        ofs << R"({"text":"keep-me"})" << '\n';
    }

    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0], "keep-me");
}

// 场景：append 之后 clear 应彻底删掉磁盘文件，再 load 返回空。这是
// /history clear 子命令的基础。
TEST(InputHistoryStore, ClearRemovesFile) {
    auto dir = make_tmp_dir("clear");
    auto path = (dir / "input_history.jsonl").string();

    InputHistoryStore::append(path, "something", 10);
    ASSERT_TRUE(fs::exists(path));

    InputHistoryStore::clear(path);
    EXPECT_FALSE(fs::exists(path));

    auto entries = InputHistoryStore::load(path);
    EXPECT_TRUE(entries.empty());
}

// 场景：Shell 模式条目以 `!` 开头。持久化层必须把它原样存、原样读回，
// 不能剥前缀。这保证 input_history 混合 Normal / Shell 两种模式的
// 既有 `prepend_mode_prefix` / `parse_mode_prefix` 流水线不受影响。
TEST(InputHistoryStore, ModePrefixRoundTrip) {
    auto dir = make_tmp_dir("mode");
    auto path = (dir / "input_history.jsonl").string();

    InputHistoryStore::append(path, "!ls -la", 10);
    InputHistoryStore::append(path, "explain recursion", 10);

    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0], "!ls -la") << "shell 前缀 ! 必须保留";
    EXPECT_EQ(entries[1], "explain recursion");
}

// 场景：超阈值触发 rewrite 之后，文件应为合法 JSONL，行数 = max_entries，
// 且临时文件 `<path>.tmp` 不残留。防止将来有人改成非原子写入却没被
// 测试拦住。
TEST(InputHistoryStore, AtomicRewriteDoesNotLoseDataOnTruncation) {
    auto dir = make_tmp_dir("atomic");
    auto path = (dir / "input_history.jsonl").string();
    const int cap = 3;

    for (int i = 1; i <= 5; ++i) {
        InputHistoryStore::append(path, std::string("e") + std::to_string(i), cap);
    }

    // 每行必须都是合法 JSON 且恰好 cap 行
    std::ifstream ifs(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), static_cast<size_t>(cap));
    for (const auto& l : lines) {
        auto j = nlohmann::json::parse(l);
        EXPECT_TRUE(j.contains("text") && j["text"].is_string());
    }

    auto entries = InputHistoryStore::load(path);
    ASSERT_EQ(entries.size(), static_cast<size_t>(cap));
    EXPECT_EQ(entries[0], "e3");
    EXPECT_EQ(entries[1], "e4");
    EXPECT_EQ(entries[2], "e5");

    EXPECT_FALSE(fs::exists(path + ".tmp"))
        << ".tmp 在 rename 成功后应被 OS 清理，残留说明 rewrite 非原子";
}

// 场景：append 传入空串 / 纯空白 / max_entries <= 0 时，应被静默忽略，
// 磁盘文件保持不变。这避免用户按 Enter 把空行一直刷到磁盘上。
TEST(InputHistoryStore, AppendSuppressesEmptyOrInvalidArgs) {
    auto dir = make_tmp_dir("empty");
    auto path = (dir / "input_history.jsonl").string();

    InputHistoryStore::append(path, "", 10);
    InputHistoryStore::append(path, "   \t  ", 10);
    InputHistoryStore::append(path, "valid", 0);    // max_entries<=0 应忽略
    InputHistoryStore::append(path, "valid", -1);

    EXPECT_FALSE(fs::exists(path));
    EXPECT_TRUE(InputHistoryStore::load(path).empty());
}
