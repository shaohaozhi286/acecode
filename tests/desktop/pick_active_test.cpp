// 覆盖 src/desktop/pick_active.cpp。这条选择规则决定 desktop 启动时哪个 workspace
// 自动 spawn daemon — 选错会让用户每次启动都要手动切。优先级是 spec 锚定的契约。

#include <gtest/gtest.h>

#include "desktop/pick_active.hpp"
#include "desktop/workspace_registry.hpp"
#include "utils/cwd_hash.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using acecode::compute_cwd_hash;
using acecode::desktop::WorkspaceRegistry;
using acecode::desktop::pick_active;

namespace {

// fixture: 临时 projects_dir,seed 后让 build_into 把内容 scan 进调用方的 registry。
// WorkspaceRegistry 不可拷贝 / 移动(内部 mutex),所以不能按值返回。
class TmpRegistry {
public:
    TmpRegistry() {
        path_ = (fs::temp_directory_path() / "acecode_pick_active_test").string();
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TmpRegistry() { std::error_code ec; fs::remove_all(path_, ec); }

    // 给一个 cwd 创建对应的 .acecode/projects/<hash>/workspace.json,然后下次 build_into 时入册。
    void seed(const std::string& cwd, const std::string& name = "n") {
        WorkspaceRegistry tmp;
        tmp.register_new(path_, cwd);
        if (!name.empty()) {
            tmp.set_name(path_, compute_cwd_hash(cwd), name);
        }
    }

    void build_into(WorkspaceRegistry& r) {
        r.scan(path_);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

} // namespace

// 场景: 空 registry + 空参 → 返回空字符串(让调用方渲染 onboarding)
TEST(PickActive, EmptyRegistryReturnsEmpty) {
    WorkspaceRegistry r;
    EXPECT_EQ(pick_active("", "", r), "");
}

// 场景: 优先级 1 — last_active 在 registry 里就用它,即使 process cwd 也匹配另一项
TEST(PickActive, LastActivePreferred) {
    TmpRegistry t;
    t.seed("/a/b/c");
    t.seed("/x/y/z");
    WorkspaceRegistry reg; t.build_into(reg);
    auto h_target = compute_cwd_hash("/a/b/c");
    auto h_other  = compute_cwd_hash("/x/y/z");
    EXPECT_EQ(pick_active(h_target, "/x/y/z", reg), h_target);
    // 反过来也成立
    EXPECT_EQ(pick_active(h_other, "/a/b/c", reg), h_other);
}

// 场景: last_active 不在 registry → 跳到 process cwd 兜底
TEST(PickActive, StaleLastActiveFallsThrough) {
    TmpRegistry t;
    t.seed("/here/now");
    WorkspaceRegistry reg; t.build_into(reg);
    auto h = pick_active("deadbeef00000000", "/here/now", reg);
    EXPECT_EQ(h, compute_cwd_hash("/here/now"));
}

// 场景: process cwd 不在 registry,last_active 也无效 → registry 第一项
TEST(PickActive, FirstEntryFallback) {
    TmpRegistry t;
    t.seed("/only/one");
    WorkspaceRegistry reg; t.build_into(reg);
    auto h = pick_active("not-in-reg", "/some/random/cwd", reg);
    auto v = reg.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(h, v.front().hash);
}

// 场景: 所有线索都断了(registry 空,无 last_active,无 cwd 命中)
TEST(PickActive, AllFallthroughEmpty) {
    WorkspaceRegistry r;
    EXPECT_EQ(pick_active("", "/random", r), "");
}

// 场景: process_cwd 空字符串不应被误用做 hash 来 lookup
TEST(PickActive, EmptyProcessCwdSkipped) {
    TmpRegistry t;
    t.seed("/a");
    WorkspaceRegistry reg; t.build_into(reg);
    auto h = pick_active("", "", reg);
    // 没 last_active,process cwd 空 → 第一项兜底
    auto v = reg.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(h, v.front().hash);
}
