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
│   │   ├── markdown.js    markdown-it + highlight.js(12 lang)+ URL scheme 白名单
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
| `Message` | user 气泡 / assistant 头像+markdown / system 灰条;hover 浮出复制 + 分叉 actions |
| `ToolBlock` | 工具调用三态(进度 / summary chip / 失败折叠);file_edit/file_write 走 diff2html 渲染 hunks |
| `SidePanel` | 单会话视图右侧 280px 工作区面板,文件 / 变更 / 预览 三 tab。文件 tab lazy 拉 `/api/files`,预览 tab 拉 `/api/files/content` + hljs 高亮,变更 tab 前端聚合 messages.hunks(只覆盖 file_edit/file_write,bash sed 抓不到) |
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

## 依赖(运行时)

`pnpm add` 进 `package.json` `dependencies` 的:

| 包 | 版本 | 用途 |
|---|---|---|
| `react` / `react-dom` | ^18.3.1 | UI |
| `markdown-it` | ^14 | GFM markdown 渲染(表格/任务清单/嵌套 list) |
| `markdown-it-task-lists` | ^2 | task list 渲染 plugin |
| `highlight.js` | ^11 | 代码高亮(core + 12 种语言:c/cpp/js/ts/python/bash/json/diff/markdown/rust/go/yaml) |
| `diff2html` | ^3.4 | file_edit / file_write 工具的 hunks → 着色 diff |

bundle 体积 ~461KB(gzip ~156KB)。嵌入二进制约 +600KB(hex 编码 2x)。

## 历史

本项目 v1 是原生 ES Modules + Custom Elements + Bootstrap,单点 11 个 `<ace-*>` 元素 + 手写 `style.css`。重构 PR 把全部前端换成 React + Vite + Tailwind v4,保留 `lib/api.js` / `lib/auth.js` / `lib/connection.js` 的协议层不变,daemon 端 0 改动。后续 `enhance-webui-chat-rendering` 把 `lib/markdown.js` 从手写极简版升级到 markdown-it 管线,Message 加 hover copy/fork actions,ToolBlock 引入 diff2html 渲染 hunks。再后 `add-webui-side-panel` 在单会话视图右侧加固定 280px 的 SidePanel(文件 / 变更 / 预览 三 tab),后端新增 `GET /api/files` + `GET /api/files/content` 两个端点(`src/web/handlers/files_handler.{hpp,cpp}` 纯函数 + 17 个 unit test),前端抽 `lib/diff.js` / `lib/lang.js` / `lib/sessionChanges.js` 三个共用模块。
