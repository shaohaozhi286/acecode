#include <gtest/gtest.h>

#include "worktree/worktree_core.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace acecode::worktree;

// ---- slug 校验 -------------------------------------------------------------

// 场景:普通字母数字 slug 与带 "/" 的嵌套 slug。
// 期望:校验通过(返回空错误串)—— 与 Claude Code validateWorktreeSlug 的
// 白名单一致([A-Za-z0-9._-],"/" 只作分段符)。
TEST(WorktreeSlug, AcceptsSimpleAndNestedSlugs) {
    EXPECT_EQ(validate_worktree_slug("feature-x"), "");
    EXPECT_EQ(validate_worktree_slug("user/feature.v2_final"), "");
    EXPECT_EQ(validate_worktree_slug("a"), "");
}

// 场景:slug 总长超过 64 字符(65 个 'a')。
// 期望:拒绝。64 这个上限来自 Claude Code 的 MAX_WORKTREE_SLUG_LENGTH,
// 目的是让目录名 / 分支名在所有平台的路径长度限制内可控。
TEST(WorktreeSlug, RejectsOverlongSlug) {
    EXPECT_EQ(validate_worktree_slug(std::string(64, 'a')), "");
    EXPECT_NE(validate_worktree_slug(std::string(65, 'a')), "");
}

// 场景:slug 含 "." / ".." 路径段(如 "../escape"),或以 "/" 开头/结尾
// 产生空段。期望:全部拒绝 —— slug 会被 join 进 .acecode/worktrees/,
// ".." 段会让路径逃出 worktrees 目录(路径穿越)。
TEST(WorktreeSlug, RejectsPathTraversalSegments) {
    EXPECT_NE(validate_worktree_slug(".."), "");
    EXPECT_NE(validate_worktree_slug("../escape"), "");
    EXPECT_NE(validate_worktree_slug("a/../b"), "");
    EXPECT_NE(validate_worktree_slug("a/./b"), "");
    EXPECT_NE(validate_worktree_slug("/abs"), "");
    EXPECT_NE(validate_worktree_slug("trailing/"), "");
    EXPECT_NE(validate_worktree_slug(""), "");
}

// 场景:slug 含白名单以外字符(空格、反斜杠、冒号、中文)。
// 期望:拒绝 —— 这些字符在 git 分支名或 Windows 路径里有歧义。
TEST(WorktreeSlug, RejectsIllegalCharacters) {
    EXPECT_NE(validate_worktree_slug("has space"), "");
    EXPECT_NE(validate_worktree_slug("back\\slash"), "");
    EXPECT_NE(validate_worktree_slug("colon:name"), "");
    EXPECT_NE(validate_worktree_slug("插件"), "");
}

// 场景:嵌套 slug "user/feature" 的扁平化与分支名生成。
// 期望:"/" → "+"(分支 D/F 冲突与目录嵌套删除两个问题的共同解),
// "+" 不在 slug 段白名单里所以映射是单射(不会与真名撞车)。
TEST(WorktreeSlug, FlattensNestedSlugForBranchAndPath) {
    EXPECT_EQ(flatten_slug("user/feature"), "user+feature");
    EXPECT_EQ(worktree_branch_name("user/feature"), "worktree-user+feature");
    EXPECT_EQ(worktree_branch_name("plain"), "worktree-plain");

    const std::string p = worktree_path_for("C:/repo", "user/feature");
    // 目录名必须用扁平化结果,否则嵌套目录会落在父 worktree 里面
    EXPECT_NE(p.find("user+feature"), std::string::npos);
    EXPECT_EQ(p.find("user/feature"), std::string::npos);
    EXPECT_NE(p.find("worktrees"), std::string::npos);
}

// ---- 随机 slug 生成 ---------------------------------------------------------

// 场景:未提供 name 时生成随机 slug。
// 期望:同种子可复现(测试可确定);产物本身能通过 slug 校验,
// 形如 "<形容词>-<名词>-<4位base36>"。
TEST(WorktreeSlug, GeneratedSlugIsValidAndDeterministic) {
    const std::string a = generate_worktree_slug(42);
    const std::string b = generate_worktree_slug(42);
    EXPECT_EQ(a, b);
    EXPECT_EQ(validate_worktree_slug(a), "");
    // 两个 '-' 分隔三段
    EXPECT_EQ(std::count(a.begin(), a.end(), '-'), 2);
}

// ---- PR 引用解析 ------------------------------------------------------------

// 场景:GitHub / GHE 风格 PR URL 与 "#N" 简写。
// 期望:解析出 PR 号;可带尾斜杠、query、hash。
TEST(WorktreePrReference, ParsesGithubUrlsAndHashForm) {
    EXPECT_EQ(parse_pr_reference("https://github.com/owner/repo/pull/123"), 123);
    EXPECT_EQ(parse_pr_reference("https://ghe.example.com/o/r/pull/7/"), 7);
    EXPECT_EQ(parse_pr_reference("https://github.com/o/r/pull/9?diff=split"), 9);
    EXPECT_EQ(parse_pr_reference("https://github.com/o/r/pull/9#discussion"), 9);
    EXPECT_EQ(parse_pr_reference("#456"), 456);
}

// 场景:非 PR 引用输入(普通 slug、GitLab/Bitbucket 风格 URL、裸数字)。
// 期望:返回 nullopt —— /pull/N 的路径形状是 GitHub 特有的,其它平台
// 的 URL 不能误判成 PR。
TEST(WorktreePrReference, RejectsNonPrInputs) {
    EXPECT_FALSE(parse_pr_reference("feature-x").has_value());
    EXPECT_FALSE(parse_pr_reference("123").has_value());
    EXPECT_FALSE(
        parse_pr_reference("https://gitlab.com/o/r/-/merge_requests/5").has_value());
    EXPECT_FALSE(
        parse_pr_reference("https://bitbucket.org/o/r/pull-requests/5").has_value());
    EXPECT_FALSE(parse_pr_reference("#").has_value());
}

// ---- .worktreeinclude 解析与匹配 --------------------------------------------

// 场景:.worktreeinclude 文本含注释、空行、首尾空白与 CRLF。
// 期望:只留下有效 pattern,内容裁掉空白。
TEST(WorktreeInclude, ParsesPatternsSkippingCommentsAndBlanks) {
    const std::string content =
        "# 注释行\n"
        "\n"
        "  .env  \r\n"
        "config/*.key\n"
        "   # 缩进注释\n";
    auto patterns = parse_worktree_include_patterns(content);
    ASSERT_EQ(patterns.size(), 2u);
    EXPECT_EQ(patterns[0], ".env");
    EXPECT_EQ(patterns[1], "config/*.key");
}

// 场景:不含 "/" 的 pattern(gitignore 语义:在任意深度按 basename 匹配)。
// 期望:".env" 命中根下与子目录下的同名文件;"*.log" 命中任意深度的 .log。
TEST(WorktreeInclude, BasenamePatternMatchesAtAnyDepth) {
    WorktreeIncludeMatcher m;
    m.add_patterns({".env", "*.log"});
    EXPECT_TRUE(m.matches(".env"));
    EXPECT_TRUE(m.matches("packages/app/.env"));
    EXPECT_TRUE(m.matches("deep/dir/x.log"));
    EXPECT_FALSE(m.matches("env"));
    EXPECT_FALSE(m.matches("x.log.bak"));
}

// 场景:含 "/" 的 pattern(锚定到仓库根)与首 "/" 显式锚定。
// 期望:"config/api.key" 只命中根下这一条路径,不命中
// "other/config/api.key";"*" 不跨目录层级。
TEST(WorktreeInclude, SlashPatternsAreAnchoredToRoot) {
    WorktreeIncludeMatcher m;
    m.add_patterns({"config/api.key", "/root.pem", "certs/*.crt"});
    EXPECT_TRUE(m.matches("config/api.key"));
    EXPECT_FALSE(m.matches("other/config/api.key"));
    EXPECT_TRUE(m.matches("root.pem"));
    EXPECT_FALSE(m.matches("sub/root.pem"));
    EXPECT_TRUE(m.matches("certs/a.crt"));
    EXPECT_FALSE(m.matches("certs/nested/a.crt"));  // "*" 不匹配 "/"
}

// 场景:目录 pattern("build/")。
// 期望:命中目录本身与其内部文件(祖先目录命中 ⇒ 内部路径全部命中,
// 与 npm ignore 库对 gitignore 的实现一致)。
TEST(WorktreeInclude, DirectoryPatternMatchesContents) {
    WorktreeIncludeMatcher m;
    m.add_patterns({"build/"});
    EXPECT_TRUE(m.matches("build/"));
    EXPECT_TRUE(m.matches("build/out/app.exe"));
    // dir-only pattern 不命中同名普通文件
    EXPECT_FALSE(m.matches("build"));
}

// 场景:"!" 取反,后写的规则覆盖先写的(gitignore last-match-wins)。
// 期望:"*.key" 之后的 "!public.key" 把该文件排除出匹配集。
TEST(WorktreeInclude, NegationLastMatchWins) {
    WorktreeIncludeMatcher m;
    m.add_patterns({"*.key", "!public.key"});
    EXPECT_TRUE(m.matches("secret.key"));
    EXPECT_FALSE(m.matches("public.key"));
    EXPECT_FALSE(m.matches("keys/public.key"));
}

// 场景:"**" 通配(前缀 "**/"、中缀 "/**/"、尾缀 "/**")。
// 期望:与 gitignore 语义一致 —— "**/" 匹配零或多层目录。
TEST(WorktreeInclude, DoubleStarSpansDirectories) {
    WorktreeIncludeMatcher m;
    m.add_patterns({"**/generated.ts", "src/**/fixture.json", "vendor/**"});
    EXPECT_TRUE(m.matches("generated.ts"));
    EXPECT_TRUE(m.matches("a/b/generated.ts"));
    EXPECT_TRUE(m.matches("src/fixture.json"));           // 中缀 ** 允许零层
    EXPECT_TRUE(m.matches("src/x/y/fixture.json"));
    EXPECT_FALSE(m.matches("other/fixture.json"));
    EXPECT_TRUE(m.matches("vendor/pkg/file.c"));
}

// ---- collapsed 目录展开计划 --------------------------------------------------

// 场景:git ls-files --directory 的输出里,完全被忽略的目录折叠成
// "node_modules/" 这样的单条目。pattern 没有明确指向其内部时不展开
// (展开要付出二次 ls-files 的成本,大仓库会退化成全量遍历)。
// 期望:普通文件直接进拷贝清单;无关 collapsed 目录不进展开清单。
TEST(WorktreeIncludePlan, MatchesFilesAndSkipsUnrelatedCollapsedDirs) {
    auto plan = plan_worktree_include_copy(
        {".env", "node_modules/", "dist/", "src/app.local.json"},
        {".env", "*.local.json"});
    ASSERT_EQ(plan.files_to_copy.size(), 2u);
    EXPECT_EQ(plan.files_to_copy[0], ".env");
    EXPECT_EQ(plan.files_to_copy[1], "src/app.local.json");
    EXPECT_TRUE(plan.dirs_to_expand.empty());
}

// 场景:pattern 明确指向 collapsed 目录内部的三种触发方式 ——
// (a) 字面前缀:"config/secrets/api.key" 落在 "config/secrets/" 下;
// (b) 锚定 glob 的字面前缀:"config/**/*.key" 覆盖 "config/secrets/";
// (c) 目录本身命中 pattern。
// 期望:对应目录进入展开清单;裸 basename pattern(如 "*.key")不触发
// 展开 —— 它匹配的是已被逐个列出的文件。
TEST(WorktreeIncludePlan, ExpandsCollapsedDirsOnlyWhenPatternTargetsInside) {
    // (a) 字面前缀
    auto plan_a = plan_worktree_include_copy(
        {"config/secrets/"}, {"config/secrets/api.key"});
    ASSERT_EQ(plan_a.dirs_to_expand.size(), 1u);
    EXPECT_EQ(plan_a.dirs_to_expand[0], "config/secrets/");

    // (b) 锚定 glob 字面前缀
    auto plan_b = plan_worktree_include_copy(
        {"config/secrets/"}, {"config/**/*.key"});
    ASSERT_EQ(plan_b.dirs_to_expand.size(), 1u);

    // (c) 目录本身命中(dir pattern)
    auto plan_c = plan_worktree_include_copy({"local-cache/"}, {"local-cache/"});
    ASSERT_EQ(plan_c.dirs_to_expand.size(), 1u);

    // 裸 basename pattern 不展开任何 collapsed 目录
    auto plan_d = plan_worktree_include_copy(
        {"node_modules/", "dist/"}, {"*.key"});
    EXPECT_TRUE(plan_d.dirs_to_expand.empty());
}
