#include <gtest/gtest.h>

#include "gitinfo/git_context_core.hpp"

#include <string>

using namespace acecode::gitinfo;

// ---- is_safe_ref_name ------------------------------------------------------

// 场景:合法的常见分支名(含斜杠层级、点号版本、dependabot 风格 @)。
// 期望:全部通过白名单校验。
TEST(GitContextCoreTest, SafeRefNameAcceptsCommonBranchNames) {
    EXPECT_TRUE(is_safe_ref_name("main"));
    EXPECT_TRUE(is_safe_ref_name("feature/foo-bar"));
    EXPECT_TRUE(is_safe_ref_name("release-1.2.3+build"));
    EXPECT_TRUE(is_safe_ref_name("dependabot/npm_and_yarn/@types/node-18.0.0"));
    EXPECT_TRUE(is_safe_ref_name("worktree-ses-abc12345"));
}

// 场景:.git/HEAD 是可篡改明文,攻击者可能塞入 shell 元字符、路径穿越、
// 参数注入(前导 -)。这些值会进 prompt / REST 响应 / git argv。
// 期望:全部拒绝。
TEST(GitContextCoreTest, SafeRefNameRejectsDangerousNames) {
    EXPECT_FALSE(is_safe_ref_name(""));
    EXPECT_FALSE(is_safe_ref_name("-D"));                 // 参数注入
    EXPECT_FALSE(is_safe_ref_name("/abs"));               // 前导斜杠
    EXPECT_FALSE(is_safe_ref_name("a..b"));               // 路径穿越
    EXPECT_FALSE(is_safe_ref_name("a/./b"));              // '.' 段归一歧义
    EXPECT_FALSE(is_safe_ref_name("a//b"));               // 空段
    EXPECT_FALSE(is_safe_ref_name("a/"));                 // 尾空段
    EXPECT_FALSE(is_safe_ref_name("$(rm -rf x)"));        // shell 元字符
    EXPECT_FALSE(is_safe_ref_name("a b"));                // 空白
    EXPECT_FALSE(is_safe_ref_name("名字"));               // 非 ASCII
    EXPECT_FALSE(is_safe_ref_name("a\nb"));               // 换行
}

// 场景:校验从 ref 文件读到的 SHA。git 只写全长 40(SHA-1)/64(SHA-256)
// 小写 hex,其它任何形态都可疑。
// 期望:只接受全长小写 hex。
TEST(GitContextCoreTest, ValidGitShaOnlyFullLengthLowercaseHex) {
    EXPECT_TRUE(is_valid_git_sha(std::string(40, 'a')));
    EXPECT_TRUE(is_valid_git_sha(std::string(64, '0')));
    EXPECT_FALSE(is_valid_git_sha("abc123"));                  // 缩写
    EXPECT_FALSE(is_valid_git_sha(std::string(40, 'A')));      // 大写
    EXPECT_FALSE(is_valid_git_sha(std::string(39, 'a')));      // 长度差一
    EXPECT_FALSE(is_valid_git_sha(std::string(40, 'g')));      // 非 hex
}

// ---- truncate_status_for_snapshot ------------------------------------------

// 场景:status 输出不超过上限(阈值 2000 与 Claude Code MAX_STATUS_CHARS
// 对齐,超出部分对模型价值低且挤占 context)。
// 期望:原样返回,不加截断说明。
TEST(GitContextCoreTest, TruncateStatusKeepsShortOutput) {
    std::string s = "M  src/a.cpp\n?? b.txt";
    EXPECT_EQ(truncate_status_for_snapshot(s), s);
}

// 场景:status 超过 2000 字符(大量改动文件)。
// 期望:截断到上限并追加"用 bash 工具跑 git status"的提示。
TEST(GitContextCoreTest, TruncateStatusCutsOversizedOutput) {
    std::string s(3000, 'x');
    std::string out = truncate_status_for_snapshot(s);
    EXPECT_LT(out.size(), s.size());
    EXPECT_NE(out.find("truncated because it exceeds 2k characters"),
              std::string::npos);
    EXPECT_EQ(out.substr(0, 2000), s.substr(0, 2000));
}

// 场景:截断点恰好落在多字节 UTF-8 字符中间(中文文件名很常见)。
// 期望:回退到序列边界,输出前缀仍是合法 UTF-8,不产生半个字符。
TEST(GitContextCoreTest, TruncateStatusRespectsUtf8Boundary) {
    // 1999 个 ASCII + 一个三字节汉字:字节 2000/2001 是继续字节。
    std::string s(1999, 'x');
    s += "\xE4\xB8\xAD"; // "中"
    s += std::string(100, 'y');
    std::string out = truncate_status_for_snapshot(s);
    // 截断点必须回退到 1999(汉字起始),不能停在继续字节上。
    std::size_t marker = out.find("\n... (truncated");
    ASSERT_NE(marker, std::string::npos);
    EXPECT_EQ(marker, 1999u);
}

// ---- format_git_status_snapshot --------------------------------------------

// 场景:完整快照(有分支、默认分支、git user、改动、提交历史)。
// 期望:五段结构齐全,与 Claude Code 的 gitStatus 文本结构一致。
TEST(GitContextCoreTest, FormatSnapshotFullParts) {
    SnapshotParts p;
    p.branch = "feature/x";
    p.default_branch = "main";
    p.user_name = "alice";
    p.status_short = "M  a.cpp";
    p.log_oneline = "abc1234 fix stuff";
    std::string out = format_git_status_snapshot(p);
    EXPECT_NE(out.find("snapshot in time"), std::string::npos);
    EXPECT_NE(out.find("Current branch: feature/x"), std::string::npos);
    EXPECT_NE(out.find("Main branch (you will usually use this for PRs): main"),
              std::string::npos);
    EXPECT_NE(out.find("Git user: alice"), std::string::npos);
    EXPECT_NE(out.find("Status:\nM  a.cpp"), std::string::npos);
    EXPECT_NE(out.find("Recent commits:\nabc1234 fix stuff"), std::string::npos);
}

// 场景:干净工作区 + 未配置 user.name + detached HEAD(branch 为空)。
// 期望:status 渲染 "(clean)",Git user 行整行省略,分支兜底 "HEAD"。
TEST(GitContextCoreTest, FormatSnapshotCleanDetachedNoUser) {
    SnapshotParts p;
    p.log_oneline = "abc1234 init";
    std::string out = format_git_status_snapshot(p);
    EXPECT_NE(out.find("Current branch: HEAD"), std::string::npos);
    EXPECT_NE(out.find("Status:\n(clean)"), std::string::npos);
    EXPECT_EQ(out.find("Git user:"), std::string::npos);
    // 默认分支未知时兜底 main(消费方是"PR 目标"提示,main 是最不坏的猜测)。
    EXPECT_NE(out.find("for PRs): main"), std::string::npos);
}
