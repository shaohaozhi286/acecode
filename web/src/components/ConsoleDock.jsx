// 控制台停靠区(openspec/changes/add-console-dock,specs/console-dock-ui)。
//
// 结构:顶边拖拽手柄 + tab 栏(新建/逐个关闭/整体收起) + 每 tab 一个 xterm
// 容器(display 切换)。xterm/WS/重连等命令式资源全放 ref Map,不进 React
// state — 折叠只把高度归 0,实例与连接全部保活,重开即续(SidePanel 同模式)。
//
// 协议(lib/consoleDock.js::parsePtyFrame):服务端 binary 帧首字节 0x00 为
// JSON 控制帧({cursor}/{exit_code}),其余为原始 VT 字节直喂 xterm;键入
// 字节原样 ws.send。断线(非 1000)按 nextReconnectDelay 退避,携带本地
// cursor 续传,刷新页面后 listPty 恢复 running 会话并从 cursor=0 回放。

import { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';

import { createApi } from '../lib/api.js';
import { getToken } from '../lib/auth.js';
import {
  CONSOLE_DOCK_MIN_HEIGHT,
  activateTab,
  addTab,
  clampDockHeight,
  createDockTabs,
  markTabExited,
  nextReconnectDelay,
  parsePtyFrame,
  ptyWsUrl,
  removeTab,
  renameTab,
} from '../lib/consoleDock.js';
import { formatDroppedPaths, parseUriList } from '../lib/consoleDropPaths.js';
import { copyTextToSystemClipboard, readTextFromSystemClipboard } from '../lib/systemClipboard.js';
import { normalizeShells, buildShellMenuItems } from '../lib/consoleShells.js';
import { useTheme } from '../theme.jsx';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';

const api = createApi();

// Linux 字体必须显式列出:中文 locale 的 fontconfig 常把通用 monospace 映射到
// CJK 字体(文泉驿 / Noto CJK),拉丁字形变宽 + xterm.js 网格测量错位,光标
// 漂移到没法打字(VS Code 同理在 Linux 上默认 'Droid Sans Mono' 而非裸 monospace)。
const TERMINAL_FONT =
  'ui-monospace, "Cascadia Mono", Consolas, "SF Mono", Menlo, ' +
  '"DejaVu Sans Mono", "Ubuntu Mono", "Liberation Mono", "Noto Sans Mono", ' +
  '"Droid Sans Mono", monospace';

const THEMES = {
  dark: {
    background: '#181818', foreground: '#cccccc',
    cursor: '#cccccc', selectionBackground: 'rgba(204,204,204,0.25)',
  },
  light: {
    background: '#fbfbfb', foreground: '#3b3b3b',
    cursor: '#3b3b3b', selectionBackground: 'rgba(59,59,59,0.18)',
  },
};

// 宿主 OS:desktop 壳由 init_script 注入 window.__ACECODE_OS__;浏览器直连兜底
// 从 userAgent 推断(直连模式拖放拿不到完整路径,功能基本只对 desktop 有意义)。
function detectHostOs() {
  if (typeof window !== 'undefined') {
    const os = window.__ACECODE_OS__;
    if (os === 'windows' || os === 'macos' || os === 'linux') return os;
  }
  const ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '';
  if (/Windows/i.test(ua)) return 'windows';
  if (/Mac/i.test(ua)) return 'macos';
  return 'linux';
}

// 是否由 native 接管系统文件拖放(Windows/WebView2 + macOS/WKWebView)。
// 为真:终端区不 preventDefault,让系统拖放下沉到 native 层后回传路径;
// 为假(Linux/WebKitGTK):走 web drop 直接读 text/uri-list。
function nativeFileDropEnabled() {
  return typeof window !== 'undefined' && window.__ACECODE_NATIVE_FILE_DROP__ === true;
}

// dataTransfer 是否携带文件(区分纯文本拖放,避免对选中文本等误响应)。
function transferHasFiles(dataTransfer) {
  if (!dataTransfer) return false;
  try {
    const types = dataTransfer.types;
    return !!types && Array.from(types).includes('Files');
  } catch {
    return false;
  }
}

const HOST_OS = detectHostOs();
const NATIVE_DROP = nativeFileDropEnabled();

export function ConsoleDock({ open, height, onHeightChange, onToggle, consoleInfo }) {
  const { theme: mode } = useTheme();
  const [tabsState, setTabsState] = useState(createDockTabs);
  const [creating, setCreating] = useState(false);
  // + 旁 shell 下拉框(控制台 Shell 选择器):可用 shell 列表 / 默认 id / 菜单开合 /
  // 「指定 bash 路径」模态状态。
  const [shells, setShells] = useState([]);
  const [defaultShellId, setDefaultShellId] = useState('');
  const [shellMenuOpen, setShellMenuOpen] = useState(false);
  const [shellMenuPos, setShellMenuPos] = useState(null); // {top,left}:fixed 定位坐标
  const shellGroupRef = useRef(null);
  const [bashPrompt, setBashPrompt] = useState(null); // null 或 {} 表示模态打开
  const [bashPathInput, setBashPathInput] = useState('');
  const [bashError, setBashError] = useState('');
  const entriesRef = useRef(new Map()); // id → {term, fit, ws, container, cursor, tries, timers, disposed}
  const bodyRef = useRef(null);
  const tabsStateRef = useRef(tabsState);
  const restoredRef = useRef(false);
  useEffect(() => { tabsStateRef.current = tabsState; }, [tabsState]);

  const backend = consoleInfo?.backend || '';

  // ── xterm 实例生命周期 ─────────────────────────────────────────────

  const focusTab = useCallback((id) => {
    const entry = entriesRef.current.get(id);
    const t = entry?.term;
    if (!t) return;
    t.focus();
    // xterm 的可输入元素是隐藏 textarea;rAF 等 open/display 切换完成后再
    // 聚焦,避免 display:none 期间 focus 落空。
    requestAnimationFrame(() => { try { t.textarea?.focus(); } catch {} });
  }, []);

  const teardownEntry = useCallback((id) => {
    const entry = entriesRef.current.get(id);
    if (!entry) return;
    entry.disposed = true;
    if (entry.reconnectTimer) clearTimeout(entry.reconnectTimer);
    if (entry.resizeTimer) clearTimeout(entry.resizeTimer);
    if (entry.ws) {
      try { entry.ws.onclose = entry.ws.onmessage = null; entry.ws.close(1000); } catch {}
    }
    try { entry.term?.dispose(); } catch {}
    entriesRef.current.delete(id);
  }, []);

  const scheduleResize = useCallback((id, cols, rows) => {
    const entry = entriesRef.current.get(id);
    if (!entry || entry.disposed) return;
    if (entry.resizeTimer) clearTimeout(entry.resizeTimer);
    // 100ms 防抖(opencode 同款):拖拽中不打爆 resize 端点。
    entry.resizeTimer = setTimeout(() => {
      entry.resizeTimer = null;
      api.resizePty(id, cols, rows).catch(() => {});
    }, 100);
  }, []);

  const connectWs = useCallback((id) => {
    const entry = entriesRef.current.get(id);
    if (!entry || entry.disposed) return;
    const url = ptyWsUrl({
      id,
      cursor: entry.cursor,
      token: getToken() || '',
      pageProtocol: location.protocol,
      pageHost: location.host,
    });
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    entry.ws = ws;

    ws.onopen = () => {
      if (entry.disposed || entry.ws !== ws) return;
      entry.tries = 0;
      // 重连后把当前尺寸推给后端(连接断开期间可能拖过大小)。
      if (entry.term) scheduleResize(id, entry.term.cols, entry.term.rows);
    };
    ws.onmessage = (ev) => {
      if (entry.disposed || entry.ws !== ws) return;
      if (typeof ev.data === 'string') {
        entry.term?.write(ev.data);
        entry.cursor += ev.data.length;
        return;
      }
      const frame = parsePtyFrame(ev.data);
      if (frame.kind === 'data') {
        entry.term?.write(frame.bytes);
        entry.cursor += frame.bytes.length;
        return;
      }
      const payload = frame.payload;
      if (!payload) return;
      if (typeof payload.cursor === 'number') entry.cursor = payload.cursor;
      if (typeof payload.exit_code === 'number') {
        setTabsState((s) => markTabExited(s, id, payload.exit_code));
        if (entry.term) {
          entry.term.options.disableStdin = true;
          entry.term.write(
            `\r\n\x1b[90m[process exited with code ${payload.exit_code}]\x1b[0m\r\n`);
        }
      }
    };
    ws.onclose = (ev) => {
      if (entry.ws === ws) entry.ws = null;
      if (entry.disposed || ev.code === 1000) return;
      const tab = tabsStateRef.current.tabs.find((t) => t.id === id);
      if (!tab || tab.status === 'exited') return;
      entry.reconnectTimer = setTimeout(() => {
        entry.reconnectTimer = null;
        entry.tries += 1;
        connectWs(id);
      }, nextReconnectDelay(entry.tries));
    };
  }, [scheduleResize]);

  // tab 容器挂载回调:首次拿到 DOM 时创建 xterm 并连 WS。
  const mountContainer = useCallback((id, el) => {
    if (!el) return;
    let entry = entriesRef.current.get(id);
    if (entry && entry.container === el) return;
    if (entry) return; // 容器被 React 重建的极端情况:保持原实例(display 切换不触发)
    entry = {
      term: null, fit: null, ws: null, container: el,
      cursor: 0, tries: 0, reconnectTimer: null, resizeTimer: null,
      disposed: false,
    };
    entriesRef.current.set(id, entry);

    const term = new Terminal({
      fontSize: 13,
      fontFamily: TERMINAL_FONT,
      scrollback: 10000,
      cursorBlink: true,
      theme: THEMES[mode === 'dark' ? 'dark' : 'light'],
      convertEol: false,
    });
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(el);
    // Ctrl+` 必须放行冒泡到 window(useGlobalShortcut),否则终端聚焦时
    // toggle 失灵 — opencode 同款处理,最易踩的坑。
    term.attachCustomKeyEventHandler((ev) => {
      if (ev.ctrlKey && !ev.altKey && !ev.metaKey && ev.key === '`') return false;
      return true;
    });
    term.onData((data) => {
      const e = entriesRef.current.get(id);
      if (e?.ws && e.ws.readyState === WebSocket.OPEN) e.ws.send(data);
    });
    term.onResize(({ cols, rows }) => scheduleResize(id, cols, rows));
    // 终端内程序经 OSC 0/2 设置标题(cmd 的 title 命令 / TUI 应用) →
    // tab 标题跟随,并同步回 daemon(刷新恢复会话时标题不丢)。
    term.onTitleChange((title) => {
      setTabsState((s) => renameTab(s, id, title));
      const trimmed = String(title || '').trim();
      if (trimmed) api.setPtyTitle(id, trimmed).catch(() => {});
    });
    entry.term = term;
    entry.fit = fit;
    requestAnimationFrame(() => {
      try { fit.fit(); } catch {}
      // 新建 tab 挂载完成时它已是 activeId(addTab 切激活)→ 焦点直接进终端,
      // 用户敲键即生效,无需先点一下。
      if (tabsStateRef.current.activeId === id) focusTab(id);
    });

    connectWs(id);
  }, [connectWs, focusTab, mode, scheduleResize]);

  // 主题切换:更新所有存活实例的配色。
  useEffect(() => {
    const theme = THEMES[mode === 'dark' ? 'dark' : 'light'];
    for (const entry of entriesRef.current.values()) {
      if (entry.term) entry.term.options.theme = theme;
    }
  }, [mode]);

  // 高度变化 / 展开 / 切 tab:rAF 节流 refit 当前激活终端。
  useEffect(() => {
    if (!open) return;
    const raf = requestAnimationFrame(() => {
      const entry = entriesRef.current.get(tabsState.activeId);
      try { entry?.fit?.fit(); } catch {}
    });
    return () => cancelAnimationFrame(raf);
  }, [open, height, tabsState.activeId]);

  // 切 tab / 展开 dock:焦点跟到激活终端(不挂 height — 拖高时不抢焦点)。
  // 新建 tab 的首次聚焦由 mountContainer 处理(此刻 term 尚未 mount)。
  useEffect(() => {
    if (!open || !tabsState.activeId) return;
    focusTab(tabsState.activeId);
  }, [open, tabsState.activeId, focusTab]);

  // 组件卸载(理论上只在整页离开)回收全部资源。
  useEffect(() => () => {
    for (const id of [...entriesRef.current.keys()]) teardownEntry(id);
  }, [teardownEntry]);

  // ── 会话操作 ───────────────────────────────────────────────────────

  // 新建终端。shellId 省略 → 用默认 shell;指定 → 用该 shell。后端对 git-bash
  // 未指定路径返回 400 {needs_path} → 弹「指定 bash 路径」模态。
  const createTab = useCallback(async (shellId) => {
    if (creating) return;
    setCreating(true);
    try {
      const info = await api.createPty(shellId ? { shell: shellId } : {});
      setTabsState((s) => addTab(s, info));
    } catch (err) {
      if (err?.body?.needs_path || err?.body?.shell === 'git-bash') {
        setBashError('');
        setBashPathInput('');
        setBashPrompt({ shellId: 'git-bash' });
      } else {
        console.warn('[console] create pty failed', err);
      }
    } finally {
      setCreating(false);
    }
  }, [creating]);

  // 拉取当前 OS 可用 shell(dock 打开时一次)。
  const loadShells = useCallback(async () => {
    try {
      const { shells: ns, defaultId } = normalizeShells(await api.listPtyShells());
      setShells(ns);
      setDefaultShellId(defaultId);
    } catch { /* 列表拉取失败:下拉留空,+ 仍用默认 shell */ }
  }, []);

  // 开合下拉:打开时按按钮 rect 计算 fixed 坐标(向下弹,逃出 tabbar overflow 裁剪)。
  const toggleShellMenu = useCallback(() => {
    if (!shellMenuOpen && shellGroupRef.current) {
      const r = shellGroupRef.current.getBoundingClientRect();
      setShellMenuPos({ top: Math.round(r.bottom + 4), left: Math.round(r.left) });
    }
    setShellMenuOpen((v) => !v);
  }, [shellMenuOpen]);

  // 下拉选 shell:可用 → 新建 + 持久化为默认;需指定路径(git-bash)→ 弹模态。
  const pickShell = useCallback(async (item) => {
    setShellMenuOpen(false);
    if (!item) return;
    if (item.needsPath || !item.available) {
      setBashError('');
      setBashPathInput('');
      setBashPrompt({ shellId: item.id });
      return;
    }
    await createTab(item.id);
    try {
      const { shells: ns, defaultId } =
        normalizeShells(await api.setConsoleShellConfig({ default_shell: item.id }));
      setShells(ns);
      setDefaultShellId(defaultId);
    } catch { /* 默认持久化失败不致命:本次仍以该 shell 打开 */ }
  }, [createTab]);

  // 提交用户指定的 Git Bash 路径:后端校验 + 持久化 → 刷新列表 → 用 git-bash 新建。
  const submitBashPath = useCallback(async () => {
    const path = bashPathInput.trim();
    if (!path) { setBashError('请输入 bash.exe 完整路径'); return; }
    try {
      const { shells: ns, defaultId } = normalizeShells(
        await api.setConsoleShellConfig({ git_bash_path: path, default_shell: 'git-bash' }));
      setShells(ns);
      setDefaultShellId(defaultId);
      setBashPrompt(null);
      toast({ kind: 'ok', text: '已记住 Git Bash 路径' });
      await createTab('git-bash');
    } catch (err) {
      setBashError(err?.body?.error || err?.message || '保存失败');
    }
  }, [bashPathInput, createTab]);

  const closeTab = useCallback((id) => {
    teardownEntry(id);
    // 关掉最后一个 tab → 整个 dock 收起。副作用(onToggle = 父 setState)
    // 放在 setTabsState 更新器外,避免"在另一组件渲染期间 setState"告警。
    const willBeEmpty = tabsStateRef.current.tabs.filter((t) => t.id !== id).length === 0;
    setTabsState((s) => removeTab(s, id));
    api.deletePty(id).catch(() => {});
    if (willBeEmpty) {
      // 重置 restore 闸门:下次展开重新拉起一个新终端(否则展开后空面板)。
      restoredRef.current = false;
      onToggle(false);
    }
  }, [teardownEntry, onToggle]);

  // 首次展开:恢复 daemon 上仍存活的会话(页面刷新场景,cursor=0 全量回放),
  // 没有则自动建第一个 tab。
  useEffect(() => {
    if (!open || restoredRef.current) return;
    restoredRef.current = true;
    (async () => {
      try {
        const out = await api.listPty();
        const running = (out?.sessions || []).filter((s) => s.status === 'running');
        if (running.length > 0) {
          setTabsState((prev) => {
            let s = prev;
            for (const info of running) s = addTab(s, info);
            return s;
          });
          return;
        }
      } catch {}
      createTab();
    })();
  }, [open, createTab]);

  // dock 打开时拉取可用 shell 列表(+ 旁下拉框)。
  useEffect(() => {
    if (open) loadShells();
  }, [open, loadShells]);

  // shell 下拉菜单:点菜单外关闭。
  useEffect(() => {
    if (!shellMenuOpen) return undefined;
    const onDown = (e) => {
      // 菜单经 portal 渲染到 body(已脱离 .ace-console-newgroup),需同时放行菜单本身。
      if (e.target instanceof Element &&
          (e.target.closest('.ace-console-newgroup') || e.target.closest('.ace-console-shell-menu'))) {
        return;
      }
      setShellMenuOpen(false);
    };
    window.addEventListener('mousedown', onDown, true);
    return () => window.removeEventListener('mousedown', onDown, true);
  }, [shellMenuOpen]);

  // ── 顶边拖拽(startSidebarResize 模式改纵向,只动 dock 高度) ─────────

  const dragActiveRef = useRef(false);
  const startDrag = useCallback((event) => {
    if (event.button != null && event.button !== 0) return;
    if (dragActiveRef.current) return;
    dragActiveRef.current = true;
    event.preventDefault();
    const startY = event.clientY;
    const startHeight = height;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      const next = clampDockHeight(
        startHeight + (startY - moveEvent.clientY), window.innerHeight);
      onHeightChange(next);
    };
    const onStop = () => {
      dragActiveRef.current = false;
      document.body.classList.remove('ace-resizing');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onStop);
      window.removeEventListener('pointercancel', onStop);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onStop, { once: true });
    window.addEventListener('pointercancel', onStop, { once: true });
  }, [height, onHeightChange]);

  // ── 文件拖放注入(plan: 桌面控制台拖放文件 → 插入完整路径)───────────
  // 把拖入终端的文件完整路径作为输入注入对应终端(等同键入,不自动回车)。
  // Windows/macOS 由 native 拦截系统拖放后回传(NATIVE_DROP);Linux/WebKitGTK
  // 直接从 web drop 的 text/uri-list 拿。命中目标终端靠「最近悬停 tab + 时间戳」。

  const [dropHoverTabId, setDropHoverTabId] = useState(null);
  const dropHoverRef = useRef({ tabId: null, ts: 0 });

  const injectTextToTab = useCallback((tabId, text) => {
    if (!text) return false;
    const entry = entriesRef.current.get(tabId);
    if (entry?.ws && !entry.disposed && entry.ws.readyState === WebSocket.OPEN) {
      entry.ws.send(text);
      focusTab(tabId); // 注入后聚焦,用户可直接继续输入 / 回车执行
      return true;
    }
    return false;
  }, [focusTab]);

  const markDropHover = useCallback((tabId) => {
    dropHoverRef.current = { tabId, ts: Date.now() };
    setDropHoverTabId((cur) => (cur === tabId ? cur : tabId));
  }, []);

  const clearDropHover = useCallback((tabId) => {
    if (dropHoverRef.current.tabId === tabId) dropHoverRef.current = { tabId: null, ts: 0 };
    setDropHoverTabId((cur) => (cur === tabId ? null : cur));
  }, []);

  const handleTermDragEnter = useCallback((tabId, event) => {
    if (!transferHasFiles(event.dataTransfer)) return;
    // web 模式(Linux)必须 preventDefault 才会触发后续 drop;native 模式不拦,
    // 让系统拖放下沉到 WebView native 层去取真实路径。
    if (!NATIVE_DROP) event.preventDefault();
    markDropHover(tabId);
  }, [markDropHover]);

  const handleTermDragOver = useCallback((tabId, event) => {
    if (!transferHasFiles(event.dataTransfer)) return;
    if (!NATIVE_DROP) {
      event.preventDefault();
      try { event.dataTransfer.dropEffect = 'copy'; } catch { /* 某些环境只读 */ }
    }
    markDropHover(tabId);
  }, [markDropHover]);

  const handleTermDragLeave = useCallback((tabId, event) => {
    // 仅当真正离开容器(relatedTarget 不在容器内)才清,避免在 xterm 内部子元素
    // 之间移动时 enter/leave 抖动造成的误清。
    const to = event.relatedTarget;
    if (to && event.currentTarget.contains(to)) return;
    clearDropHover(tabId);
  }, [clearDropHover]);

  const handleTermDrop = useCallback((tabId, event) => {
    if (NATIVE_DROP) return; // native 模式下 drop 不会触发,路径走全局回调
    event.preventDefault();
    const uris = parseUriList(event.dataTransfer?.getData?.('text/uri-list') || '');
    clearDropHover(tabId);
    if (uris.length) injectTextToTab(tabId, formatDroppedPaths(uris, HOST_OS));
  }, [injectTextToTab, clearDropHover]);

  // VS Code 式终端右键(rightClickBehavior=copyPaste):有选区 → 复制并清选区;
  // 无选区 → 把剪贴板文本粘进该终端。不弹通用右键菜单(外层 DesktopContextMenu
  // 已对 .ace-console-term 放行)。粘贴走 term.paste —— 它按 bracketed-paste 模式
  // 正确包装后经 onData → ws.send,多行粘贴不会被 shell 逐行抢跑(同 xterm Ctrl+V)。
  const handleTermContextMenu = useCallback((tabId, event) => {
    event.preventDefault();
    event.stopPropagation();
    const term = entriesRef.current.get(tabId)?.term;
    if (!term) return;
    (async () => {
      try {
        if (typeof term.hasSelection === 'function' && term.hasSelection()) {
          const sel = term.getSelection?.() || '';
          if (sel) {
            await copyTextToSystemClipboard(sel);
            term.clearSelection?.();
            return;
          }
        }
        const result = await readTextFromSystemClipboard();
        if (result?.ok && result.text) {
          term.paste(result.text);
          focusTab(tabId);
        }
      } catch { /* 剪贴板读写失败静默忽略 */ }
    })();
  }, [focusTab]);

  // NATIVE_DROP:注册全局回调,native 拦到系统文件拖放后经 host.eval 调它回传路径。
  useEffect(() => {
    if (!NATIVE_DROP) return undefined;
    window.__aceConsoleAcceptFileDrop = (payload) => {
      let paths = payload;
      if (typeof payload === 'string') {
        try { paths = JSON.parse(payload); } catch { return; }
      }
      if (!Array.isArray(paths) || paths.length === 0) return;
      const hover = dropHoverRef.current;
      // 松手落在终端上 → 该 tab 仍是最近悬停且时间戳新鲜(回传通常几十 ms 内到)。
      // 否则(拖到非终端区)静默忽略,只让 native 吞掉文件导航即可。
      if (!hover.tabId || Date.now() - hover.ts > 1500) return;
      const targetId = hover.tabId;
      dropHoverRef.current = { tabId: null, ts: 0 };
      setDropHoverTabId(null);
      injectTextToTab(targetId, formatDroppedPaths(paths, HOST_OS));
    };
    return () => {
      try { delete window.__aceConsoleAcceptFileDrop; }
      catch { window.__aceConsoleAcceptFileDrop = undefined; }
    };
  }, [injectTextToTab]);

  // ── 渲染 ───────────────────────────────────────────────────────────

  return (
    <div
      className="ace-console-dock"
      data-collapsed={!open}
      style={{ height: open ? height : 0 }}
    >
      <div
        role="separator"
        aria-orientation="horizontal"
        aria-label="调整控制台高度"
        className="ace-console-resize-handle"
        onPointerDown={startDrag}
        title="拖动调整控制台高度"
      />
      <div className="ace-console-tabbar">
        <div className="flex items-center gap-1 min-w-0 overflow-x-auto">
          {tabsState.tabs.map((tab) => (
            <div
              key={tab.id}
              className="ace-console-tab"
              data-active={tab.id === tabsState.activeId}
              data-exited={tab.status === 'exited'}
              onClick={() => setTabsState((s) => activateTab(s, tab.id))}
            >
              <VsIcon name="terminal" size={13} className="shrink-0 opacity-70" />
              {/* overflow-hidden + whitespace-nowrap 不带 truncate:超界硬截断,
                  text-overflow 保持默认 clip(不要 … 省略号占版面)。 */}
              <span className="overflow-hidden whitespace-nowrap text-[12px]">
                {tab.title}
                {tab.status === 'exited' ? ' (已退出)' : ''}
              </span>
              <button
                type="button"
                className="ace-console-tab-close"
                title="关闭终端"
                onClick={(ev) => { ev.stopPropagation(); closeTab(tab.id); }}
              >
                <VsIcon name="close" size={12} />
              </button>
            </div>
          ))}
          <div className="ace-console-newgroup" ref={shellGroupRef}>
            <button
              type="button"
              className="ace-console-iconbtn"
              title="新建终端(默认 shell)"
              disabled={creating}
              onClick={() => createTab()}
            >
              <VsIcon name="add" size={14} />
            </button>
            <button
              type="button"
              className="ace-console-iconbtn ace-console-shell-caret"
              title="选择 shell"
              aria-haspopup="menu"
              aria-expanded={shellMenuOpen}
              disabled={creating}
              onClick={toggleShellMenu}
            >
              <VsIcon name="expandDown" size={12} />
            </button>
            {shellMenuOpen && shellMenuPos && createPortal(
              <div
                className="ace-console-shell-menu"
                role="menu"
                style={{ top: shellMenuPos.top, left: shellMenuPos.left }}
              >
                {buildShellMenuItems(shells, defaultShellId).map((item) => (
                  <button
                    key={item.id}
                    type="button"
                    role="menuitem"
                    className="ace-console-shell-item"
                    data-default={item.isDefault ? 'true' : undefined}
                    onClick={() => pickShell(item)}
                  >
                    <span className="ace-console-shell-label">{item.label}</span>
                    {item.needsPath && (
                      <span className="ace-console-shell-tag">需指定路径</span>
                    )}
                    {item.isDefault && (
                      <VsIcon name="check" size={12} className="shrink-0 opacity-70" />
                    )}
                  </button>
                ))}
                {buildShellMenuItems(shells, defaultShellId).length === 0 && (
                  <div className="ace-console-shell-empty">无可用 shell</div>
                )}
              </div>,
              document.body,
            )}
          </div>
        </div>
        <div className="flex items-center gap-1 shrink-0">
          {backend === 'pipe' && (
            <span
              className="text-[11px] text-fg-mute px-2"
              title="当前系统不支持伪终端,运行于兼容模式:交互式程序(vim/top 等)不可用"
            >
              兼容模式
            </span>
          )}
          <button
            type="button"
            className="ace-console-iconbtn"
            title="收起控制台 (Ctrl+`)"
            onClick={() => onToggle(false)}
          >
            <VsIcon name="close" size={14} />
          </button>
        </div>
      </div>
      <div ref={bodyRef} className="ace-console-body">
        {tabsState.tabs.map((tab) => (
          <div
            key={tab.id}
            className="ace-console-term"
            data-tab-id={tab.id}
            data-drop-active={dropHoverTabId === tab.id ? 'true' : undefined}
            style={{ display: tab.id === tabsState.activeId ? 'block' : 'none' }}
            ref={(el) => mountContainer(tab.id, el)}
            onDragEnter={(e) => handleTermDragEnter(tab.id, e)}
            onDragOver={(e) => handleTermDragOver(tab.id, e)}
            onDragLeave={(e) => handleTermDragLeave(tab.id, e)}
            onDrop={(e) => handleTermDrop(tab.id, e)}
            onContextMenu={(e) => handleTermContextMenu(tab.id, e)}
          />
        ))}
        {tabsState.tabs.length === 0 && (
          <div className="h-full flex items-center justify-center text-fg-mute text-sm">
            {creating ? '正在启动终端…' : '没有打开的终端'}
          </div>
        )}
      </div>
      {bashPrompt && (
        <Modal onClose={() => setBashPrompt(null)} width={460}>
          {({ close }) => (
            <div className="p-4 flex flex-col gap-3">
              <div className="text-sm font-medium text-fg">指定 Git Bash 路径</div>
              <div className="text-[12px] text-fg-mute leading-relaxed">
                未自动找到 Git Bash。请输入 <code>bash.exe</code> 的完整路径
                (例如 <code>C:\Program Files\Git\bin\bash.exe</code>)。保存后会永久记住。
              </div>
              <input
                type="text"
                autoFocus
                className="w-full h-9 px-2.5 rounded-md bg-surface-alt border border-border text-fg text-[13px] outline-none focus:border-accent"
                placeholder="C:\Program Files\Git\bin\bash.exe"
                value={bashPathInput}
                onChange={(e) => { setBashPathInput(e.target.value); setBashError(''); }}
                onKeyDown={(e) => { if (e.key === 'Enter') submitBashPath(); }}
              />
              {bashError && <div className="text-[12px] text-danger">{bashError}</div>}
              <div className="flex justify-end gap-2 pt-1">
                <button
                  type="button"
                  className="h-8 px-3 rounded-md border border-border text-fg text-[12px] hover:bg-surface-hi"
                  onClick={close}
                >
                  取消
                </button>
                <button
                  type="button"
                  className="h-8 px-3 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90"
                  onClick={submitBashPath}
                >
                  保存并打开
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
    </div>
  );
}
