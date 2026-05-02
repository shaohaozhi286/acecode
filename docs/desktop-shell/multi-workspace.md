# Desktop multi-workspace 备忘

> 配套 change: `openspec/changes/add-desktop-multi-workspace/`

## 关键模型

```
acecode-desktop.exe (父)
  ├── WebHost (webview, 主窗口)
  ├── WorkspaceRegistry  ← .acecode/projects/<hash>/workspace.json
  └── DaemonPool
       ├── Slot[hashA]  ── DaemonSupervisor → acecode.exe daemon (cwdA, portA, tokenA)
       ├── Slot[hashB]  ── DaemonSupervisor → acecode.exe daemon (cwdB, portB, tokenB)
       └── ...                                  Job Object 各自一个
```

每个 workspace 对应一个独立 daemon 子进程,daemon 自身代码完全不动 — 仍用
`current_path()` 做 cwd,session 落到 `.acecode/projects/<cwd_hash>/`。

## 启动顺序

1. desktop 扫 `.acecode/projects/*` → `WorkspaceRegistry`
2. 读 `state.json::last_active_workspace_hash`
3. `pick_active(last_active, current_path, registry)` → active hash
4. registry 空 + 进程 cwd 非空 → 自动 `register_new(cwd)`(首次安装兜底)
5. 注册 4 个 webview bridge:`aceDesktop_listWorkspaces / activateWorkspace / renameWorkspace / addWorkspace`
6. `pool.activate(active_hash)` → 拿到 `{port, token}` → `host.navigate("http://127.0.0.1:port/?token=...")`
7. 主窗口 `host.run()`

## 切换 workspace

前端 `ace-sidebar.js` 处理点击:
1. 调 `aceDesktop_activateWorkspace(hash)` → desktop 端 lazy spawn 该 workspace 的 daemon
2. 拿到 `{port, token}` 后 **整页 navigate** 到新 daemon 的 URL
3. WebView 重载 → 新 origin 下 ace-app 重新初始化 → sidebar 拉一遍 listWorkspaces

> **为什么不无重载切**(对 design D6 的偏离): 切到不同 workspace = 切到不同
> loopback 端口 = 不同 origin。浏览器 CORS 阻止跨 origin fetch。要让原页面的
> connection / api 模块直接连新端口需要 daemon 端给 loopback 互通发 CORS 头,
> 或者前端走 desktop bridge 中转所有请求。MVP 选最简单的整页 navigate,代价是
> 滚动位置 / 弹窗状态丢失,但符合 spec 的"input box 清空 / 消息列表替换"要求。
> 后续优化方向:daemon 加 `Access-Control-Allow-Origin: http://127.0.0.1:*` 让
> sibling daemon 互通;或前端切走 desktop 中转。

## 添加 workspace

`+` 按钮 → `aceDesktop_addWorkspace()`:
1. desktop 端 `pick_folder` 弹 `IFileOpenDialog`(`FOS_PICKFOLDERS`)
2. 用户选目录 → `registry.register_new(cwd)` 写 `workspace.json`
3. 返回 `{hash, cwd, name}`
4. sidebar 自动调 activate 切到新 workspace(整页 navigate)

## 行内重命名

铅笔按钮 → input 替换 name 文本 → Enter / blur 提交 → `aceDesktop_renameWorkspace(hash, name)` → `set_name` 原子写盘 → sidebar 显示新名。Esc 取消复原。空名 / 未知 hash / 写盘失败均拒绝。

## 退出

- WebView 窗口关闭 → `host.run()` 返回
- 写 `last_active_workspace_hash` 到 `state.json`
- `pool.stop_all()` 逐个 `TerminateJobObject` → daemon 子进程死亡
- `Job Object KILL_ON_JOB_CLOSE` 是兜底:即使 desktop 进程被外部 kill,daemon 也会跟着死(参见 `add-desktop-shell` 的 design)

## 关键文件

| 路径 | 作用 |
|---|---|
| `src/desktop/workspace_registry.{hpp,cpp}` | 扫盘 / 读写 `workspace.json` / 默认命名 |
| `src/desktop/daemon_pool.{hpp,cpp}` | N daemon 进程池,per-key 串行化 |
| `src/desktop/daemon_supervisor.{hpp,cpp}` | 单 daemon 子进程托管(spawn / probe / stop / Job Object)。提了 `IDaemonSupervisor` 虚基类便于单测 mock |
| `src/desktop/folder_picker_win.cpp` | `IFileOpenDialog` 包装 |
| `src/desktop/pick_active.{hpp,cpp}` | 启动时挑哪个 workspace 当 active 的纯函数 |
| `src/desktop/web_host.{hpp,cpp}` | webview/webview wrapper,暴露 `bind` / `eval` / `native_window` |
| `src/desktop/main.cpp` | wWinMain 入口,串起所有 |
| `src/utils/cwd_hash.{hpp,cpp}` | desktop 与 SessionStorage 共享的 hash 算法(FNV-1a 64bit) |
| `web/components/ace-sidebar.js` | workspace 分组渲染 + bridge 调用 |
| `web/connection.js` / `web/api.js` | 加 `reconfigure({port, token})` / `setBase`(MVP 走整页 navigate 不调用,留接口给后续) |

## 后续 issue

参见 `tasks.md` 第 13 节:
- 闲置 daemon LRU 回收
- 非活跃 workspace 的 session 列表展示
- 历史 hash 目录 backfill `workspace.json`
- 折叠状态持久化
- 跨 workspace 全局搜索
- 设置面板 workspace 管理
- POSIX folder picker
- 跨端口 CORS 协商,实现"无重载切 workspace"
