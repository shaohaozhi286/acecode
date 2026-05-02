# Desktop multi-workspace 备忘

> 配套 change: `openspec/changes/add-desktop-multi-workspace/`、`openspec/changes/share-daemon-across-workspaces/`

## 关键模型

```
acecode-desktop.exe (父)
  ├── WebHost (webview, 主窗口)
  ├── WorkspaceRegistry  ← .acecode/projects/<hash>/workspace.json
  └── DaemonPool
    └── Slot[__shared_daemon__/default]
      └── DaemonSupervisor → acecode.exe daemon (shared port/token)
```

所有 workspace / session / conversation 共享同一个 daemon 子进程。workspace
身份通过 HTTP API 和 `SessionOptions{cwd, workspace_hash}` 传入 session 层,
每个 session 的 `AgentLoop`、`SessionManager`、tool context 都使用自身 cwd。
磁盘布局仍然是 `.acecode/projects/<cwd_hash>/`,兼容旧 session。

## 启动顺序

1. desktop 扫 `.acecode/projects/*` → `WorkspaceRegistry`
2. 读 `state.json::last_active_workspace_hash`
3. `pick_active(last_active, current_path, registry)` → active hash
4. registry 空 + 进程 cwd 非空 → 自动 `register_new(cwd)`(首次安装兜底)
5. 注册 4 个 webview bridge:`aceDesktop_listWorkspaces / activateWorkspace / renameWorkspace / addWorkspace`
6. `pool.activate(__shared_daemon__/default)` → 拿到 `{port, token}` → `host.navigate("http://127.0.0.1:port/?token=...")`
7. 主窗口 `host.run()`

## 切换 workspace

前端 Sidebar 优先调用共享 daemon:
1. `GET /api/workspaces` 拉 workspace 列表。
2. `GET /api/workspaces/:hash/sessions` 按 workspace 拉 session。
3. 点击 workspace 只更新前端 active workspace,不切端口、不派生新 daemon。
4. 恢复 inactive conversation 时调用 `POST /api/workspaces/:hash/sessions/:id/resume`。

`aceDesktop_activateWorkspace(hash)` 仍保留为 bridge fallback:它只确保共享
daemon slot 已启动,并向 daemon `POST /api/workspaces` 注册 cwd,返回同一组
`{port, token}`。

## 添加 workspace

`+` 按钮 → `aceDesktop_addWorkspace()`:
1. desktop 端 `pick_folder` 弹 `IFileOpenDialog`(`FOS_PICKFOLDERS`)
2. 用户选目录 → `registry.register_new(cwd)` 写 `workspace.json`
3. 返回 `{hash, cwd, name}`
4. sidebar 调共享 daemon `POST /api/workspaces` 注册 cwd,然后把该 hash 设为 active

## 行内重命名

铅笔按钮 → input 替换 name 文本 → Enter / blur 提交 → `aceDesktop_renameWorkspace(hash, name)` → `set_name` 原子写盘 → sidebar 显示新名。Esc 取消复原。空名 / 未知 hash / 写盘失败均拒绝。

## 退出

- WebView 窗口关闭 → `host.run()` 返回
- 写 `last_active_workspace_hash` 到 `state.json`
- `pool.stop_all()` 停掉共享 daemon slot
- `Job Object KILL_ON_JOB_CLOSE` 是兜底:即使 desktop 进程被外部 kill,daemon 也会跟着死(参见 `add-desktop-shell` 的 design)

## 关键文件

| 路径 | 作用 |
|---|---|
| `src/desktop/workspace_registry.{hpp,cpp}` | 扫盘 / 读写 `workspace.json` / 默认命名 |
| `src/desktop/daemon_pool.{hpp,cpp}` | 通用进程池;Desktop 当前只使用 `__shared_daemon__/default` slot |
| `src/desktop/daemon_supervisor.{hpp,cpp}` | 单 daemon 子进程托管(spawn / probe / stop / Job Object)。提了 `IDaemonSupervisor` 虚基类便于单测 mock |
| `src/desktop/folder_picker_win.cpp` | `IFileOpenDialog` 包装 |
| `src/desktop/pick_active.{hpp,cpp}` | 启动时挑哪个 workspace 当 active 的纯函数 |
| `src/desktop/web_host.{hpp,cpp}` | webview/webview wrapper,暴露 `bind` / `eval` / `native_window` |
| `src/desktop/main.cpp` | wWinMain 入口,串起所有 |
| `src/utils/cwd_hash.{hpp,cpp}` | desktop 与 SessionStorage 共享的 hash 算法(FNV-1a 64bit) |
| `src/session/session_registry.{hpp,cpp}` | workspace-aware session create/resume/list |
| `src/web/server.{hpp,cpp}` | `/api/workspaces` 与 workspace-scoped session routes |
| `web/src/components/Sidebar.jsx` | workspace 分组渲染 + workspace-scoped session 选择 |
| `web/src/lib/api.js` | workspace-scoped list/create/resume helper |

## 后续 issue

参见 `tasks.md` 第 13 节:
- shared daemon 内的 per-workspace provider/model override
- shared daemon 旧 per-workspace run 目录诊断/清理
- 历史 hash 目录 backfill `workspace.json`
- 折叠状态持久化
- 跨 workspace 全局搜索
- 设置面板 workspace 管理
- POSIX folder picker
