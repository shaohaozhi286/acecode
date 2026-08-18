# Skill 休眠(Dormancy)管理 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 skill 使用次数记录与自动休眠管理——闲置 skill 从系统提示词自动列表中隐藏,手动调用/path 匹配保留,调用后自动恢复。

**Architecture:** 新建 `skill_usage_store` 模块管理 `~/.acecode/.skill_usage_state.json`(进程内 mutex + 原子 rename),在 agent_loop 注入点记录使用,在 `build_skills_index_context_prompt` 构建列表时实时判定 `is_dormant` 过滤。配置项 `skills.idleDays` 进 SkillsConfig。

**Tech Stack:** C++17, nlohmann/json, GoogleTest, CMake/Ninja, 沿用 `atomic_write_file` 模式

**Spec:** `docs/superpowers/plans/2026-08-18-skill-dormancy-design.md`

## Global Constraints

- 遵循 `.editorconfig`:UTF-8, LF, 4 空格缩进 C++
- 无 emoji/宽字符,ASCII 符号
- 使用 `EXPECT_*` 断言(除非失败使后续不安全)
- 测试用 `fs::temp_directory_path()`,不写仓库树
- 文件权限 0600,原子写(临时文件 + rename)
- `record_skill_usage` 是 best-effort,失败不阻断主流程

---

## File Structure

| 文件 | 职责 | 新建/修改 |
|------|------|:---:|
| `src/skills/skill_usage_store.hpp` | 数据模型 + 公开接口声明 | 新建 |
| `src/skills/skill_usage_store.cpp` | store 实现:读/写/判定/并发 | 新建 |
| `src/agent_loop.cpp` | 注入点记录 + 列表过滤 | 修改 |
| `src/config/config.hpp` | `SkillsConfig` 新增 `idle_days` | 修改 |
| `src/tui_state.hpp` | 新增 store 引用 | 修改 |
| `src/main.cpp` | 初始化 store + 传参 | 修改 |
| `src/tui/settings/management_center.cpp` | TUI 展示次数/状态/pin | 修改 |
| `src/web/handlers/skills_handler.cpp` | Web API 返回状态 + pin | 修改 |
| `tests/skills/skill_usage_store_test.cpp` | store 单元测试 | 新建 |
| `src/CMakeLists.txt` | 注册新源文件 | 修改 |
| `tests/CMakeLists.txt` | 注册新测试 | 修改 |

---

### Task 1: 数据模型与 store 接口声明

**Files:**
- Create: `src/skills/skill_usage_store.hpp`

**Interfaces:**
- Produces: `SkillUsageRecord`, `SkillUsageSummary`, `SkillUsageStore`, `parse_iso8601_to_epoch_ms`

- [ ] **Step 1: 编写 header**

```cpp
// src/skills/skill_usage_store.hpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace acecode {

struct SkillUsageRecord {
    std::string last_used_at;   // ISO8601
    std::uint64_t use_count = 0;
    bool pinned = false;
};

struct SkillUsageSummary {
    std::string name;
    std::uint64_t use_count = 0;
    std::string last_used_at;
    bool pinned = false;
    bool dormant = false;   // 实时判定
};

class SkillUsageStore {
public:
    explicit SkillUsageStore(std::string state_path);

    bool record(const std::string& skill_name, const std::string& now_iso);
    bool is_dormant(const std::string& skill_name,
                    std::int64_t now_epoch_ms,
                    std::int64_t idle_days_ms) const;
    bool set_pinned(const std::string& skill_name, bool pinned);
    std::vector<SkillUsageSummary> get_summary(
        std::int64_t now_epoch_ms, std::int64_t idle_days_ms) const;
    void reload();

private:
    std::string state_path_;
    mutable std::mutex mu_;
};

std::int64_t parse_iso8601_to_epoch_ms(const std::string& iso);

}  // namespace acecode
```

- [ ] **Step 2: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过(仅有声明,链接时符号缺失是预期)

- [ ] **Step 3: Commit**

```bash
git add src/skills/skill_usage_store.hpp
git commit -m "feat: add SkillUsageStore header with interface declarations"
```

---

### Task 2: Store 实现

**Files:**
- Create: `src/skills/skill_usage_store.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: `SkillUsageStore`, `SkillUsageRecord`, `SkillUsageSummary` from Task 1
- Produces: full `SkillUsageStore` impl, `parse_iso8601_to_epoch_ms`

- [ ] **Step 1: 编写实现**

```cpp
// src/skills/skill_usage_store.cpp
#include "skills/skill_usage_store.hpp"
#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {
constexpr int kStateVersion = 1;
constexpr std::size_t kMaxStateFileBytes = 1024 * 1024;  // 1 MB

nlohmann::json load_state_or_empty(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    if (fs::file_size(path, ec) > kMaxStateFileBytes) {
        LOG_WARN("[skill_usage] state file too large, ignoring");
        return nlohmann::json::object();
    }
    std::ifstream ifs(path);
    if (!ifs) return nlohmann::json::object();
    try {
        auto j = nlohmann::json::parse(ifs);
        if (!j.is_object() || j.value("version", 0) != kStateVersion) {
            LOG_WARN("[skill_usage] version mismatch, resetting");
            return nlohmann::json::object();
        }
        return j;
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("[skill_usage] parse error: " + std::string(e.what()));
        return nlohmann::json::object();
    }
}
}  // namespace

SkillUsageStore::SkillUsageStore(std::string state_path)
    : state_path_(std::move(state_path)) {}

bool SkillUsageStore::record(const std::string& skill_name,
                              const std::string& now_iso) {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    auto& skills = state["skills"];
    if (!skills.contains(skill_name)) {
        skills[skill_name] = {{"lastUsedAt", now_iso},
                              {"useCount", 1},
                              {"pinned", false}};
    } else {
        auto& entry = skills[skill_name];
        entry["lastUsedAt"] = now_iso;
        entry["useCount"] = entry.value("useCount", 0u) + 1u;
    }
    return atomic_write_file(state_path_, state.dump(2));
}

bool SkillUsageStore::is_dormant(const std::string& skill_name,
                                  std::int64_t now_epoch_ms,
                                  std::int64_t idle_days_ms) const {
    if (idle_days_ms == 0) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    auto& skills = state["skills"];
    if (!skills.contains(skill_name)) return false;
    auto& entry = skills[skill_name];
    if (entry.value("pinned", false)) return false;
    std::string last_used = entry.value("lastUsedAt", "");
    if (last_used.empty()) return false;
    std::int64_t last_ms = parse_iso8601_to_epoch_ms(last_used);
    if (last_ms == 0) return false;
    return (now_epoch_ms - last_ms) > idle_days_ms;
}

bool SkillUsageStore::set_pinned(const std::string& skill_name, bool pinned) {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    auto& skills = state["skills"];
    if (!skills.contains(skill_name)) {
        skills[skill_name] = {{"lastUsedAt", ""},
                              {"useCount", 0},
                              {"pinned", pinned}};
    } else {
        skills[skill_name]["pinned"] = pinned;
    }
    return atomic_write_file(state_path_, state.dump(2));
}

std::vector<SkillUsageSummary> SkillUsageStore::get_summary(
    std::int64_t now_epoch_ms, std::int64_t idle_days_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    std::vector<SkillUsageSummary> out;
    for (auto& [name, entry] : state["skills"].items()) {
        SkillUsageSummary s;
        s.name = name;
        s.use_count = entry.value("useCount", 0u);
        s.last_used_at = entry.value("lastUsedAt", "");
        s.pinned = entry.value("pinned", false);
        if (idle_days_ms > 0 && !s.pinned && !s.last_used_at.empty()) {
            std::int64_t last_ms = parse_iso8601_to_epoch_ms(s.last_used_at);
            s.dormant = (last_ms > 0) &&
                        (now_epoch_ms - last_ms) > idle_days_ms;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void SkillUsageStore::reload() {
    // load_state_or_empty on next access handles this
}

std::int64_t parse_iso8601_to_epoch_ms(const std::string& iso) {
    std::tm tm = {};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    int ms = 0;
    if (ss.peek() == '.') { ss.ignore(); ss >> ms; }
    auto tp = std::chrono::system_clock::from_time_t(
        std::mktime(&tm)) + std::chrono::milliseconds(ms);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count();
}

}  // namespace acecode
```

- [ ] **Step 2: 注册到 CMake**

Modify `src/CMakeLists.txt`:在 `skills/` 源文件列表追加 `skills/skill_usage_store.cpp`

- [ ] **Step 3: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 4: Commit**

```bash
git add src/skills/skill_usage_store.cpp src/CMakeLists.txt
git commit -m "feat: implement SkillUsageStore with JSON read/write"
```

---

### Task 3: Store 单元测试

**Files:**
- Create: `tests/skills/skill_usage_store_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SkillUsageStore`, `parse_iso8601_to_epoch_ms` from Task 2

- [ ] **Step 1: 编写测试**

```cpp
#include "skills/skill_usage_store.hpp"
#include <gtest/gtest.h>
#include <filesystem>
namespace fs = std::filesystem;

TEST(SkillUsageStoreTest, ParseIso8601) {
    auto ms = acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Z");
    EXPECT_GT(ms, 0);
    EXPECT_EQ(ms, acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("not-a-date"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms(""));
}

TEST(SkillUsageStoreTest, RecordCreatesEntry) {
    auto tmp = fs::temp_directory_path() / "acecode_test_skill_usage.json";
    fs::remove(tmp);
    acecode::SkillUsageStore store(tmp.string());
    EXPECT_TRUE(store.record("pdf", "2026-08-01T10:00:00Z"));
    EXPECT_FALSE(store.is_dormant("pdf", 1722500000000LL, 30LL * 86400000));
    auto s = store.get_summary(1722500000000LL, 30LL * 86400000);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].name, "pdf");
    EXPECT_EQ(s[0].use_count, 1u);
    EXPECT_FALSE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, DormantAfterThreshold) {
    auto tmp = fs::temp_directory_path() / "acecode_test_dormant.json";
    fs::remove(tmp);
    acecode::SkillUsageStore store(tmp.string());
    std::int64_t used_ms = 1722500000000LL;
    std::int64_t now_ms = used_ms + 40LL * 86400000;
    std::int64_t idle_ms = 30LL * 86400000;
    std::string used_iso = "2026-08-01T10:00:00Z";
    EXPECT_TRUE(store.record("xlsx", used_iso));
    EXPECT_TRUE(store.is_dormant("xlsx", now_ms, idle_ms));
    auto s = store.get_summary(now_ms, idle_ms);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_TRUE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, PinnedSkillNeverDormant) {
    auto tmp = fs::temp_directory_path() / "acecode_test_pinned.json";
    fs::remove(tmp);
    acecode::SkillUsageStore store(tmp.string());
    EXPECT_TRUE(store.record("pinned_skill", "2026-06-01T10:00:00Z"));
    EXPECT_TRUE(store.set_pinned("pinned_skill", true));
    std::int64_t now_ms = 1730000000000LL;
    std::int64_t idle_ms = 30LL * 86400000;
    EXPECT_FALSE(store.is_dormant("pinned_skill", now_ms, idle_ms));
    auto s = store.get_summary(now_ms, idle_ms);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, IdleDaysZeroDisablesFeature) {
    auto tmp = fs::temp_directory_path() / "acecode_test_zero.json";
    fs::remove(tmp);
    acecode::SkillUsageStore store(tmp.string());
    EXPECT_TRUE(store.record("skill", "2020-01-01T00:00:00Z"));
    EXPECT_FALSE(store.is_dormant("skill
", 0));
    auto s = store.get_summary(1730000000000LL, 0);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, IncrementUseCount) {
    auto tmp = fs::temp_directory_path() / "acecode_test_incr.json";
    fs::remove(tmp);
    acecode::SkillUsageStore store(tmp.string());
    store.record("pdf", "2026-08-01T10:00:00Z");
    store.record("pdf", "2026-08-02T10:00:00Z");
    store.record("pdf", "2026-08-03T10:00:00Z");
    auto s = store.get_summary(1722500000000LL, 30LL * 86400000);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].use_count, 3u);
    fs::remove(tmp);
}
```

- [ ] **Step 2: 注册到 tests/CMakeLists.txt**

在 `tests/CMakeLists.txt` 中添加(仿现有 `default_skill_seeder_test.cpp` 模式):
```cmake
add_executable(acecode_skill_usage_store_test
    skills/skill_usage_store_test.cpp
)
target_link_libraries(acecode_skill_usage_store_test ...)
add_test(NAME acecode_skill_usage_store_test ...)
```

- [ ] **Step 3: 运行测试,确认失败(如果 store 实现未完成则 link 失败)**

Run: `cmake --build build --target acecode_skill_usage_store_test && ./build/tests/acecode_skill_usage_store_test`
Expected: 7/7 tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/skills/skill_usage_store_test.cpp tests/CMakeLists.txt
git commit -m "test: add SkillUsageStore unit tests"
```

---

### Task 4: 配置项 `skills.idleDays`

**Files:**
- Modify: `src/config/config.hpp`

**Interfaces:**
- Produces: `SkillsConfig::idle_days` (int, default 30)

- [ ] **Step 1: 添加字段**

在 `src/config/config.hpp` 的 `SkillsConfig` struct 中追加:

```cpp
struct SkillsConfig {
    std::vector<std::string> disabled;
    std::vector<std::string> external_dirs;
    bool reuse_opencode = true;
    std::optional<std::vector<std::string>> allowed;
    int idle_days = 30;  // 新增:0=关闭休眠,>0=判定阈值
};
```

- [ ] **Step 2: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 3: Commit**

```bash
git add src/config/config.hpp
git commit -m "feat: add idle_days config to SkillsConfig"
```

---

### Task 5: 注入点记录 + 列表过滤

**Files:**
- Modify: `src/agent_loop.cpp`

**Interfaces:**
- Consumes: `SkillUsageStore` from Task 2, `SkillsConfig::idle_days` from Task 4
- Produces: injection recording + dormant filtering

- [ ] **Step 1: 在注入成功处插入 `record_skill_usage`**

在 `inject_explicit_skill_instructions` 返回后,对每个 `injected_skill_names` 调用:

```cpp
// agent_loop.cpp, after inject_explicit_skill_instructions(...)
if (skill_usage_store_) {
    auto now_iso = /* current time as ISO8601 */;
    for (const auto& name : skill_expansion.injected_skill_names) {
        skill_usage_store_->record(name, now_iso);
    }
}
```

生成 ISO8601 的辅助:
```cpp
#include <chrono>
#include <iomanip>
#include <sstream>

static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
```

- [ ] **Step 2: 在 `build_skills_index_context_prompt` 处加过滤**

调用处(agent_loop.cpp 约 957 行),在构建列表前:

```cpp
auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
auto idle_ms = config_ ? config_->skills().idle_days * 86400000LL : 0LL;

auto skill_list = skill_registry_->list();
std::vector<SkillMetadata> active_skills;
for (const auto& s : skill_list) {
    if (!skill_usage_store_ ||
        !skill_usage_store_->is_dormant(s.name, now_ms, idle_ms)) {
        active_skills.push_back(s);
    }
}
// 用 active_skills 替代原 skill_list 构建上下文
```

- [ ] **Step 3: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 4: Commit**

```bash
git add src/agent_loop.cpp
git commit -m "feat: record skill usage on injection and filter dormant skills"
```

---

### Task 6: 初始化与状态传递

**Files:**
- Modify: `src/tui_state.hpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `SkillUsageStore` from Task 2
- Produces: `tui_state.skill_usage_store` 可用

- [ ] **Step 1: 在 tui_state 添加 store 引用**

```cpp
// src/tui_state.hpp, 在 slash_command_usage_counts 附近
#include "skills/skill_usage_store.hpp"
// ...
std::shared_ptr<SkillUsageStore> skill_usage_store;
```

- [ ] **Step 2: 在 main.cpp 初始化 store**

```cpp
// src/main.cpp, 在 skill_registry 初始化后
auto home = /* acecode home dir */;
auto state_path = home + "/.skill_usage_state.json";
state.skill_usage_store = std::make_shared<SkillUsageStore>(state_path);
```

- [ ] **Step 3: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 4: Commit**

```bash
git add src/tui_state.hpp src/main.cpp
git commit -m "feat: wire SkillUsageStore into tui_state and main"
```

---

### Task 7: TUI 展示

**Files:**
- Modify: `src/tui/settings/management_center.cpp`

- [ ] **Step 1: 在 /skills 面板或管理中心的 skill 列表追加状态列**

在现有 skill 列表渲染处,为每个 skill 追加:

```cpp
// 在显示每个 skill 的行末追加
auto summary = skill_usage_store_->get_summary(
    now_epoch_ms(),
    config_.skills().idle_days * 86400000LL);
for (const auto& s : summary) {
    std::string status = s.dormant ? " [dormant]"
                       : s.pinned  ? " [pinned]"
                       :             " [active]";
    std::string info = "  used " + std::to_string(s.use_count) +
                       " times, last " + s.last_used_at;
    // 渲染到 TUI 行
}
```

- [ ] **Step 2: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 3: Commit**

```bash
git add src/tui/settings/management_center.cpp
git commit -m "feat: display skill usage stats and dormant status in TUI"
```

---

### Task 8: Web 展示

**Files:**
- Modify: `src/web/handlers/skills_handler.cpp`

- [ ] **Step 1: 在现有 GET /skills API 响应中追加 usage 字段**

```cpp
// 在 skills_handler 的 JSON 响应中追加
nlohmann::json usage_array = nlohmann::json::array();
if (skill_usage_store_) {
    for (const auto& s : skill_usage_store_->get_summary(
             now_epoch_ms(), idle_days_ms)) {
        usage_array.push_back({
            {"name", s.name},
            {"useCount", s.use_count},
            {"lastUsedAt", s.last_used_at},
            {"pinned", s.pinned},
            {"dormant", s.dormant}
        });
    }
}
response["usage"] = std::move(usage_array);
```

- [ ] **Step 2: 编译验证**

Run: `cmake --build build --target acecode --config Release`
Expected: 编译通过

- [ ] **Step 3: Commit**

```bash
git add src/web/handlers/skills_handler.cpp
git commit -m "feat: expose skill usage stats via Web API"
```

---

## Verification

完成所有 8 个 task 后:

```bash
cmake --build build --target acecode_unit_tests --config Release
ctest --test-dir build --output-on-failure
```

预期:所有现有测试 + 新增 `acecode_skill_usage_store_test` 全部通过。
