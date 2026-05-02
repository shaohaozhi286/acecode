# ACECode Web UI

`acecode daemon` 前端 — React + Vite + Tailwind v4。

构建产物 `web/dist/` 由 cmake 通过 [`cmake/acecode_embed_assets.cmake`](../cmake/acecode_embed_assets.cmake) 嵌入到 `acecode_testable` / `acecode` 二进制,daemon 启动后自带前端、无外部 CDN 依赖。

## 开发流程

```bash
cd web
pnpm install         # 一次性安装依赖(也可用 npm / bun)
pnpm dev             # 起 Vite dev server,默认 http://localhost:5173
                     # /api 与 /ws 自动代理到 127.0.0.1:28080(本机 daemon)
```

需要先在另一个终端跑 `acecode daemon --foreground` 让 API 可用。

## 生产构建

```bash
pnpm build           # 产出 web/dist/(被 .gitignore 忽略,需重新 build 才会出现)
```

产物结构:

```
web/dist/
├── index.html
└── assets/
    ├── index-XXXX.js     (~200 KB / gzip ~63 KB)
    └── index-XXXX.css    (~31 KB / gzip ~7 KB)
```

cmake configure 时会扫 `web/dist/` 并把这些文件 hex 编码写到 `${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp`。**`web/dist/` 不存在时 cmake 会发警告并嵌入空 map(daemon 仍可启动,但 SPA 返回 404)** — 第一次 build 前必须 `pnpm build` 一次。

CI / release 流程在 cmake 之前先跑 `pnpm install && pnpm build`。

## 目录结构

```
web/
├── package.json           Vite + React + Tailwind v4 依赖
├── vite.config.js         Vite 配置(含 dev proxy 到 daemon)
├── index.html             Vite entry,`__VERSION__` 由 cmake 替换为 git short hash
├── src/
│   ├── main.jsx           React 入口
│   ├── App.jsx            顶层壳:鉴权 gate + 主壳 + 弹框层
│   ├── theme.jsx          ThemeContext(light / dark + persist + system fallback)
│   ├── lib/
│   │   ├── api.js         HTTP 客户端(沿用与 daemon API 协议)
│   │   ├── auth.js        token 持久化(URL ?token= → sessionStorage)
│   │   ├── connection.js  WebSocket + 指数退避重连 + last_seq 持久化
│   │   ├── markdown.js    极简 markdown → safe HTML(不依赖外部库)
│   │   └── format.js      relativeTime / formatBytes / clsx 工具
│   ├── components/        18 个 React 组件(详见下方)
│   └── styles/globals.css Tailwind v4 入口 + CSS 变量驱动主题
└── dist/                  (generated, gitignored)
```

## 组件

| 组件 | 作用 |
|---|---|
| `App.jsx` | 鉴权 gate + 视图切换 + 弹框层 |
| `TopBar` | logo + 快捷工具(新对话/搜索) + 中央 single/grid4/grid9 pill + 主题切换 + 设置 |
| `Sidebar` | workspace 分组 + session 列表 + 底部 Skills/MCP tab |
| `ChatView` | 主聊天区(头部 + 消息流 + InputBar + StatusBar)+ 空态欢迎屏 |
| `Message` | user 气泡 / assistant 头像+markdown / system 灰条 |
| `ToolBlock` | 工具调用三态(进度 / summary chip / 失败折叠) |
| `InputBar` | 自动撑高 textarea + Enter 发 + ↑/↓ 历史 + 提交按钮 + abort |
| `StatusBar` | 权限模式下拉 + 模型 tag + 轮次 + 分支 |
| `ModelPicker` | `/api/models` 下拉 + 切换 |
| `Modal` / `SlideOver` / `Toggle` | 共享 modal / 抽屉 / 开关 |
| `PermissionModal` | 工具权限请求 |
| `QuestionModal` | AskUserQuestion(多选/单选 + custom_text) |
| `SkillsPanel` | 右侧抽屉:skill 列表 + toggle + 查看正文 |
| `MCPPanel` | 右侧抽屉:JSON 编辑器 + 校验 + 保存 |
| `SettingsPage` | 全屏左右两栏:常规(权限/轮次/daemon)+ 外观(主题选卡) |
| `Grid4View` / `Grid9View` | 宫格视图 |
| `MiniSession` | 宫格里的迷你会话卡 |
| `ExpandedOverlay` | 点宫格展开为完整 ChatView |
| `TokenPrompt` | 远程访问 / loopback token 鉴权页 |
| `Toast` / `Toaster` | 全局 toast 系统 |

## 主题系统

Tailwind v4 + CSS 变量。`<html data-theme="light|dark">` 切主题,变量值在 `globals.css` 里定义,Tailwind 类名(`bg-bg` / `text-fg` / `bg-accent` / `border-border` / `bg-ok-bg` 等)通过 `@theme inline` 引用变量 — 切主题不重新生成 class,只换 var 值。

颜色变量:`bg / surface / surface-alt / surface-hi / border / border-soft / fg / fg-2 / fg-mute / accent / accent-bg / accent-soft / ok / ok-bg / ok-border / warn / warn-bg / danger / danger-bg / code-bg / code-fg / code-line`。

新增颜色:在 `globals.css` 的 `@theme inline` + `:root` + `[data-theme="dark"]` 三处都加。

## 协议

后端 API 协议见 [`docs/daemon-api.md`](../docs/daemon-api.md);WS envelope 见 `src/web/`(C++ daemon 实现)。

## 历史

本项目 v1 是原生 ES Modules + Custom Elements + Bootstrap,单点 11 个 `<ace-*>` 元素 + 手写 `style.css`。重构 PR 把全部前端换成 React + Vite + Tailwind v4,保留 `lib/api.js` / `lib/auth.js` / `lib/connection.js` 的协议层不变,daemon 端 0 改动。
