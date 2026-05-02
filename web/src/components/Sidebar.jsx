// Sidebar(200px):workspace 分组 → session list,底部 Skills/MCP tab。
//
// 数据源:
//   - 当 window.aceDesktop_listWorkspaces 存在(desktop 多 workspace 模式),
//     调它拿 workspaces;切换走整页 navigate(跨 loopback 端口 fetch 受 CORS 拦截)
//   - 否则走 api.listSessions() 拿当前 daemon 的 sessions,做单 workspace 显示
//
// 收起态(view !== 'single')→ width 0,sidebar 整个折叠让出主区。

import { useCallback, useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { relativeTime, clsx } from '../lib/format.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { toast } from './Toast.jsx';

function hasDesktopBridge() {
  return typeof window.aceDesktop_listWorkspaces === 'function';
}

function parseDesktopResult(value) {
  // webview/webview 会把 native binding 返回的 JSON value 先解析成 JS 值；
  // 但开发调试 shim 可能仍返回原始字符串。两种形态都兼容。
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function statusDot(state) {
  if (state === 'running')   return 'bg-ok shadow-[0_0_4px_var(--ace-ok)]';
  if (state === 'starting')  return 'bg-warn shadow-[0_0_4px_var(--ace-warn)]';
  if (state === 'failed')    return 'bg-danger shadow-[0_0_4px_var(--ace-danger)]';
  if (state === 'waiting')   return 'bg-warn';
  return 'bg-fg-mute';
}

function SessionRow({ s, active, onSelect }) {
  return (
    <a
      href="#"
      onClick={(e) => { e.preventDefault(); onSelect(s); }}
      className={clsx(
        'flex items-center gap-2 mx-1.5 my-px px-2 py-[5px] pl-[22px] rounded-md text-[12px] transition cursor-pointer',
        active
          ? 'bg-accent-soft/50 text-accent'
          : 'text-fg hover:bg-surface-hi',
      )}
    >
      <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', statusDot(s.status))} />
      <span className="flex-1 truncate">{sessionDisplayTitle(s, s.name || s.id)}</span>
      <span className="text-[10px] text-fg-mute shrink-0">{relativeTime(s.updated_at || s.created_at)}</span>
    </a>
  );
}

function WorkspaceGroup({ ws, expanded, onToggle, sessions, activeId, onSelect, onRename, onActivate }) {
  const [editing, setEditing] = useState(false);
  const [draft,   setDraft]   = useState(ws.name);

  const commit = async () => {
    setEditing(false);
    const name = draft.trim();
    if (!name || name === ws.name) { setDraft(ws.name); return; }
    try { await onRename(ws.hash, name); }
    catch (e) {
      toast({ kind: 'err', text: '重命名失败:' + (e.message || '') });
      setDraft(ws.name);
    }
  };

  return (
    <div className={clsx('my-px', ws.active && 'rounded-md')}>
      <div
        className={clsx(
          'flex items-center gap-2 mx-1.5 px-2.5 py-[6px] rounded-md text-[12px] cursor-pointer transition',
          ws.active ? 'bg-accent-bg text-fg' : 'text-fg hover:bg-surface-hi',
        )}
        onClick={() => (ws.active ? onToggle(ws.hash) : onActivate(ws))}
      >
        <span className="text-[10px] w-3 shrink-0 text-fg-mute">{expanded ? '▾' : '▸'}</span>
        <span className="text-[13px] shrink-0">📁</span>
        {editing ? (
          <input
            autoFocus
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') { e.preventDefault(); commit(); }
              if (e.key === 'Escape') { setEditing(false); setDraft(ws.name); }
            }}
            onBlur={commit}
            onClick={(e) => e.stopPropagation()}
            className="flex-1 px-1 py-0 text-[12px] bg-surface border border-accent rounded outline-none"
          />
        ) : (
          <span className="flex-1 truncate font-medium">{ws.name || ws.hash}</span>
        )}
        <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', statusDot(ws.daemon_state))} title={ws.daemon_state || 'stopped'} />
        <button
          type="button"
          onClick={(e) => { e.stopPropagation(); setEditing(true); }}
          className="text-[11px] text-fg-mute hover:text-fg opacity-0 group-hover:opacity-100 transition px-1"
          title="重命名"
        >✎</button>
      </div>
      {expanded && (
        <div className="my-1">
          {sessions.length === 0 ? (
            <div className="mx-1.5 ml-[22px] px-2 py-[3px] text-[11px] text-fg-mute italic">暂无对话</div>
          ) : (
            sessions.map((s) => (
              <SessionRow key={s.id} s={s} active={s.id === activeId} onSelect={onSelect} />
            ))
          )}
        </div>
      )}
    </div>
  );
}

export function Sidebar({ activeId, onSelect, collapsed, onOpenSkills, onOpenMcp }) {
  const [pane,        setPane]        = useState('sessions'); // 'sessions' | 'skills' | 'mcp'
  const [workspaces,  setWorkspaces]  = useState([]);
  const [sessions,    setSessions]    = useState([]);
  const [expanded,    setExpanded]    = useState(new Set());

  const refresh = useCallback(async () => {
    if (hasDesktopBridge()) {
      try {
        const list = parseDesktopResult(await window.aceDesktop_listWorkspaces());
        const arr = Array.isArray(list) ? list : [];
        setWorkspaces(arr);
        setExpanded((prev) => {
          const next = new Set(prev);
          for (const w of arr) if (w.active) next.add(w.hash);
          return next;
        });
      } catch {
        setWorkspaces([]);
      }
    } else {
      setWorkspaces([{ hash: '__local__', cwd: '', name: '当前会话',
                       daemon_state: 'running', active: true }]);
      setExpanded((prev) => new Set(prev).add('__local__'));
    }
    try {
      const list = await api.listSessions();
      const arr = Array.isArray(list) ? list : [];
      setSessions(arr);
      arr.filter((s) => s.active && s.id).forEach((s) => connection.subscribe(s.id));
    }
    catch { /* 鉴权失败不致命 */ }
  }, []);

  useEffect(() => {
    refresh();
    const t = setInterval(() => refresh().catch(() => {}), 2000);
    return () => clearInterval(t);
  }, [refresh]);

  const onToggle = (hash) => {
    setExpanded((prev) => {
      const next = new Set(prev);
      next.has(hash) ? next.delete(hash) : next.add(hash);
      return next;
    });
  };

  const onActivate = async (ws) => {
    if (!hasDesktopBridge()) return;
    try {
      const r = parseDesktopResult(await window.aceDesktop_activateWorkspace(ws.hash));
      if (r.error) { toast({ kind: 'err', text: '切换失败:' + r.error }); return; }
      // 整页 navigate(跨 loopback 端口 fetch 受 CORS 拦截)
      location.href = `http://127.0.0.1:${r.port}/?token=${encodeURIComponent(r.token)}`;
    } catch (e) { toast({ kind: 'err', text: '切换异常:' + (e.message || '') }); }
  };

  const selectSession = async (ws, session) => {
    if (!session?.id) return;
    if (!session.active) {
      if (hasDesktopBridge() && ws?.hash) {
        try {
          const r = parseDesktopResult(await window.aceDesktop_resumeSession(ws.hash, session.id));
          if (r.error) { toast({ kind: 'err', text: '恢复失败:' + r.error }); return; }
          onSelect?.({
            workspaceHash: ws.hash,
            contextId: r.context_id,
            sessionId: r.session_id || session.id,
            port: r.port,
            token: r.token,
            title: session.title,
            summary: session.summary,
            message_count: session.message_count,
            created_at: session.created_at,
            updated_at: session.updated_at,
          });
          return;
        } catch (e) {
          toast({ kind: 'err', text: '恢复异常:' + (e.message || '') });
          return;
        }
      }
      try {
        await api.resumeSession(session.id);
        refresh().catch(() => {});
      } catch (e) {
        toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
        return;
      }
    }
    onSelect?.({
      workspaceHash: ws?.hash,
      contextId: 'default',
      sessionId: session.id,
      port: ws?.port,
      token: ws?.token,
      title: session.title,
      summary: session.summary,
      message_count: session.message_count,
      created_at: session.created_at,
      updated_at: session.updated_at,
    });
  };

  const onRename = async (hash, name) => {
    if (!hasDesktopBridge()) throw new Error('not in desktop mode');
    const r = parseDesktopResult(await window.aceDesktop_renameWorkspace(hash, name));
    if (!r.ok) throw new Error(r.error || 'rename failed');
    refresh();
  };

  const onAddWorkspace = async () => {
    if (!hasDesktopBridge()) {
      toast({ kind: 'info', text: '需在 desktop shell 中使用' });
      return;
    }
    try {
      const ws = parseDesktopResult(await window.aceDesktop_addWorkspace());
      if (ws == null) return;
      if (!ws || !ws.hash) return;
      onActivate(ws);
    } catch (e) {
      toast({ kind: 'err', text: '添加项目失败:' + (e.message || '') });
    }
  };

  return (
    <aside
      className={[
        'bg-surface-alt border-r border-border flex flex-col font-sans shrink-0 overflow-hidden',
        'transition-[width,min-width] duration-250',
        collapsed ? 'w-0 min-w-0' : 'w-[200px] min-w-[200px]',
      ].join(' ')}
    >
      <div className="flex-1 flex flex-col min-h-0">
        <div className={clsx('flex-1 flex flex-col min-h-0', pane !== 'sessions' && 'hidden')}>
          <div className="flex items-center justify-between px-2 pt-2.5 pb-1 text-[10px] font-semibold uppercase tracking-wider text-fg-mute">
            <span>项目</span>
            <button
              type="button"
              onClick={onAddWorkspace}
              className="w-5 h-5 rounded text-fg-mute hover:text-fg hover:bg-surface-hi text-[14px] leading-none flex items-center justify-center"
              title="添加项目"
            >+</button>
          </div>
          <div className="flex-1 overflow-y-auto pb-2">
            {workspaces.map((ws) => {
              const items = ws.active ? sessions : [];
              return (
                <WorkspaceGroup
                  key={ws.hash}
                  ws={ws}
                  expanded={expanded.has(ws.hash)}
                  onToggle={onToggle}
                  sessions={items}
                  activeId={activeId}
                  onSelect={(session) => selectSession(ws, session)}
                  onRename={onRename}
                  onActivate={onActivate}
                />
              );
            })}
          </div>
        </div>

        <div className={clsx('flex-1 overflow-y-auto', pane !== 'skills' && 'hidden')}>
          {/* SkillsPanel 是右侧滑出面板,左侧 tab 不内嵌列表 — 点击直接打开 */}
          <button
            type="button"
            onClick={onOpenSkills}
            className="m-2 w-[calc(100%-1rem)] py-2 rounded-md border border-dashed border-border text-fg-mute hover:text-accent hover:border-accent text-xs transition"
          >
            打开 Skills 面板
          </button>
        </div>
        <div className={clsx('flex-1 overflow-y-auto', pane !== 'mcp' && 'hidden')}>
          <button
            type="button"
            onClick={onOpenMcp}
            className="m-2 w-[calc(100%-1rem)] py-2 rounded-md border border-dashed border-border text-fg-mute hover:text-accent hover:border-accent text-xs transition"
          >
            打开 MCP 面板
          </button>
        </div>
      </div>
      <div className="border-t border-border px-2.5 py-2 flex gap-1">
        {[
          { key: 'sessions', label: '会话' },
          { key: 'skills',   label: 'Skills' },
          { key: 'mcp',      label: 'MCP' },
        ].map((t) => (
          <button
            key={t.key}
            type="button"
            onClick={() => setPane(t.key)}
            className={clsx(
              'flex-1 py-[5px] text-[11px] rounded transition',
              pane === t.key ? 'text-accent' : 'text-fg-mute hover:text-fg hover:bg-surface-hi',
            )}
          >
            {t.label}
          </button>
        ))}
      </div>
    </aside>
  );
}
