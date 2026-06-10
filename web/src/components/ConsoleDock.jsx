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
import { useTheme } from '../theme.jsx';
import { VsIcon } from './Icon.jsx';

const api = createApi();

const TERMINAL_FONT =
  'ui-monospace, "Cascadia Mono", Consolas, "SF Mono", Menlo, monospace';

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

export function ConsoleDock({ open, height, onHeightChange, onToggle, consoleInfo }) {
  const { theme: mode } = useTheme();
  const [tabsState, setTabsState] = useState(createDockTabs);
  const [creating, setCreating] = useState(false);
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

  const createTab = useCallback(async () => {
    if (creating) return;
    setCreating(true);
    try {
      const info = await api.createPty({});
      setTabsState((s) => addTab(s, info));
    } catch (err) {
      console.warn('[console] create pty failed', err);
    } finally {
      setCreating(false);
    }
  }, [creating]);

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
          <button
            type="button"
            className="ace-console-iconbtn"
            title="新建终端"
            disabled={creating}
            onClick={createTab}
          >
            <VsIcon name="add" size={14} />
          </button>
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
            style={{ display: tab.id === tabsState.activeId ? 'block' : 'none' }}
            ref={(el) => mountContainer(tab.id, el)}
          />
        ))}
        {tabsState.tabs.length === 0 && (
          <div className="h-full flex items-center justify-center text-fg-mute text-sm">
            {creating ? '正在启动终端…' : '没有打开的终端'}
          </div>
        )}
      </div>
    </div>
  );
}
