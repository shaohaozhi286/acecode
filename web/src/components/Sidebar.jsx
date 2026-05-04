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
import { SESSION_PIN_TOGGLE_EVENT } from '../lib/desktopContextMenu.js';
import { relativeTime, clsx } from '../lib/format.js';
import {
  filterPinnedSessions,
  normalizePinnedIds,
  pinSessionId,
  pinnedSessionsForList,
  unpinSessionId,
} from '../lib/pinnedSessions.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import {
  applyStatusSnapshot,
  applyStatusUpdate,
  mergeSessionStatus,
  mergeSessionsWithStatus,
  optimisticReadStatus,
  statusCursor,
  workspaceHasUnread,
} from '../lib/sessionStatus.js';
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

function attentionMeta(state) {
  if (state === 'in_progress') return { label: '进行中', dot: 'bg-accent shadow-[0_0_4px_var(--ace-accent)]', text: 'text-accent' };
  if (state === 'unread') return { label: '未读', dot: 'bg-ok shadow-[0_0_4px_var(--ace-ok)]', text: 'text-ok' };
  return { label: '已读', dot: 'bg-fg-mute/45', text: 'text-fg-mute' };
}

function daemonHealthMeta(state) {
  if (state === 'failed') return { label: '失败', className: 'border-danger/40 text-danger bg-danger/10' };
  if (state === 'starting' || state === 'waiting') return { label: '启动', className: 'border-warn/40 text-warn bg-warn/10' };
  if (state === 'running') return { label: '运行', className: 'border-border text-fg-mute bg-surface-hi' };
  return { label: '停止', className: 'border-border text-fg-mute bg-surface' };
}

function SessionRow({ s, active, pinned = false, onSelect, onTogglePin }) {
  const attention = s.attention_state || s.read_state || 'read';
  const meta = attentionMeta(attention);
  const workspaceHash = s.workspace_hash || s.workspaceHash || '';
  return (
    <div
      data-desktop-session-id={s.id || undefined}
      data-desktop-session-workspace={workspaceHash || undefined}
      data-desktop-session-pinned={pinned ? 'true' : 'false'}
      className={clsx(
        'group flex items-center gap-1 mx-1.5 my-px pl-1 pr-2 rounded-md text-[12px] transition',
        active
          ? 'bg-accent-soft/50 text-accent'
          : 'text-fg hover:bg-surface-hi',
        attention === 'unread' && !active && 'font-semibold',
      )}
    >
      <button
        type="button"
        onClick={(e) => {
          e.preventDefault();
          e.stopPropagation();
          onTogglePin?.(s, !pinned);
        }}
        className={clsx(
          'ace-session-pin-btn w-5 h-6 rounded flex items-center justify-center shrink-0 transition',
          pinned
            ? 'opacity-100 text-accent'
            : 'opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 text-fg-mute hover:text-accent',
        )}
        title={pinned ? '取消置顶' : '置顶'}
        aria-label={pinned ? '取消置顶' : '置顶'}
      >
        <VsIcon name="pin" size={12} />
      </button>
      <button
        type="button"
        onClick={(e) => { e.preventDefault(); onSelect(s); }}
        className="flex flex-1 items-center gap-2 min-w-0 py-[5px] bg-transparent text-left cursor-pointer"
      >
        <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', meta.dot)} title={meta.label} />
        <span className="flex-1 truncate">{sessionDisplayTitle(s, s.name || s.id)}</span>
        <span className={clsx('text-[10px] shrink-0', meta.text)}>{meta.label}</span>
        <span className="text-[10px] text-fg-mute shrink-0">{relativeTime(s.updated_at || s.created_at)}</span>
      </button>
    </div>
  );
}

function WorkspaceGroup({ ws, expanded, onToggle, sessions, activeId, onSelect, onRename, onActivate, onNewSession, onTogglePin }) {
  const [editing, setEditing] = useState(false);
  const [draft,   setDraft]   = useState(ws.name);
  const hasUnread = workspaceHasUnread(sessions);
  const daemon = daemonHealthMeta(ws.daemon_state || 'stopped');

  useEffect(() => {
    if (!editing) setDraft(ws.name);
  }, [editing, ws.name]);

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
        data-desktop-open-in-explorer-kind="workspace"
        data-desktop-open-in-explorer-path={ws.cwd || undefined}
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
        {hasUnread && <span className="w-1.5 h-1.5 rounded-full shrink-0 bg-ok shadow-[0_0_4px_var(--ace-ok)]" title="有未读会话" />}
        <span
          className={clsx('px-1.5 py-px rounded border text-[9px] leading-none shrink-0', daemon.className)}
          title={`daemon: ${ws.daemon_state || 'stopped'}`}
        >{daemon.label}</span>
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
              <SessionRow key={s.id} s={s} active={s.id === activeId} onSelect={onSelect} onTogglePin={onTogglePin} />
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
  const [statusBySession, setStatusBySession] = useState(() => new Map());
  const [pinnedByWorkspace, setPinnedByWorkspace] = useState(() => new Map());
  const [expanded,    setExpanded]    = useState(new Set());
  const [activeWorkspaceHash, setActiveWorkspaceHash] = useState('');
  const refreshingRef = useRef(false);
  const expandedRef = useRef(new Set());
  const pinnedByWorkspaceRef = useRef(new Map());
  const retainedSessionIdsRef = useRef(new Set());

  const updateExpanded = useCallback((updater) => {
    setExpanded((prev) => {
      const next = typeof updater === 'function' ? updater(prev) : updater;
      expandedRef.current = next;
      return next;
    });
  }, []);

  const syncRetainedSessionIds = useCallback((ids) => {
    const next = new Set(Array.from(ids || []).filter(Boolean));
    const prev = retainedSessionIdsRef.current;
    for (const sessionId of prev) {
      if (!next.has(sessionId)) connection.releaseSession(sessionId);
    }
    for (const sessionId of next) {
      if (!prev.has(sessionId)) connection.retainSession(sessionId);
    }
    retainedSessionIdsRef.current = next;
  }, []);

  useEffect(() => () => {
    syncRetainedSessionIds([]);
  }, [syncRetainedSessionIds]);

  const setPinnedMap = useCallback((updater) => {
    setPinnedByWorkspace((prev) => {
      const rawNext = typeof updater === 'function' ? updater(prev) : updater;
      const next = rawNext instanceof Map ? new Map(rawNext) : new Map(Object.entries(rawNext || {}));
      pinnedByWorkspaceRef.current = next;
      return next;
    });
  }, []);

  const setPinnedWorkspaceIds = useCallback((workspaceHash, ids) => {
    if (!workspaceHash) return;
    const normalized = normalizePinnedIds(ids);
    setPinnedMap((prev) => {
      const next = new Map(prev);
      if (normalized.length) next.set(workspaceHash, normalized);
      else next.delete(workspaceHash);
      return next;
    });
  }, [setPinnedMap]);

  const togglePinnedSession = useCallback(async (session, nextPinned) => {
    const id = session?.id || session?.sessionId || session?.session_id || '';
    const workspaceHash = session?.workspace_hash || session?.workspaceHash || activeWorkspaceHash || '';
    if (!id || !workspaceHash) return;

    const previous = normalizePinnedIds(pinnedByWorkspaceRef.current.get(workspaceHash) || []);
    const shouldPin = typeof nextPinned === 'boolean' ? nextPinned : !previous.includes(id);
    const next = shouldPin ? pinSessionId(previous, id) : unpinSessionId(previous, id);
    setPinnedWorkspaceIds(workspaceHash, next);

    try {
      const saved = await api.setPinnedSessions(workspaceHash, next);
      setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || next));
      toast({ kind: 'ok', text: shouldPin ? '已置顶' : '已取消置顶' });
    } catch (e) {
      setPinnedWorkspaceIds(workspaceHash, previous);
      toast({ kind: 'err', text: (shouldPin ? '置顶失败:' : '取消置顶失败:') + (e.message || '') });
    }
  }, [activeWorkspaceHash, setPinnedWorkspaceIds]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      togglePinnedSession({
        id: detail.sessionId,
        workspace_hash: detail.workspaceHash,
      }, detail.pinned);
    };
    window.addEventListener(SESSION_PIN_TOGGLE_EVENT, handler);
    return () => window.removeEventListener(SESSION_PIN_TOGGLE_EVENT, handler);
  }, [togglePinnedSession]);

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
      const expandedHashes = new Set(expandedRef.current);
      if (chosen) expandedHashes.add(chosen);
      setActiveWorkspaceHash(chosen);
      setWorkspaces(withActive);

      const pinnedPairs = await Promise.all(withActive.map(async (w) => {
        if (!w.hash) return [w.hash, []];
        try {
          const state = await api.getPinnedSessions(w.hash);
          return [w.hash, normalizePinnedIds(state?.session_ids || [])];
        } catch {
          return [w.hash, normalizePinnedIds(pinnedByWorkspaceRef.current.get(w.hash) || [])];
        }
      }));
      const nextPinnedMap = new Map();
      for (const [hash, ids] of pinnedPairs) {
        if (hash && ids.length) nextPinnedMap.set(hash, ids);
      }
      setPinnedMap(nextPinnedMap);
      const pinnedWorkspaceHashes = new Set(Array.from(nextPinnedMap.entries())
        .filter(([, ids]) => ids.length > 0)
        .map(([hash]) => hash));

      updateExpanded((prev) => {
        const next = new Set(prev);
        for (const w of withActive) if (w.active) next.add(w.hash);
        return next;
      });
      withActive
        .filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || pinnedWorkspaceHashes.has(w.hash))
        .forEach((w) => connection.subscribeWorkspaceStatus(w.hash));

      try {
        const visibleWorkspaces = withActive.filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || pinnedWorkspaceHashes.has(w.hash));
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
        setStatusBySession((prev) => arr.reduce((map, s) => applyStatusUpdate(map, {
          ...s,
          session_id: s.id,
          state: s.attention_state || s.read_state,
          cursor: s.status_cursor,
        }), prev));
        syncRetainedSessionIds(arr.filter((s) => s.active && s.id).map((s) => s.id));
      }
      catch { /* 鉴权失败不致命 */ }
    } finally {
      refreshingRef.current = false;
    }
  }, [activeWorkspaceHash, setPinnedMap, syncRetainedSessionIds, updateExpanded]);

  useEffect(() => {
    refresh();
    const t = setInterval(() => refresh().catch(() => {}), 5000);
    return () => clearInterval(t);
  }, [refresh]);

  useEffect(() => {
    const handler = (e) => {
      const msg = e.detail || {};
      if (msg.type === 'session_status_snapshot') {
        setStatusBySession((prev) => applyStatusSnapshot(prev, msg.payload || {}));
      } else if (msg.type === 'session_status' || msg.type === 'mark_session_read_ack') {
        setStatusBySession((prev) => applyStatusUpdate(prev, msg.payload || {}));
      }
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, []);

  const markSessionRead = useCallback((session) => {
    if (!session?.id) return;
    const merged = mergeSessionStatus(session, statusBySession);
    const cursor = statusCursor(merged);
    connection.markSessionRead({
      sessionId: merged.id,
      workspaceHash: merged.workspace_hash || '',
      cursor,
    });
    const optimistic = optimisticReadStatus(merged);
    if (optimistic) setStatusBySession((prev) => applyStatusUpdate(prev, optimistic));
  }, [statusBySession]);

  useEffect(() => {
    if (!activeId) return;
    const session = sessions.find((s) => s.id === activeId);
    if (!session) return;
    const merged = mergeSessionStatus(session, statusBySession);
    if (merged.attention_state === 'unread') markSessionRead(merged);
  }, [activeId, sessions, statusBySession, markSessionRead]);

  const onToggle = (hash) => {
    updateExpanded((prev) => {
      const next = new Set(prev);
      next.has(hash) ? next.delete(hash) : next.add(hash);
      return next;
    });
  };

  const onActivate = async (ws) => {
    setActiveWorkspaceHash(ws.hash);
    updateExpanded((prev) => new Set(prev).add(ws.hash));
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
            markSessionRead({
              ...session,
              id: r.session_id || session.id,
              workspace_hash: r.workspace_hash || ws.hash,
              cwd: r.cwd || ws.cwd,
            });
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
    markSessionRead(session);
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
    setWorkspaces((prev) => prev.map((w) => w.hash === hash ? { ...w, name } : w));
    await refresh(hash);
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
        attention_state: r.attention_state || 'read',
        read_state: r.read_state || 'read',
        workspace_hash: workspaceHash,
        cwd,
        created_at: r.created_at || now,
        updated_at: r.updated_at || now,
      };

      setActiveWorkspaceHash(workspaceHash);
      updateExpanded((prev) => new Set(prev).add(workspaceHash));
      setWorkspaces((prev) => prev.map((item) => ({ ...item, active: item.hash === workspaceHash })));
      setSessions((prev) => [nextSession, ...prev.filter((item) => item.id !== id)]);
      setStatusBySession((prev) => applyStatusUpdate(prev, { ...nextSession, session_id: id, state: 'read' }));
      connection.subscribeWorkspaceStatus(workspaceHash);
      syncRetainedSessionIds(new Set([...retainedSessionIdsRef.current, id]));
      markSessionRead(nextSession);
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

  const renderedSessions = mergeSessionsWithStatus(sessions, statusBySession);
  const pinnedSessions = pinnedSessionsForList(renderedSessions, pinnedByWorkspace);
  const workspaceForSession = (session) => {
    const hash = session.workspace_hash || session.workspaceHash || '';
    return workspaces.find((w) => w.hash === hash) || {
      hash,
      cwd: session.cwd || '',
      name: hash || 'workspace',
      daemon_state: 'running',
    };
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
            {pinnedSessions.length > 0 && (
              <div className="mb-2">
                <div className="px-4 pt-1 pb-1 text-[10px] font-semibold uppercase tracking-wider text-fg-mute">置顶</div>
                <div className="my-1">
                  {pinnedSessions.map((s) => (
                    <SessionRow
                      key={`pinned-${s.workspace_hash || ''}-${s.id}`}
                      s={s}
                      pinned
                      active={s.id === activeId}
                      onSelect={(session) => selectSession(workspaceForSession(session), session)}
                      onTogglePin={togglePinnedSession}
                    />
                  ))}
                </div>
              </div>
            )}
            {workspaces.map((ws) => {
              const items = filterPinnedSessions(
                renderedSessions.filter((s) => s.workspace_hash ? s.workspace_hash === ws.hash : !!ws.active),
                pinnedByWorkspace,
              );
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
                  onTogglePin={togglePinnedSession}
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
