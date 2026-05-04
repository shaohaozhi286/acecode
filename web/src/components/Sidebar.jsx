// Sidebar(200px):workspace 分组 → session list,底部 Skills/MCP tab。
//
// 数据源:
//   - 优先走共享 daemon 的 /api/workspaces + workspace-scoped sessions。
//   - Desktop bridge 只作为启动/注册 shared daemon 的 fallback。
//
// 收起态(view !== 'single')→ width 0,sidebar 整个折叠让出主区。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { relativeTime, clsx } from '../lib/format.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

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

function WorkspaceGroup({ ws, expanded, onToggle, sessions, activeId, onSelect, onRename, onActivate, onNewSession }) {
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
          'group flex items-center gap-2 mx-1.5 px-2.5 py-[6px] rounded-md text-[12px] cursor-pointer transition',
          ws.active ? 'bg-accent-bg text-fg' : 'text-fg hover:bg-surface-hi',
        )}
        onClick={() => (ws.active ? onToggle(ws.hash) : onActivate(ws))}
      >
        <span className="w-3 shrink-0 flex items-center justify-center opacity-70">
          <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={10} />
        </span>
        <VsIcon name="folder" size={14} />
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
        <button
          type="button"
          onClick={(e) => { e.stopPropagation(); onNewSession(ws); }}
          className="w-5 h-5 rounded text-fg-mute hover:text-accent hover:bg-surface-hi flex items-center justify-center shrink-0 transition"
          title="新增会话"
        ><VsIcon name="editWindow" size={13} /></button>
        <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', statusDot(ws.daemon_state))} title={ws.daemon_state || 'stopped'} />
        <button
          type="button"
          onClick={(e) => { e.stopPropagation(); setEditing(true); }}
          className="text-[11px] text-fg-mute hover:text-fg opacity-0 group-hover:opacity-100 transition px-1"
          title="重命名"
        ><VsIcon name="edit" size={12} /></button>
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

export function Sidebar({ activeId, onSelect, collapsed, width = 200, onOpenSkills, onOpenMcp }) {
  const [pane,        setPane]        = useState('sessions'); // 'sessions' | 'skills' | 'mcp'
  const [workspaces,  setWorkspaces]  = useState([]);
  const [sessions,    setSessions]    = useState([]);
  const [expanded,    setExpanded]    = useState(new Set());
  const [activeWorkspaceHash, setActiveWorkspaceHash] = useState('');
  const refreshingRef = useRef(false);

  const refresh = useCallback(async (preferredHash = activeWorkspaceHash) => {
    if (refreshingRef.current) return;
    refreshingRef.current = true;
    try {
      let workspaceArr = [];
      try {
        const list = await api.listWorkspaces();
        workspaceArr = Array.isArray(list)
          ? list.map((w) => ({ ...w, daemon_state: w.daemon_state || 'running' }))
          : [];
      } catch {
        workspaceArr = [];
      }

      if (workspaceArr.length === 0 && hasDesktopBridge()) {
        try {
          const list = parseDesktopResult(await window.aceDesktop_listWorkspaces());
          workspaceArr = Array.isArray(list) ? list : [];
        } catch {
          workspaceArr = [];
        }
      }

      if (workspaceArr.length === 0 && !hasDesktopBridge()) {
        workspaceArr = [{ hash: '__local__', cwd: '', name: '当前会话',
                          daemon_state: 'running', active: true }];
      }

      const chosen = preferredHash || workspaceArr.find((w) => w.active)?.hash || workspaceArr[0]?.hash || '';
      const withActive = workspaceArr.map((w) => ({ ...w, active: w.hash === chosen }));
      setActiveWorkspaceHash(chosen);
      setWorkspaces(withActive);
      setExpanded((prev) => {
        const next = new Set(prev);
        for (const w of withActive) if (w.active) next.add(w.hash);
        return next;
      });

      try {
        const visibleWorkspaces = withActive.filter((w) => w.active || w.hash === '__local__');
        const perWorkspace = await Promise.all(visibleWorkspaces.map(async (w) => {
          if (w.hash === '__local__') {
            const list = await api.listSessions();
            return (Array.isArray(list) ? list : []).map((s) => ({ ...s, workspace_hash: s.workspace_hash || w.hash, cwd: s.cwd || w.cwd }));
          }
          const list = await api.listWorkspaceSessions(w.hash);
          return (Array.isArray(list) ? list : []).map((s) => ({ ...s, workspace_hash: s.workspace_hash || w.hash, cwd: s.cwd || w.cwd }));
        }));
        const arr = perWorkspace.flat();
        setSessions(arr);
        arr.filter((s) => s.active && s.id).forEach((s) => connection.subscribe(s.id));
      }
      catch { /* 鉴权失败不致命 */ }
    } finally {
      refreshingRef.current = false;
    }
  }, [activeWorkspaceHash]);

  useEffect(() => {
    refresh();
    const t = setInterval(() => refresh().catch(() => {}), 5000);
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
    setActiveWorkspaceHash(ws.hash);
    setExpanded((prev) => new Set(prev).add(ws.hash));
    if (!hasDesktopBridge()) return;
    try {
      const r = parseDesktopResult(await window.aceDesktop_activateWorkspace(ws.hash));
      if (r.error) { toast({ kind: 'err', text: '切换失败:' + r.error }); return; }
      const currentPort = Number(location.port || (location.protocol === 'https:' ? 443 : 80));
      if (r.port && Number(r.port) !== currentPort) {
        location.href = `http://127.0.0.1:${r.port}/?token=${encodeURIComponent(r.token)}`;
      } else {
        refresh(ws.hash).catch(() => {});
      }
    } catch (e) { toast({ kind: 'err', text: '切换异常:' + (e.message || '') }); }
  };

  const selectSession = async (ws, session) => {
    if (!session?.id) return;
    if (!session.active) {
      try {
        if (ws?.hash && ws.hash !== '__local__') {
          await api.resumeWorkspaceSession(ws.hash, session.id);
        } else {
          await api.resumeSession(session.id);
        }
        refresh().catch(() => {});
      } catch (e) {
        if (hasDesktopBridge() && ws?.hash) {
          try {
            const r = parseDesktopResult(await window.aceDesktop_resumeSession(ws.hash, session.id));
            if (r.error) { toast({ kind: 'err', text: '恢复失败:' + r.error }); return; }
            onSelect?.({
              workspaceHash: r.workspace_hash || ws.hash,
              contextId: r.context_id,
              sessionId: r.session_id || session.id,
              port: r.port,
              token: r.token,
              cwd: r.cwd || ws.cwd,
              title: session.title,
              summary: session.summary,
              message_count: session.message_count,
              created_at: session.created_at,
              updated_at: session.updated_at,
            });
            return;
          } catch (inner) {
            toast({ kind: 'err', text: '恢复异常:' + (inner.message || '') });
            return;
          }
        } else {
          toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
          return;
        }
      }
    }
    onSelect?.({
      workspaceHash: session.workspace_hash || ws?.hash,
      contextId: 'default',
      sessionId: session.id,
      port: ws?.port,
      token: ws?.token,
      cwd: session.cwd || ws?.cwd,
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

  const createSessionInWorkspace = async (ws) => {
    if (!ws?.hash) return;
    try {
      const r = ws.hash === '__local__'
        ? await api.createSession({})
        : await api.createWorkspaceSession(ws.hash, {});
      const id = r && (r.session_id || r.id);
      if (!id) return;

      const workspaceHash = r.workspace_hash || ws.hash;
      const cwd = r.cwd || ws.cwd;
      const now = new Date().toISOString();
      const nextSession = {
        ...r,
        id,
        active: true,
        status: r.status || 'idle',
        workspace_hash: workspaceHash,
        cwd,
        created_at: r.created_at || now,
        updated_at: r.updated_at || now,
      };

      setActiveWorkspaceHash(workspaceHash);
      setExpanded((prev) => new Set(prev).add(workspaceHash));
      setWorkspaces((prev) => prev.map((item) => ({ ...item, active: item.hash === workspaceHash })));
      setSessions((prev) => [nextSession, ...prev.filter((item) => item.id !== id)]);
      connection.subscribe(id);
      onSelect?.({
        workspaceHash,
        contextId: 'default',
        sessionId: id,
        port: ws.port,
        token: ws.token,
        cwd,
        title: r.title,
        summary: r.summary,
        message_count: r.message_count,
        created_at: r.created_at,
        updated_at: r.updated_at,
      });
      refresh(workspaceHash).catch(() => {});
    } catch (e) {
      toast({ kind: 'err', text: '新增会话失败:' + (e.message || '') });
    }
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
      try { await api.registerWorkspace(ws.cwd); } catch { /* daemon 可能已入册;忽略 */ }
      onActivate(ws);
    } catch (e) {
      toast({ kind: 'err', text: '添加项目失败:' + (e.message || '') });
    }
  };

  return (
    <aside
      className={[
        'ace-sidebar bg-surface-alt border-r border-border flex flex-col font-sans shrink-0 overflow-hidden',
        'transition-[width,min-width] duration-250',
        collapsed ? 'w-0 min-w-0' : '',
      ].join(' ')}
      style={collapsed ? undefined : { width, minWidth: width }}
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
            ><VsIcon name="folderAdd" size={15} /></button>
          </div>
          <div className="flex-1 overflow-y-auto pb-2">
            {workspaces.map((ws) => {
              const items = sessions.filter((s) => s.workspace_hash ? s.workspace_hash === ws.hash : !!ws.active);
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
                  onNewSession={createSessionInWorkspace}
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
          { key: 'sessions', label: '会话', icon: 'document' },
          { key: 'skills',   label: 'Skills', icon: 'extension' },
          { key: 'mcp',      label: 'MCP', icon: 'mcp' },
        ].map((t) => (
          <button
            key={t.key}
            type="button"
            onClick={() => setPane(t.key)}
            className={clsx(
              'flex-1 py-[5px] text-[11px] rounded transition flex items-center justify-center gap-1',
              pane === t.key ? 'text-accent' : 'text-fg-mute hover:text-fg hover:bg-surface-hi',
            )}
          >
            <VsIcon name={t.icon} size={18} />
            {t.label}
          </button>
        ))}
      </div>
    </aside>
  );
}
