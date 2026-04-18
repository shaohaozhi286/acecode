# acecode Skill 实现流程

本文档梳理 acecode 当前 skill 系统的完整数据流、关键数据结构与源文件分工，便于二次开发与问题定位。目录扫描策略参照了 claudecodehaha（Claude Code 前端）的 `getProjectDirsUpToHome` 模型，但命名沿用 acecode 自身的 `.acecode/` 约定。

---

## 1. 核心目标

- **Progressive disclosure**：系统提示词里只告诉模型"有哪些 skill"（名字 + 一句话描述），完整内容由模型通过 `skill_view` 工具按需拉取。节省 token，扩展到几十个 skill 也不爆上下文。
- **两条激活路径**：
  1. **用户显式触发** — 在 TUI 中输入 `/<skill-name> [args]`，由 acecode 本地拦截并注入 SKILL.md 内容；
  2. **模型自主触发** — 模型调用 `skills_list` → `skill_view` 发现并加载 skill。
- **项目级覆盖全局** — cwd 内的 skill 可以覆盖同名全局 skill，便于单仓库特化。
- **平台感知** — `SKILL.md` 里 `platforms:` frontmatter 可限定 skill 仅在 Windows / macOS / Linux 可见。

---

## 2. 磁盘布局

单个 skill 的标准结构：

```
<root>/<category>/<name>/
├── SKILL.md              # 必需：YAML frontmatter + markdown 正文
├── references/           # 可选：参考资料（模型按需 skill_view 加载）
├── templates/            # 可选：模板文件
├── scripts/              # 可选：脚本
└── assets/               # 可选：二进制/其它资源
```

`<category>` 这一级是可选的。直接 `<root>/<name>/SKILL.md` 也合法，此时 skill 的 `category` 字段为空。

### 扫描根目录（优先级从高到低）

`main.cpp` 在启动时按下述顺序组装 `roots`（`SkillRegistry::scan()` 按名字 first-wins 去重，所以前面的根会覆盖后面的同名 skill）：

| # | 来源 | 路径 |
|---|------|------|
| 1 | 项目 walk（deepest first） | 从 `cwd` 一路向上到（但不含）HOME，每层的 `<dir>/.acecode/skills/` |
| 2 | 用户外部配置 | `config.skills.external_dirs` 里每一项（支持 `~` / `${VAR}` 展开） |
| 3 | 用户全局 | `~/.acecode/skills/`（不存在时自动创建） |

项目 walk 的收集由 `get_project_dirs_up_to_home(cwd)` 实现（`src/config/config.cpp`）：以 cwd 为起点逐级向上，遇到 HOME 或文件系统根则停止，HOME 本身**不包含**（它已由第 3 项覆盖）。

---

## 3. SKILL.md frontmatter

acecode 只识别 YAML frontmatter 的一个子集，解析器位于 `src/skills/frontmatter.{hpp,cpp}`。仅读取文件首 8KB 足以抓到 frontmatter。

| 字段 | 类型 | 用途 |
|------|------|------|
| `name` | 字符串 | skill 名；缺省时使用目录名并在日志中警告 |
| `description` | 字符串 | 一句话简介；缺省时用正文第一行非标题段 |
| `platforms` | 字符串列表 | 白名单：`windows` / `macos` / `linux`。空表示全平台 |
| `metadata.hermes.tags` / `metadata.tags` | 字符串列表 | 可选标签 |

支持三种 YAML 形态：标量、方括号列表（`[a, b, c]`）、连字符列表（多行 `- item`），以及一层嵌套映射（`metadata:` → `hermes:` → …）。任何解析失败的行会被静默跳过，不影响整体加载。

---

## 4. 扫描与内存索引

### SkillMetadata（`src/skills/skill_metadata.hpp`）

```cpp
struct SkillMetadata {
    std::string name;              // frontmatter 里的 name，或目录名
    std::string command_key;       // 归一化后的 kebab-case，用作 /<key>
    std::string description;
    std::string category;          // 扫描根下的一级子目录名，扁平布局为空
    std::filesystem::path skill_md_path;
    std::filesystem::path skill_dir;
    std::vector<std::string> platforms;
    std::vector<std::string> tags;
};
```

`command_key` 通过 `normalize_skill_command_key()` 生成：小写化、空格/下划线转连字符、保留字母数字、去掉其它字符、折叠多余连字符、修剪两端连字符。

### 扫描流程（`SkillRegistry::scan()`）

1. 读取当前 `roots_` 和 `disabled_` 集合的副本（锁内完成）。
2. 对每个根目录递归遍历（`recursive_directory_iterator`，`skip_permission_denied`）：
   - 命中目录名为 `.git` / `.github` / `.hub` 的子树跳过；
   - 只看文件名为 `SKILL.md` 的文件；
   - 用 `load_skill_from_dir(parent, root)` 解析 frontmatter 并构造 `SkillMetadata`；
   - 依次过滤：平台不匹配、配置里被禁用、名字已见过（first-wins）；
3. 按 `(category, name)` 升序排序。
4. 写回 `skills_`（锁内），记录日志。

### Registry 主要 API

| API | 用途 |
|-----|------|
| `list(category)` | 返回当前 skill 列表，可按 category 过滤 |
| `find(name_or_key)` | 按名字或 `command_key` 查找 |
| `read_skill_body(name)` | 读取 SKILL.md 去掉 frontmatter 后的正文 |
| `list_supporting_files(name)` | 列出 `references/` / `templates/` / `scripts/` / `assets/` 下相对路径 |
| `resolve_skill_file(name, rel)` | 把 `rel` 解析为 skill 目录下的绝对路径，拒绝 `..` 穿越 |
| `reload()` | 重新 `scan()`，语义与 `/skills reload` 对应 |

线程安全：所有读写都通过 `std::mutex` 保护；典型用法是 UI 线程启动时 `scan` 一次，之后 Agent worker 线程通过 `list` / `find` / `read_skill_body` 只读访问。

---

## 5. 激活路径 A — 用户输入 `/<skill-name>`

相关代码：`src/skills/skill_commands.{hpp,cpp}`、`src/skills/skill_activation.{hpp,cpp}`、`main.cpp` 对 `CommandRegistry` 的集成。

### 注册流程

`register_skill_commands_tracked(cmd_registry, skill_registry)` 在启动期被调用：

1. 拉取 `SkillRegistry::list()` 当前所有 skill。
2. 对每个 skill 构造一个 `SlashCommand`：
   - `name = meta.command_key`
   - `description = truncate(meta.description, 80)`
   - `execute` 闭包捕获 `skill_name` 和 `SkillRegistry&`，运行时再 `find` 一次（避免 skill 被 reload 后失效）。
3. 若 `/<key>` 与现有内置命令冲突（如 `/help`、`/clear`），记录警告并跳过注册。
4. 把新注册的 key 列表写入 `g_tracked_keys`（带 `std::mutex`），供 `/skills reload` 时清理。

### 用户触发时

用户在 TUI 输入 `/<skill-name> [args]` + 回车，事件链如下：

1. `CommandRegistry` 把命令 dispatch 到 skill 的 `execute` 闭包。
2. 闭包通过 `registry.read_skill_body()` 读取 SKILL.md 正文，`list_supporting_files()` 列出相关文件。
3. 调用 `build_activation_message()`（`skill_activation.cpp`）拼装一条用户角色的提示，结构：

   ```
   [SYSTEM: The user has invoked the "<name>" skill, indicating they want
   you to follow its instructions. The full skill content is loaded below.]

   <SKILL.md body>

   [This skill has supporting files you can load with the skill_view tool:]
   - references/api.md
   - ...
   To view any of these, use: skill_view(name="<name>", file_path="<path>")

   The user has provided the following instruction alongside the skill
   invocation: <args 部分若有>
   ```
4. 闭包把一条 system 消息（`"[Invoking skill: <name>]"`）追加进对话视图（让用户知道发生了什么），再：
   - 如果当前 agent 正在忙（`state.is_waiting`），把激活文本推入 `pending_queue`，等下一轮再投递；
   - 否则置位 `state.is_waiting = true` 并调用 `agent_loop.submit(message)` 作为新一轮输入送给 LLM。

这条激活消息的 `[SYSTEM: ...]` 开头与系统提示词里的"if you see a `[SYSTEM: The user has invoked …]` block, the skill has ALREADY been loaded"相呼应 — 模型不会重复调用 `skill_view`。

---

## 6. 激活路径 B — 模型调用工具

当模型自己判断某个请求需要 skill 时，走 `skills_list` + `skill_view` 两个工具（都注册为 read-only，`ToolExecutor::register_tool` 在 `main.cpp` 完成）。

### `skills_list`（`src/tool/skills_tool.cpp`）

- 入参：可选 `category`。
- 返回：`{success, skills: [{name, description, category?}], count, categories, message?, hint?}`。
- 仅返回**名字 + 描述 + 分类**，即 "tier-1" 元数据。保证模型低成本地全量扫一遍再决定要不要深入。

### `skill_view`（`src/tool/skill_view_tool.cpp`）

- 入参：`name`（必填）、`file_path`（可选，相对 skill 目录）。
- 不带 `file_path`：返回 SKILL.md 正文 + `linked_files`（支持文件列表），即 "tier-2" 加载。
- 带 `file_path`：返回该支持文件的内容，即 "tier-3" 按需加载。
- 安全：显式拒绝 `..`；`SkillRegistry::resolve_skill_file` 内部再用 `weakly_canonical` 校验路径必须落在 `skill_dir` 之下；文件大小超过 2 MB 返回错误，避免把巨型二进制灌入上下文。
- 错误分支带 `available_skills`（最多 20 个）或 `supporting_files` 提示，引导模型自我纠正。

---

## 7. 系统提示词中的 Skills 段

`build_system_prompt()`（`src/prompt/system_prompt.cpp`）在 skill 列表非空时插入一段 `# Skills`，核心约束：

- **BLOCKING REQUIREMENT** — 请求匹配某个 skill 时必须先加载再响应；
- **NEVER mention a skill by name without loading** — 避免"看到名字就脑补"；
- **不要对内建命令用 skill 工具** — `/help`、`/clear`、`/model` 等由 CommandRegistry 直接处理；
- 已经看到 `[SYSTEM: The user has invoked the "<name>" skill …]` 块时**不要再调** `skill_view`；
- 说明两条触发路径并统一到"先发现、再深入、再执行"的 progressive disclosure 顺序。

当 skill 列表为空时整段省略，避免误导模型去调用空工具。

---

## 8. `/skills reload` 流程

位于 `src/commands/builtin_commands.cpp`，对应 `reload_skill_commands(cmd_registry, skill_registry)`：

1. 取出 `g_tracked_keys` 并清空。
2. 对每个 key `cmd_registry.unregister_command(k)` — 反注册上一轮的 `/<skill-name>`。
3. `skill_registry.reload()` → 重新 `scan()`。
4. `register_skill_commands(...)` 按新 list 再注册一轮，并写回 `g_tracked_keys`。
5. 返回新的 skill 数量，由 built-in 命令反馈给用户。

注意：**新增 skill 的 `/<name>` 命令补全**依赖 `CommandRegistry` 的内容，`reload` 后即可在 slash 下拉里看到新命令（`src/tui/slash_dropdown.cpp` 每次输入变化都会重读 registry）。

被 `SkillRegistry` 过滤掉（比如平台不符 / `config.skills.disabled` 中 / 与内置命令冲突）的 skill 不会创建 slash 命令，但仍存在于 `SkillRegistry`，模型如果拿到名字仍可通过 `skill_view` 加载 —— 这是有意为之的：禁用只影响 TUI 触达，不影响模型能力。

---

## 9. 配置项

`~/.acecode/config.json` 的相关字段（`src/config/config.{hpp,cpp}`）：

```json
{
  "skills": {
    "disabled": ["some-skill-name"],
    "external_dirs": [
      "~/work/shared-skills",
      "${HOME}/Documents/acecode-skills"
    ]
  }
}
```

- `disabled`：按 `SkillMetadata.name` 精确匹配（不是 `command_key`）。
- `external_dirs`：直接作为扫描根追加，**不**自动拼 `.acecode/skills/` 后缀（与项目 walk 的"自动拼后缀"行为区分开来），便于指向任意目录结构。

---

## 10. 关键源文件对应表

| 文件 | 职责 |
|------|------|
| `src/skills/skill_metadata.hpp` | `SkillMetadata` 数据结构 |
| `src/skills/frontmatter.{hpp,cpp}` | 受限 YAML frontmatter 解析 |
| `src/skills/skill_loader.{hpp,cpp}` | 单个 `SKILL.md` 目录 → `SkillMetadata`；name 归一化；平台匹配 |
| `src/skills/skill_registry.{hpp,cpp}` | 多根递归扫描、去重、查询、支持文件定位 |
| `src/skills/skill_activation.{hpp,cpp}` | 用户 `/<name>` 触发时向 agent 投递的消息模板 |
| `src/skills/skill_commands.{hpp,cpp}` | 把每个 skill 注册/反注册为 slash 命令；`/skills reload` |
| `src/tool/skills_tool.{hpp,cpp}` | `skills_list` 工具（tier-1） |
| `src/tool/skill_view_tool.{hpp,cpp}` | `skill_view` 工具（tier-2/3，含路径安全校验） |
| `src/prompt/system_prompt.cpp` | 系统提示词 Skills 段 |
| `src/config/config.{hpp,cpp}` | `SkillsConfig`、`expand_path`、`get_project_dirs_up_to_home` |
| `main.cpp`（skill 段，行号随代码移动） | 组装扫描根、初始化 `SkillRegistry`、注册工具和 slash 命令 |

---

## 11. 完整数据流一图

```
启动
 ├─ main.cpp 组装 roots：
 │    项目 walk (cwd→…→<home以下>) / external_dirs / ~/.acecode/skills
 ├─ SkillRegistry.scan() → 读 frontmatter → 去重 → SkillMetadata[]
 ├─ register_skill_commands_tracked() → CommandRegistry 注册 /<key>
 ├─ ToolExecutor 注册 skills_list / skill_view
 └─ build_system_prompt() 附加 # Skills 段（非空时）

用户路径 A：
 用户输入 /<name> [args]
 └─ CommandRegistry dispatch → skill 闭包 execute
     ├─ registry.find + read_skill_body + list_supporting_files
     ├─ build_activation_message() 生成 [SYSTEM: …] 文本
     ├─ 追加 "[Invoking skill: <name>]" 到会话视图
     └─ agent_loop.submit() or pending_queue.push_back()

模型路径 B：
 模型 tool_call: skills_list
 └─ 返回 {name, description, category} 列表
     模型 tool_call: skill_view(name)  → 返回 SKILL.md 正文 + linked_files
     模型 tool_call: skill_view(name, file_path) → 返回支持文件内容

/skills reload：
 └─ 反注册旧 /<key> → SkillRegistry.reload() → 重新注册 /<key>
```

这张图可以作为调试 skill 行为时的路径定位参考：任何异常先判断发生在哪一步，再去对应的源文件。
