# Skill 休眠(Dormancy)管理 — 设计文档

> 2026-08-18 | 基于 brainstorming 需求澄清结论
>
> 一句话:技能长期未用后,从系统提示词的自动候选列表中隐藏(休眠),但手动调用/path 匹配/显式提及全部保留,调用后自动唤醒。

---

## 1. 背景与问题

ACECode 的技能库随使用增长,每个技能的名称和描述都会注入系统提示词的 `<available_skills>` 列表。很多技能长期未用,它们不删除也不禁用,只是"暂时用不上"——却在列表中占着上下文,干扰模型注意力。

**目标**:引入一个低风险、非破坏性的"休眠"机制,让闲置技能不再出现在自动推荐列表里,但保留所有主动使用路径,被调用后自动恢复。

---

## 2. 三个状态

| 状态 | 进入自动列表? | 模型/用户能否使用? | 说明 |
|------|:---:|:---:|------|
| `active`(活跃) | ✅ 是 | ✅ 完全可用 | 正常状态 |
| `dormant`(休眠) | ❌ 否 | ✅ 可用——手动 `/skill`、显式提及名、path 匹配全部正常 | 从列表里"藏起来",不阻碍任何真实调用 |
| `disabled`(禁用) | ❌ 否 | ❌ 不可用 | 现有功能,彻底关闭 |

**核心区别**:休眠只做"眼不见"(省 context),不做"手不能"。被调用后自动恢复活跃。

---

## 3. 用户感受

### 日常使用
- 正常使用技能,系统自动记录"最后使用时间"
- 超过 30 天(可配置)未用的技能,下次新会话时不再出现在自动候选列表里
- 手动 `/技能名` 调用照常执行,同时自动恢复活跃,回到列表
- path 匹配激活、显式提及名全部保留生效

### 查看和管理
- `/skills` 面板或 Web 设置页展示每个技能的:**使用次数、最后使用时间、当前状态(活跃/休眠)**
- **驻留(pin)**:把重要技能标记为"驻留",永不休眠——即使半年不用也在列表里
- **解除驻留**:恢复自动判定

### 配置
- `skills.idleDays`(默认 30):超过此天数未用视为休眠
- 设为 0:关闭休眠功能,所有技能永远活跃

---

## 4. 架构

```
触发路径(inject_explicit_skill_instructions / path 激活 / 模型自动)
  └─ injection 成功后 → record_skill_usage(name)

     ┌─────────────────────────────────────────────┐
     │  skill_usage_store  (新模块 src/skills/)      │
     │  ~/.acecode/.skill_usage_state.json          │
     │  { version:1, skills{ name: {lastUsedAt,     │
     │    useCount, pinned} } }                     │
     │  进程内 mutex + 原子写(临时文件 + rename)       │
     └──────────────┬──────────────────────────────┘
                    │
   ┌────────────────┼───────────────────┐
   ▼                ▼                   ▼
自动列表过滤       TUI 展示            Web 展示
(构建时实时判定)   /skills 面板         settings 页
active→注入       次数/最后使用/状态    次数/状态/pin
dormant→跳过      + pin 操作           + 唤醒操作
```

---

## 5. 数据模型

```jsonc
// ~/.acecode/.skill_usage_state.json
{
  "version": 1,
  "skills": {
    "pdf":  { "lastUsedAt": "2026-08-01T10:00:00Z", "useCount": 12, "pinned": false },
    "xlsx": { "lastUsedAt": "2026-05-20T09:00:00Z", "useCount": 3,  "pinned": false }
  }
}
```

- `lastUsedAt`:最近一次注入时间(ISO8601),首次使用自动初始化
- `useCount`:累计注入次数
- `pinned`:是否驻留(永不休眠)
- **dormant 不落盘为持久标记**——读取时实时判定:`now - lastUsedAt > idleDays && !pinned` → 视为休眠

### 配置项

**`skills.idleDays`**(新,默认 30):
- 整数,>=0
- 0 = 关闭休眠功能
- 进现有 config schema + TUI/Web 设置项

---

## 6. 记录链路

### 触发点

所有 skill 注入路径收敛到 `record_skill_usage(name)`:

| 触发点 | 文件 | 说明 |
|--------|------|------|
| 显式注入 | `agent_loop.cpp`:`inject_explicit_skill_instructions` | 用户 `/skill` 或模型提及名 |
| path 激活 | skill activation 匹配处 | 工具触碰匹配路径,自动激活 |
| 模型自动 | 主循环注入路径 | 模型从列表中选择调用 |

### record_skill_usage 行为

1. 进程内 mutex 锁住 store
2. 读状态文件(带大小上限,防坏文件)
3. 记录存在 → `useCount++`、`lastUsedAt = now`;不存在 → 新建(`useCount = 1`)
4. 原子写(临时文件 + rename,0600 权限)
5. 失败不抛异常——best-effort,不阻断注入主流程

**计数口径**:注入即计数。同会话重复注入每次都计。只计成功注入(失败不注入→不计)。

---

## 7. 休眠过滤

### 判定公式

```
is_dormant(skill) = !pinned && (now - lastUsedAt > idleDays)
```

### 过滤位置

`build_skills_index_context_prompt`(构建系统提示词自动列表处):

```
遍历 skill 列表:
  if is_dormant(skill) → 跳过,不注入自动列表
  else → 正常注入
```

### 不受影响

- 用户手动 `/skill-name` — 执行正常,同时刷新 lastUsedAt
- 显式提及名 — 注入正常
- path 匹配激活 — 激活正常,注入正常
- `/skills` 面板 — 所有 skill 可见(含休眠,标注状态)
- `skill_view` — 不受过滤

---

## 8. 边界

| 场景 | 行为 |
|------|------|
| 新 skill 首次使用 | 自动建记录,`lastUsedAt` = 首次注入时间,`useCount` = 1,默认 active |
| skill 被卸载 | 状态记录保留,下次构建列表时自动跳过(文件不存在) |
| 状态文件损坏/丢失 | 全部 skill 视为无历史,下一轮从头累积,不影响功能 |
| 用户 pin 一个休眠 skill | 立即生效,下次构建列表时恢复出现 |
| 用户手动调用休眠 skill | 执行正常,`lastUsedAt` 刷新,下次会话自动恢复 active |
| daemon 和 TUI 同时写 | 原子写(临时文件+rename),后写者覆盖,不产生破损文件 |
| `idleDays = 0` | 关闭休眠,全部 active |
| 无 skill 的系统 | 状态文件为空/不存在,构建列表时无过滤,行为不变 |

---

## 9. 实现路径

| # | 任务 | 涉及文件 | 说明 |
|---|------|----------|------|
| 1 | 新增 store | `src/skills/skill_usage_store.{hpp,cpp}` | 读/写状态文件,原子 rename,进程内 mutex |
| 2 | 记录钩子 | `src/agent_loop.cpp` + `src/skills/skill_activation.cpp` | `record_skill_usage(name)` 插入注入成功处 |
| 3 | 列表过滤 | `src/agent_loop.cpp`(`build_skills_index_context_prompt`) | 构建自动列表时按 `is_dormant` 跳过 |
| 4 | 配置项 | `src/config/` | 新增 `skills.idleDays`(默认 30) |
| 5 | 状态字段 | `src/tui_state.hpp` | 新增 `skill_usage_store` 引用 |
| 6 | TUI 展示 | `src/tui/`(settings 或 /skills 面板) | 次数/最后使用/状态/pin 操作 |
| 7 | Web 展示 | `src/web/`(开辟现有 API 或新增) | skill 状态 + pin/唤醒 |
| 8 | 测试 | `tests/skills/skill_usage_store_test.cpp` | 读/写/并发/边界/判定逻辑 |

---

## 10. 测试清单

- [ ] 新建 skill → 自动创建记录,初始 active
- [ ] 注入后刷新 `lastUsedAt`,`useCount` 递增
- [ ] 超阈值未用 → `is_dormant` = true,不进自动列表
- [ ] pinned 的 skill 即使超阈值也保持 active
- [ ] 手动调用休眠 skill 后恢复 active
- [ ] 状态文件损坏时优雅降级,不影响功能
- [ ] 原子写并发安全
- [ ] `idleDays = 0` 关闭休眠