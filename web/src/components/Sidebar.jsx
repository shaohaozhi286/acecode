// Sidebar(200px):workspace 分组 → session list。
//
// 数据源:
//   - 优先走共享 daemon 的 /api/workspaces + workspace-scoped sessions。
//   - Desktop bridge 只作为启动/注册 shared daemon 的 fallback。
//
// 收起态(view !== 'single')→ width 0,sidebar 整个折叠让出主区。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  SESSION_PIN_TOGGLE_EVENT,
} from '../lib/desktopContextMenu.js';
import { relativeTime, clsx } from '../lib/format.js';
import {
  filterPinnedSessions,
  normalizePinnedIds,
  pinSessionId,
  pinnedSessionsForList,
  reorderPinnedSessionId,
  unpinSessionId,
} from '../lib/pinnedSessions.js';
import { sessionDisplayTitle, withNewSessionDisplayTitles } from '../lib/sessionTitle.js';
import { pushTrayMenu } from '../lib/desktopTrayMenu.js';
import { usePreference } from '../lib/usePreference.js';
import {
  SESSION_LIST_CHANGED_EVENT,
  normalizeSessionListChangedDetail,
} from '../lib/sessionListEvents.js';
import { sessionHasPendingQuestion } from '../lib/pendingQuestions.js';
import {
  applyStatusSnapshot,
  applyStatusUpdate,
  mergeSessionStatus,
  mergeSessionsWithStatus,
  optimisticReadStatus,
  statusCursor,
  workspaceHasUnread,
} from '../lib/sessionStatus.js';
import {
  reconcileSidebarSessions,
  sidebarSessionProjection,
  upsertSidebarSession,
} from '../lib/sidebarSessions.js';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

const CUSTOM_SECTION_STORAGE_KEY = 'acecode.sidebarCustomSectionExpanded.v1';
const PINNED_DRAG_START_PX = 5;
const PINNED_DRAG_EDGE_SCROLL_PX = 34;
const PINNED_DRAG_EDGE_SCROLL_STEP = 16;

function pinnedSessionKey(workspaceHash, sessionId) {
  const ws = String(workspaceHash || '');
  const id = String(sessionId || '');
  return ws && id ? `${ws}\u0000${id}` : '';
}

function sameStringArray(a = [], b = []) {
  if (a.length !== b.length) return false;
  return a.every((value, index) => value === b[index]);
}

function pinnedDropTargetForPointer(clientY, workspaceHash) {
  if (typeof document === 'undefined') return null;
  const rows = Array.from(document.querySelectorAll('[data-sidebar-pinned-key]'))
    .filter((el) => el.dataset.sidebarPinnedWorkspace === workspaceHash);
  if (rows.length === 0) return null;

  const first = rows[0];
  const last = rows[rows.length - 1];
  const firstRect = first.getBoundingClientRect();
  const lastRect = last.getBoundingClientRect();
  if (clientY <= firstRect.top) {
    return {
      targetId: first.dataset.sidebarPinnedId || '',
      targetKey: first.dataset.sidebarPinnedKey || '',
      placement: 'before',
    };
  }
  if (clientY >= lastRect.bottom) {
    return {
      targetId: last.dataset.sidebarPinnedId || '',
      targetKey: last.dataset.sidebarPinnedKey || '',
      placement: 'after',
    };
  }

  for (const row of rows) {
    const rect = row.getBoundingClientRect();
    if (clientY >= rect.top && clientY <= rect.bottom) {
      return {
        targetId: row.dataset.sidebarPinnedId || '',
        targetKey: row.dataset.sidebarPinnedKey || '',
        placement: clientY < rect.top + rect.height / 2 ? 'before' : 'after',
      };
    }
  }
  return null;
}

function validateBooleanPreference(value) {
  return typeof value === 'boolean';
}

function hasDesktopBridge() {
  return typeof window.aceDesktop_listWorkspaces === 'function';
}

function hasDesktopRemoveWorkspace() {
  return typeof window.aceDesktop_removeWorkspace === 'function';
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
  if (state === 'in_progress') return { label: '进行中', dot: '' };
  if (state === 'unread') return { label: '未读', dot: 'bg-ok shadow-[0_0_4px_var(--ace-ok)]' };
  return { label: '已读', dot: 'bg-fg-mute/45' };
}

function countObjectKeys(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return 0;
  return Object.keys(value).length;
}

const LEGACY_CUSTOM_SIDEBAR_ICONS = {
  lightbulb: 'IntellisenseLightBulbSparkle',
  mcp: 'MCP',
  code: 'Code',
};

function CustomSidebarIcon({ icon }) {
  const file = LEGACY_CUSTOM_SIDEBAR_ICONS[icon];
  if (!file) return <VsIcon name={icon} size={16} />;
  return (
    <img
      src={`/vs-icons/${file}.svg`}
      alt=""
      width="16"
      height="16"
      className="ace-sidebar-custom-icon"
      draggable="false"
      aria-hidden="true"
      data-monochrome="true"
    />
  );
}

function CustomSidebarItem({ icon, label, count = null, warning = false, onClick }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className="w-full flex items-center gap-2 px-4 py-[6px] text-[12px] text-fg hover:bg-surface-hi transition text-left"
    >
      <CustomSidebarIcon icon={icon} />
      <span className="flex-1 min-w-0 truncate">{label}</span>
      {warning ? (
        <VsIcon name="warning" size={14} mono={false} className="shrink-0" title="未配置模型" />
      ) : count != null ? (
        <span className="text-[11px] text-fg-mute shrink-0 tabular-nums">{count}</span>
      ) : null}
    </button>
  );
}

function CustomSidebarSection({ activeRef, activeWorkspaceHash, onOpenSettingsSection }) {
  const [expanded, setExpanded] = usePreference(
    CUSTOM_SECTION_STORAGE_KEY,
    true,
    validateBooleanPreference,
  );
  const [counts, setCounts] = useState({ skills: null, mcp: null, models: null });

  const refreshCounts = useCallback(async () => {
    const [skills, mcp, models] = await Promise.allSettled([
      api.listSkills(),
      api.getMcp(),
      api.listModels(),
    ]);
    setCounts({
      skills: skills.status === 'fulfilled' && Array.isArray(skills.value) ? skills.value.length : null,
      mcp: mcp.status === 'fulfilled' ? countObjectKeys(mcp.value) : null,
      models: models.status === 'fulfilled' && Array.isArray(models.value) ? models.value.length : null,
    });
  }, []);

  useEffect(() => {
    refreshCounts().catch(() => {});
    const timer = setInterval(() => refreshCounts().catch(() => {}), 15000);
    return () => clearInterval(timer);
  }, [refreshCounts]);

  const openSkills = async () => {
    const workspaceHash =
      activeRef?.workspaceHash || activeRef?.workspace_hash || activeWorkspaceHash || '';
    try {
      const root = await api.getSkillRoot(workspaceHash);
      const path = root?.path || '';
      if (!path) throw new Error('empty path');
      if (typeof window.aceDesktop_openInExplorer === 'function') {
        const result = parseDesktopResult(await window.aceDesktop_openInExplorer(path));
        if (!result?.ok) throw new Error(result?.error || 'open failed');
        toast({ kind: 'ok', text: '已打开技能目录' });
        return;
      }
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(path);
        toast({ kind: 'ok', text: '技能目录路径已复制' });
      } else {
        toast({ kind: 'info', text: path });
      }
    } catch (e) {
      toast({ kind: 'err', text: '打开技能目录失败:' + (e?.message || '') });
    }
  };

  return (
    <div className="border-t border-border shrink-0 py-2">
      <button
        type="button"
        onClick={() => setExpanded((v) => !v)}
        className="w-full flex items-center px-4 py-1.5 text-[12px] text-fg-2 hover:text-fg transition"
      >
        <span className="flex-1 text-left">自定义</span>
        <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={12} />
      </button>
      {expanded && (
        <div className="pt-1">
          <CustomSidebarItem icon="lightbulb" label="技能" count={counts.skills} onClick={openSkills} />
          <CustomSidebarItem
            icon="mcp"
            label="MCP 服务器"
            count={counts.mcp}
            onClick={() => onOpenSettingsSection?.('mcp')}
          />
          <CustomSidebarItem
            icon="code"
            label="模型"
            count={counts.models && counts.models > 0 ? counts.models : null}
            warning={counts.models === 0}
            onClick={() => onOpenSettingsSection?.('models')}
          />
        </div>
      )}
    </div>
  );
}

function SessionRow({
  s,
  active,
  pinned = false,
  pendingQuestion = false,
  onSelect,
  onTogglePin,
  onArchive,
  onRename,
  onPinnedPointerDown,
  dragging = false,
  dropPlacement = '',
}) {
  const attention = s.attention_state || s.read_state || 'read';
  const meta = attentionMeta(attention);
  const workspaceHash = s.workspace_hash || s.workspaceHash || '';
  const rowKey = pinned ? pinnedSessionKey(workspaceHash, s.id) : '';
  const title = sessionDisplayTitle(s, s.name || '');
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(title);
  const committingRef = useRef(false);

  useEffect(() => {
    if (!editing) setDraft(title);
  }, [editing, title]);

  const commitRename = async () => {
    if (committingRef.current) return;
    committingRef.current = true;
    setEditing(false);
    const nextTitle = draft.trim();
    if (nextTitle === (s.title || '')) {
      setDraft(title);
      committingRef.current = false;
      return;
    }
    try {
      await onRename?.(s, nextTitle);
    } catch (e) {
      toast({ kind: 'err', text: '重命名失败:' + (e.message || '') });
      setDraft(title);
    } finally {
      committingRef.current = false;
    }
  };

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'session' || target.sessionId !== s.id) return;
      if (target.workspaceHash && workspaceHash && target.workspaceHash !== workspaceHash) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION) {
        detail.handled = true;
        onSelect?.(s);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION) {
        detail.handled = true;
        setDraft(title);
        setEditing(true);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION) {
        detail.handled = true;
        onArchive?.(s);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onArchive, onSelect, s, title, workspaceHash]);

  return (
    <div
      data-desktop-session-id={s.id || undefined}
      data-desktop-session-workspace={workspaceHash || undefined}
      data-desktop-session-pinned={pinned ? 'true' : 'false'}
      data-desktop-session-title={title || undefined}
      data-desktop-session-archive="true"
      data-sidebar-pinned-key={rowKey || undefined}
      data-sidebar-pinned-id={pinned ? s.id || undefined : undefined}
      data-sidebar-pinned-workspace={pinned ? workspaceHash || undefined : undefined}
      className={clsx(
        'ace-sidebar-session-row group flex items-center gap-1 mx-1.5 my-px pl-1 pr-2 rounded-md text-[12px] transition',
        pinned && 'ace-sidebar-pinned-session-row',
        dragging && 'is-dragging',
        dropPlacement === 'before' && 'is-drop-before',
        dropPlacement === 'after' && 'is-drop-after',
        active
          ? 'bg-accent-soft/50 text-accent'
          : 'text-fg hover:bg-surface-hi',
        attention === 'unread' && !active && 'font-semibold',
      )}
      onPointerDown={pinned ? (event) => onPinnedPointerDown?.(event, s, title) : undefined}
      onClick={pinned ? (event) => {
        if (event.defaultPrevented) return;
        if (event.target?.closest?.('[data-sidebar-row-control="true"], input, textarea, select')) return;
        onSelect?.(s);
      } : undefined}
    >
      <button
        type="button"
        data-sidebar-row-control="true"
        onClick={(e) => {
          e.preventDefault();
          e.stopPropagation();
          onTogglePin?.(s, !pinned);
        }}
        className={clsx(
          'ace-session-pin-btn w-5 h-6 rounded flex items-center justify-center shrink-0 transition-colors',
          // 未 pin: 默认软灰(text-fg-mute),hover 走 text-fg —— 亮色模式下
          // 颜色加深(灰→近黑),暗色模式下变亮(深灰→近白),符合"hover
          // 增强对比"的双向语义。
          // 已 pin: 用 text-accent(蓝)区别于普通可 hover 状态;它本身已经
          // 是激活态,不再做颜色 hover,避免大幅 hue 跳动。
          pinned
            ? 'opacity-100 text-accent'
            : 'opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 text-fg-mute hover:text-fg',
        )}
        title={pinned ? '取消置顶' : '置顶'}
        aria-label={pinned ? '取消置顶' : '置顶'}
      >
        <VsIcon name="pin" size={12} className="-rotate-45" />
      </button>
      {editing ? (
        <form
          className="flex flex-1 items-center gap-2 min-w-0 py-[3px]"
          onSubmit={(e) => { e.preventDefault(); commitRename(); }}
        >
          <input
            autoFocus
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            onBlur={commitRename}
            onKeyDown={(e) => {
              if (e.key === 'Escape') {
                e.preventDefault();
                setEditing(false);
                setDraft(title);
              }
            }}
            className="flex-1 min-w-0 h-6 px-1 rounded border border-accent bg-surface text-[12px] outline-none"
          />
        </form>
      ) : (
        <button
          type="button"
          onClick={(e) => { e.preventDefault(); onSelect(s); }}
          className="flex flex-1 items-center gap-2 min-w-0 py-[5px] bg-transparent text-left cursor-pointer"
        >
          {attention === 'in_progress' ? (
            <span className="ace-session-loading shrink-0" title={meta.label} aria-label={meta.label} />
          ) : (
            <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', meta.dot)} title={meta.label} />
          )}
          <span className="flex-1 min-w-0 truncate">{title}</span>
          {pendingQuestion && (
            <span
              className="shrink-0 rounded-full border border-ok-border bg-ok-bg px-2 py-[1px] text-[11px] font-medium leading-[18px] text-ok"
              title="等待用户回复 AskUserQuestion"
            >
              等待回复
            </span>
          )}
          <span className="text-[10px] text-fg-mute shrink-0">{relativeTime(s.updated_at || s.created_at)}</span>
        </button>
      )}
      <button
        type="button"
        data-sidebar-row-control="true"
        onClick={(e) => {
          e.preventDefault();
          e.stopPropagation();
          onArchive?.(s);
        }}
        className="w-5 h-6 rounded flex items-center justify-center shrink-0 text-fg-mute hover:text-fg hover:bg-surface-hi opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 transition"
        title="归档"
        aria-label="归档"
      >
        <VsIcon name="archive" size={14} />
      </button>
    </div>
  );
}

function WorkspaceGroup({
  ws,
  expanded,
  onToggle,
  sessions,
  sessionListExpanded,
  onToggleSessionList,
  activeId,
  onSelect,
  onRename,
  onActivate,
  onNewSession,
  onRemove,
  onTogglePin,
  onArchive,
  onRenameSession,
  pendingQuestionSessionIds,
  sessionsLoading = false,
}) {
  const [editing, setEditing] = useState(false);
  const [draft,   setDraft]   = useState(ws.name);
  const hasUnread = workspaceHasUnread(sessions);
  const projectedSessions = sidebarSessionProjection(sessions, sessionListExpanded);

  useEffect(() => {
    if (!editing) setDraft(ws.name);
  }, [editing, ws.name]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'workspace' || target.workspaceHash !== ws.hash) return;
      switch (action) {
        case DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE:
          detail.handled = true;
          if (!ws.active) onActivate?.(ws);
          break;
        case DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE:
          detail.handled = true;
          if (!expanded) onToggle?.(ws.hash);
          break;
        case DESKTOP_CONTEXT_ACTIONS.COLLAPSE_WORKSPACE:
          detail.handled = true;
          if (expanded) onToggle?.(ws.hash);
          break;
        case DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION:
          detail.handled = true;
          onNewSession?.(ws);
          break;
        case DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE:
          detail.handled = true;
          setEditing(true);
          break;
        case DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE:
          detail.handled = true;
          onRemove?.(ws);
          break;
        default:
          break;
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [expanded, onActivate, onNewSession, onRemove, onToggle, ws]);

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
        data-desktop-workspace-id={ws.hash || undefined}
        data-desktop-workspace-name={ws.name || undefined}
        data-desktop-workspace-path={ws.cwd || undefined}
        data-desktop-workspace-active={ws.active ? 'true' : 'false'}
        data-desktop-workspace-expanded={expanded ? 'true' : 'false'}
        data-desktop-workspace-rename="true"
        data-desktop-workspace-remove={onRemove ? 'true' : undefined}
        className={clsx(
          'group flex items-center gap-2 mx-1.5 px-2.5 py-[6px] rounded-md text-[12px] cursor-pointer transition',
          ws.active ? 'bg-accent-bg text-fg' : 'text-fg hover:bg-surface-hi',
        )}
        onClick={() => (ws.active ? onToggle(ws.hash) : onActivate(ws))}
      >
        <VsIcon name={expanded ? 'folderOpen' : 'folder'} size={14} />
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
          className="ace-sidebar-workspace-action w-5 h-5 rounded hover:bg-surface-hi flex items-center justify-center shrink-0 transition"
          title="新增会话"
        ><VsIcon name="editWindow" size={13} /></button>
        {hasUnread && <span className="w-1.5 h-1.5 rounded-full shrink-0 bg-ok shadow-[0_0_4px_var(--ace-ok)]" title="有未读会话" />}
        <button
          type="button"
          onClick={(e) => { e.stopPropagation(); setEditing(true); }}
          className="w-5 h-5 rounded text-fg-mute hover:text-fg hover:bg-surface-hi opacity-0 group-hover:opacity-100 flex items-center justify-center shrink-0 transition"
          title="重命名"
        ><VsIcon name="edit" size={12} /></button>
        {onRemove && (
          <button
            type="button"
            onClick={(e) => { e.stopPropagation(); onRemove(ws); }}
            className="w-5 h-5 rounded text-fg-mute hover:text-danger hover:bg-danger-bg opacity-0 group-hover:opacity-100 flex items-center justify-center shrink-0 transition"
            title="从桌面项目列表移除"
          ><VsIcon name="close" size={12} /></button>
        )}
      </div>
      {expanded && (
        <div className="my-1">
          {sessions.length === 0 ? (
            <div className="mx-1.5 ml-[22px] px-2 py-[3px] text-[11px] text-fg-mute italic">
              {sessionsLoading ? '加载中...' : '暂无对话'}
            </div>
          ) : (
            <>
              {projectedSessions.visibleSessions.map((s) => (
                <SessionRow
                  key={s.id}
                  s={s}
                  active={s.id === activeId}
                  pendingQuestion={sessionHasPendingQuestion(s, pendingQuestionSessionIds)}
                  onSelect={onSelect}
                  onTogglePin={onTogglePin}
                  onArchive={onArchive}
                  onRename={onRenameSession}
                />
              ))}
              {projectedSessions.collapsible && (
                <button
                  type="button"
                  onClick={() => onToggleSessionList?.(ws.hash)}
                  className="block w-[calc(100%-28px)] mx-1.5 ml-[22px] px-2 py-[5px] rounded-md text-left text-[12px] text-fg-mute hover:text-fg hover:bg-surface-hi transition"
                >
                  {projectedSessions.action === 'expand' ? '展开显示' : '折叠显示'}
                </button>
              )}
            </>
          )}
        </div>
      )}
    </div>
  );
}

export function Sidebar({
  activeId,
  activeRef,
  onSelect,
  collapsed,
  width = 200,
  onOpenHome,
  onOpenSettingsSection,
  pendingQuestionSessionIds = new Set(),
}) {
  const [workspaces,  setWorkspaces]  = useState([]);
  const [sessions,    setSessions]    = useState([]);
  const [statusBySession, setStatusBySession] = useState(() => new Map());
  const [pinnedByWorkspace, setPinnedByWorkspace] = useState(() => new Map());
  const [sessionLoadingWorkspaces, setSessionLoadingWorkspaces] = useState(() => new Set());
  const [sessionLoadedWorkspaces, setSessionLoadedWorkspaces] = useState(() => new Set());
  const [expanded,    setExpanded]    = useState(new Set());
  const [expandedSessionLists, setExpandedSessionLists] = useState(new Set());
  const [activeWorkspaceHash, setActiveWorkspaceHash] = useState('');
  const refreshingRef = useRef(false);
  const pendingRefreshHashRef = useRef('');
  const expandedRef = useRef(new Set());
  const pinnedByWorkspaceRef = useRef(new Map());
  const retainedSessionIdsRef = useRef(new Set());
  const sidebarScrollRef = useRef(null);
  const pinnedDragRef = useRef(null);
  const suppressPinnedClickRef = useRef(false);
  const [pinnedDragState, setPinnedDragState] = useState(null);
  const [pinnedDragGhost, setPinnedDragGhost] = useState(null);

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

  const applyPinnedReorder = useCallback(async ({ workspaceHash, sourceId, targetId, placement }) => {
    if (!workspaceHash || !sourceId || !targetId) return;
    const previous = normalizePinnedIds(pinnedByWorkspaceRef.current.get(workspaceHash) || []);
    const next = reorderPinnedSessionId(previous, sourceId, targetId, placement);
    if (sameStringArray(previous, next)) return;

    setPinnedWorkspaceIds(workspaceHash, next);
    try {
      const saved = await api.setPinnedSessions(workspaceHash, next);
      setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || next));
    } catch (e) {
      setPinnedWorkspaceIds(workspaceHash, previous);
      toast({ kind: 'err', text: '置顶排序失败:' + (e.message || '') });
    }
  }, [setPinnedWorkspaceIds]);

  const updatePinnedDragTarget = useCallback((clientY) => {
    const drag = pinnedDragRef.current;
    if (!drag) return;
    const target = pinnedDropTargetForPointer(clientY, drag.workspaceHash);
    if (!target) return;
    drag.targetId = target.targetId;
    drag.targetKey = target.targetKey;
    drag.placement = target.placement;
    setPinnedDragState({
      sourceKey: drag.sourceKey,
      targetKey: target.targetKey,
      placement: target.placement,
    });
  }, []);

  const updatePinnedDragScroll = useCallback((clientY) => {
    const scrollEl = sidebarScrollRef.current;
    if (!scrollEl) return;
    const rect = scrollEl.getBoundingClientRect();
    if (clientY < rect.top + PINNED_DRAG_EDGE_SCROLL_PX) {
      scrollEl.scrollTop -= PINNED_DRAG_EDGE_SCROLL_STEP;
    } else if (clientY > rect.bottom - PINNED_DRAG_EDGE_SCROLL_PX) {
      scrollEl.scrollTop += PINNED_DRAG_EDGE_SCROLL_STEP;
    }
  }, []);

  const finishPinnedDrag = useCallback((commit) => {
    const drag = pinnedDragRef.current;
    if (!drag) return;
    drag.cleanup?.();
    pinnedDragRef.current = null;
    document.body.classList.remove('ace-sidebar-pinned-reordering');
    setPinnedDragState(null);
    setPinnedDragGhost(null);
    if (drag.dragging) {
      suppressPinnedClickRef.current = true;
      window.setTimeout(() => {
        suppressPinnedClickRef.current = false;
      }, 80);
    }
    if (commit && drag.dragging) {
      applyPinnedReorder({
        workspaceHash: drag.workspaceHash,
        sourceId: drag.sourceId,
        targetId: drag.targetId,
        placement: drag.placement || 'before',
      });
    }
  }, [applyPinnedReorder]);

  const handlePinnedPointerDown = useCallback((event, session, title) => {
    if (event.button !== 0) return;
    if (pinnedDragRef.current) return;
    if (event.target?.closest?.('[data-sidebar-row-control="true"], input, textarea, select')) return;

    const workspaceHash = session?.workspace_hash || session?.workspaceHash || '';
    const sourceId = session?.id || session?.session_id || session?.sessionId || '';
    const sourceKey = pinnedSessionKey(workspaceHash, sourceId);
    if (!workspaceHash || !sourceId || !sourceKey) return;

    finishPinnedDrag(false);
    const pointerId = event.pointerId;
    const pointerTarget = event.currentTarget;
    const rect = pointerTarget.getBoundingClientRect();
    try {
      pointerTarget.setPointerCapture?.(pointerId);
    } catch {
      // Best effort; window-level listeners below still own cleanup.
    }

    const cleanup = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onCancel);
      try {
        pointerTarget.releasePointerCapture?.(pointerId);
      } catch {
        // The pointer can already be released if the row unmounted.
      }
    };

    const beginDragIfNeeded = (moveEvent) => {
      const drag = pinnedDragRef.current;
      if (!drag || drag.dragging) return true;
      const dx = moveEvent.clientX - drag.startX;
      const dy = moveEvent.clientY - drag.startY;
      if (Math.hypot(dx, dy) < PINNED_DRAG_START_PX) return false;
      drag.dragging = true;
      document.body.classList.add('ace-sidebar-pinned-reordering');
      setPinnedDragState({
        sourceKey: drag.sourceKey,
        targetKey: drag.sourceKey,
        placement: 'before',
      });
      return true;
    };

    function onMove(moveEvent) {
      const drag = pinnedDragRef.current;
      if (!drag || moveEvent.pointerId !== pointerId) return;
      if (!beginDragIfNeeded(moveEvent)) return;
      moveEvent.preventDefault();
      const nextY = moveEvent.clientY - drag.offsetY;
      setPinnedDragGhost({
        left: drag.rect.left,
        top: nextY,
        width: drag.rect.width,
        title: drag.title,
        timeText: drag.timeText,
      });
      updatePinnedDragScroll(moveEvent.clientY);
      updatePinnedDragTarget(moveEvent.clientY);
    }

    function onUp(upEvent) {
      const drag = pinnedDragRef.current;
      if (!drag || upEvent.pointerId !== pointerId) return;
      if (drag.dragging) upEvent.preventDefault();
      finishPinnedDrag(true);
    }

    function onCancel(cancelEvent) {
      const drag = pinnedDragRef.current;
      if (!drag || cancelEvent.pointerId !== pointerId) return;
      finishPinnedDrag(false);
    }

    pinnedDragRef.current = {
      workspaceHash,
      sourceId,
      sourceKey,
      targetId: sourceId,
      targetKey: sourceKey,
      placement: 'before',
      startX: event.clientX,
      startY: event.clientY,
      offsetY: event.clientY - rect.top,
      rect,
      title,
      timeText: relativeTime(session.updated_at || session.created_at),
      dragging: false,
      cleanup,
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onCancel);
  }, [finishPinnedDrag, updatePinnedDragScroll, updatePinnedDragTarget]);

  useEffect(() => () => {
    const drag = pinnedDragRef.current;
    drag?.cleanup?.();
    pinnedDragRef.current = null;
    document.body.classList.remove('ace-sidebar-pinned-reordering');
  }, []);

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

  const toggleSessionListExpanded = useCallback((hash) => {
    if (!hash) return;
    setExpandedSessionLists((prev) => {
      const next = new Set(prev);
      next.has(hash) ? next.delete(hash) : next.add(hash);
      return next;
    });
  }, []);

  const setSessionWorkspaceLoading = useCallback((hashes, loading) => {
    const normalized = Array.from(new Set(Array.from(hashes || []).filter(Boolean)));
    if (normalized.length === 0) return;
    setSessionLoadingWorkspaces((prev) => {
      const next = new Set(prev);
      for (const hash of normalized) {
        if (loading) next.add(hash);
        else next.delete(hash);
      }
      return next;
    });
  }, []);

  const setSessionWorkspacesLoaded = useCallback((hashes, loaded) => {
    const normalized = Array.from(new Set(Array.from(hashes || []).filter(Boolean)));
    if (normalized.length === 0) return;
    setSessionLoadedWorkspaces((prev) => {
      const next = new Set(prev);
      for (const hash of normalized) {
        if (loaded) next.add(hash);
        else next.delete(hash);
      }
      return next;
    });
  }, []);

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
    if (refreshingRef.current) {
      pendingRefreshHashRef.current = preferredHash || activeWorkspaceHash || pendingRefreshHashRef.current;
      return;
    }
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

      const availableHashes = new Set(workspaceArr.map((w) => w.hash).filter(Boolean));
      const chosen = (preferredHash && availableHashes.has(preferredHash))
        ? preferredHash
        : (workspaceArr.find((w) => w.active)?.hash || workspaceArr[0]?.hash || '');
      const withActive = workspaceArr.map((w) => ({ ...w, active: w.hash === chosen }));
      const expandedHashes = new Set(expandedRef.current);
      if (chosen) expandedHashes.add(chosen);
      setActiveWorkspaceHash(chosen);
      setWorkspaces(withActive);
      const earlyVisibleWorkspaceHashes = withActive
        .filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash))
        .map((w) => w.hash)
        .filter(Boolean);
      setSessionWorkspaceLoading(earlyVisibleWorkspaceHashes, true);

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

      const visibleWorkspaces = withActive.filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || pinnedWorkspaceHashes.has(w.hash));
      const visibleWorkspaceHashes = visibleWorkspaces.map((w) => w.hash).filter(Boolean);
      const visibleWorkspaceHashSet = new Set(visibleWorkspaceHashes);
      const hiddenWorkspaceHashes = withActive
        .map((w) => w.hash)
        .filter((hash) => hash && !visibleWorkspaceHashSet.has(hash));
      setSessionWorkspacesLoaded(hiddenWorkspaceHashes, false);
      setSessionWorkspaceLoading(hiddenWorkspaceHashes, false);
      setSessionWorkspaceLoading(visibleWorkspaceHashes, true);
      try {
        const perWorkspace = await Promise.all(visibleWorkspaces.map(async (w) => {
          if (w.hash === '__local__') {
            const list = await api.listSessions();
            return (Array.isArray(list) ? list : []).map((s) => ({ ...s, workspace_hash: s.workspace_hash || w.hash, cwd: s.cwd || w.cwd }));
          }
          const list = await api.listWorkspaceSessions(w.hash);
          return (Array.isArray(list) ? list : []).map((s) => ({ ...s, workspace_hash: s.workspace_hash || w.hash, cwd: s.cwd || w.cwd }));
        }));
        const incoming = perWorkspace.flat();
        setSessions((prev) => reconcileSidebarSessions(prev, incoming));
        setStatusBySession((prev) => incoming.reduce((map, s) => applyStatusUpdate(map, {
          ...s,
          session_id: s.id,
          state: s.attention_state || s.read_state,
          cursor: s.status_cursor,
        }), prev));
        syncRetainedSessionIds(incoming.filter((s) => s.active && s.id).map((s) => s.id));
      }
      catch { /* 鉴权失败不致命 */ }
      finally {
        setSessionWorkspacesLoaded(visibleWorkspaceHashes, true);
        setSessionWorkspaceLoading(visibleWorkspaceHashes, false);
      }
    } finally {
      refreshingRef.current = false;
      const pendingHash = pendingRefreshHashRef.current;
      pendingRefreshHashRef.current = '';
      if (pendingHash) setTimeout(() => refresh(pendingHash).catch(() => {}), 0);
    }
  }, [activeWorkspaceHash, setPinnedMap, setSessionWorkspaceLoading, setSessionWorkspacesLoaded, syncRetainedSessionIds, updateExpanded]);

  const archiveSession = useCallback(async (session) => {
    const id = session?.id || session?.sessionId || session?.session_id || '';
    const workspaceHash = session?.workspace_hash || session?.workspaceHash || activeWorkspaceHash || '';
    if (!id) return;

    try {
      if (workspaceHash && workspaceHash !== '__local__') {
        await api.archiveWorkspaceSession(workspaceHash, id);
      } else {
        await api.archiveSession(id);
      }

      const previousPinned = normalizePinnedIds(pinnedByWorkspaceRef.current.get(workspaceHash) || []);
      if (workspaceHash && previousPinned.includes(id)) {
        const nextPinned = unpinSessionId(previousPinned, id);
        setPinnedWorkspaceIds(workspaceHash, nextPinned);
        try {
          const saved = await api.setPinnedSessions(workspaceHash, nextPinned);
          setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || nextPinned));
        } catch {
          // Archiving already succeeded; stale pinned state will be pruned on next refresh.
        }
      }

      setSessions((prev) => prev.filter((item) => (item.id || item.session_id || item.sessionId) !== id));
      if (id === activeId) {
        onOpenHome?.({
          hash: workspaceHash,
          workspaceHash,
          cwd: session.cwd || '',
          name: session.workspaceName || session.workspace_name || '',
        });
      }
      toast({ kind: 'ok', text: '已归档' });
      window.dispatchEvent(new Event('ace-session-archive-changed'));
      refresh(workspaceHash).catch(() => {});
    } catch (e) {
      toast({ kind: 'err', text: '归档失败:' + (e.message || '') });
    }
  }, [activeId, activeWorkspaceHash, onOpenHome, refresh, setPinnedWorkspaceIds]);

  const renameSession = useCallback(async (session, title) => {
    const id = session?.id || session?.sessionId || session?.session_id || '';
    const workspaceHash = session?.workspace_hash || session?.workspaceHash || activeWorkspaceHash || '';
    if (!id) return;

    const updated = workspaceHash && workspaceHash !== '__local__'
      ? await api.setSessionTitle(id, title, workspaceHash)
      : await api.setSessionTitle(id, title);
    const nextTitle = updated?.title ?? title;
    const nextSource = updated?.title_source ?? (nextTitle ? 'user' : '');
    setSessions((prev) => prev.map((item) => {
      const itemId = item.id || item.session_id || item.sessionId;
      const itemWorkspace = item.workspace_hash || item.workspaceHash || '';
      if (itemId !== id) return item;
      if (workspaceHash && itemWorkspace && itemWorkspace !== workspaceHash) return item;
      return {
        ...item,
        ...updated,
        id,
        title: nextTitle,
        title_source: nextSource,
        workspace_hash: updated?.workspace_hash || workspaceHash || itemWorkspace,
      };
    }));
    toast({ kind: 'ok', text: title.trim() ? '已重命名' : '已清除标题' });
    refresh(workspaceHash).catch(() => {});
  }, [activeWorkspaceHash, refresh]);

  useEffect(() => {
    refresh();
    const t = setInterval(() => refresh().catch(() => {}), 5000);
    return () => clearInterval(t);
  }, [refresh]);

  useEffect(() => {
    const handler = () => refresh().catch(() => {});
    window.addEventListener('ace-session-archive-changed', handler);
    return () => window.removeEventListener('ace-session-archive-changed', handler);
  }, [refresh]);

  useEffect(() => {
    const handler = (event) => {
      const detail = normalizeSessionListChangedDetail(event.detail || {});
      const workspaceHash = detail.workspaceHash || activeWorkspaceHash;
      if (workspaceHash) {
        updateExpanded((prev) => new Set(prev).add(workspaceHash));
        connection.subscribeWorkspaceStatus(workspaceHash);
      }
      if (detail.session) {
        const session = {
          ...detail.session,
          workspace_hash: detail.session.workspace_hash || detail.session.workspaceHash || workspaceHash,
        };
        setSessions((prev) => upsertSidebarSession(prev, session));
        setStatusBySession((prev) => applyStatusUpdate(prev, {
          ...session,
          session_id: session.id || detail.sessionId,
          state: session.attention_state || session.read_state || 'read',
        }));
      }
      refresh(workspaceHash).catch(() => {});
    };
    window.addEventListener(SESSION_LIST_CHANGED_EVENT, handler);
    return () => window.removeEventListener(SESSION_LIST_CHANGED_EVENT, handler);
  }, [activeWorkspaceHash, refresh, updateExpanded]);

  const renderedSessions = useMemo(
    () => withNewSessionDisplayTitles(mergeSessionsWithStatus(sessions, statusBySession)),
    [sessions, statusBySession],
  );

  // 把 active workspace 的 sessions / pinned ids / workspaceName 推到桌面 tray 菜单。
  // pushTrayMenu 内部 100ms debounce + 无 bridge 时 no-op。
  // 设计:openspec/changes/enhance-desktop-tray-menu。
  useEffect(() => {
    const activeWs = workspaces.find((w) => w.hash === activeWorkspaceHash);
    const activeSessions = activeWorkspaceHash
      ? renderedSessions.filter((s) => (s.workspace_hash || s.workspaceHash) === activeWorkspaceHash)
      : [];
    const pinnedIds = activeWorkspaceHash
      ? normalizePinnedIds(pinnedByWorkspace.get(activeWorkspaceHash) || [])
      : [];
    pushTrayMenu({
      sessions: activeSessions,
      pinnedSessionIds: pinnedIds,
      workspaceName: activeWs?.name || '',
    });
  }, [renderedSessions, pinnedByWorkspace, activeWorkspaceHash, workspaces]);

  useEffect(() => {
    const handler = (e) => {
      const msg = e.detail || {};
      if (msg.type === 'session_status_snapshot') {
        setStatusBySession((prev) => applyStatusSnapshot(prev, msg.payload || {}));
      } else if (msg.type === 'session_status' || msg.type === 'mark_session_read_ack') {
        setStatusBySession((prev) => applyStatusUpdate(prev, msg.payload || {}));
      } else if (msg.type === 'session_updated') {
        const payload = msg.payload || {};
        const sid = payload.session_id || msg.session_id || '';
        if (!sid) return;
        setSessions((prev) => prev.map((session) => {
          if ((session.id || session.session_id || session.sessionId) !== sid) return session;
          return {
            ...session,
            title: Object.prototype.hasOwnProperty.call(payload, 'title')
              ? (payload.title || '')
              : session.title,
            title_source: payload.title_source || session.title_source || '',
            workspace_hash: payload.workspace_hash || session.workspace_hash,
            cwd: payload.cwd || session.cwd,
          };
        }));
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
    if (!hash) return;
    const next = new Set(expandedRef.current);
    const willExpand = !next.has(hash);
    if (willExpand) next.add(hash);
    else next.delete(hash);
    expandedRef.current = next;
    setExpanded(next);
    if (willExpand) {
      setSessionWorkspaceLoading([hash], true);
      refresh(hash).catch(() => {});
    }
  };

  const onActivate = async (ws) => {
    setSessionWorkspaceLoading([ws.hash], true);
    setActiveWorkspaceHash(ws.hash);
    updateExpanded((prev) => new Set(prev).add(ws.hash));
    if (!hasDesktopBridge()) {
      setWorkspaces((prev) => prev.map((item) => ({ ...item, active: item.hash === ws.hash })));
      onOpenHome?.(ws);
      refresh(ws.hash).catch(() => {});
      return;
    }
    try {
      const r = parseDesktopResult(await window.aceDesktop_activateWorkspace(ws.hash));
      if (r.error) { toast({ kind: 'err', text: '切换失败:' + r.error }); return; }
      const currentPort = Number(location.port || (location.protocol === 'https:' ? 443 : 80));
      if (r.port && Number(r.port) !== currentPort) {
        location.href = `http://127.0.0.1:${r.port}/?token=${encodeURIComponent(r.token)}`;
      } else {
        onOpenHome?.({ ...ws, port: r.port || ws.port, token: r.token || ws.token });
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
              displayTitle: session.displayTitle || session.display_title,
              port: r.port,
              token: r.token,
              cwd: r.cwd || ws.cwd,
              title: session.title,
              summary: session.summary,
              provider: session.provider,
              model: session.model,
              model_name: session.model_name,
              model_preset: session.model_preset,
              context_window: session.context_window,
              deleted: session.deleted || session.model_deleted || session.modelDeleted || false,
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
      displayTitle: session.displayTitle || session.display_title,
      port: ws?.port,
      token: ws?.token,
      cwd: session.cwd || ws?.cwd,
      title: session.title,
      summary: session.summary,
      provider: session.provider,
      model: session.model,
      model_name: session.model_name,
      model_preset: session.model_preset,
      context_window: session.context_window,
      deleted: session.deleted || session.model_deleted || session.modelDeleted || false,
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

  const removeWorkspace = async (ws) => {
    if (!ws?.hash) return;
    if (!hasDesktopRemoveWorkspace()) {
      toast({ kind: 'info', text: '需在 desktop shell 中使用' });
      return;
    }
    try {
      const r = parseDesktopResult(await window.aceDesktop_removeWorkspace(ws.hash));
      if (!r?.ok) throw new Error(r?.error || 'remove failed');

      const remaining = workspaces.filter((w) => w.hash !== ws.hash);
      const nextHash = r.active_workspace_hash
        || ((ws.active || activeWorkspaceHash === ws.hash) ? (remaining[0]?.hash || '') : activeWorkspaceHash);

      setWorkspaces(remaining.map((w) => ({ ...w, active: w.hash === nextHash })));
      setSessions((prev) => prev.filter((s) => (s.workspace_hash || s.workspaceHash || '') !== ws.hash));
      setPinnedMap((prev) => {
        const next = new Map(prev);
        next.delete(ws.hash);
        return next;
      });
      updateExpanded((prev) => {
        const next = new Set(prev);
        next.delete(ws.hash);
        if (nextHash) next.add(nextHash);
        return next;
      });
      setActiveWorkspaceHash(nextHash);
      toast({ kind: 'ok', text: '已从桌面项目列表移除' });
      await refresh(nextHash);
    } catch (e) {
      toast({ kind: 'err', text: '移除项目失败:' + (e.message || '') });
    }
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
      const decoratedNextSession =
        withNewSessionDisplayTitles([...sessions.filter((item) => item.id !== id), nextSession])
          .find((item) => item.id === id) || nextSession;

      setActiveWorkspaceHash(workspaceHash);
      updateExpanded((prev) => new Set(prev).add(workspaceHash));
      setWorkspaces((prev) => prev.map((item) => ({ ...item, active: item.hash === workspaceHash })));
      setSessions((prev) => [decoratedNextSession, ...prev.filter((item) => item.id !== id)]);
      setStatusBySession((prev) => applyStatusUpdate(prev, { ...decoratedNextSession, session_id: id, state: 'read' }));
      connection.subscribeWorkspaceStatus(workspaceHash);
      syncRetainedSessionIds(new Set([...retainedSessionIdsRef.current, id]));
      markSessionRead(decoratedNextSession);
      onSelect?.({
        workspaceHash,
        contextId: 'default',
        sessionId: id,
        displayTitle: decoratedNextSession.displayTitle || decoratedNextSession.display_title,
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
    try {
      const ws = hasDesktopBridge()
        ? parseDesktopResult(await window.aceDesktop_addWorkspace())
        : await api.pickWorkspaceFolder();
      if (ws == null) return;
      if (!ws || !ws.hash) return;
      if (hasDesktopBridge()) {
        try { await api.registerWorkspace(ws.cwd); } catch { /* daemon 可能已入册;忽略 */ }
      }
      await refresh(ws.hash);
      await onActivate(ws);
    } catch (e) {
      if (!hasDesktopBridge() && (e.status === 404 || e.status === 501)) {
        toast({ kind: 'info', text: '需在 desktop webapp 中使用' });
      } else {
        toast({ kind: 'err', text: '添加项目失败:' + (e.message || '') });
      }
    }
  };

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
        <div className="flex-1 flex flex-col min-h-0">
          <div className="flex items-center justify-between px-2 pt-2.5 pb-1 text-[10px] font-semibold uppercase tracking-wider text-fg-mute">
            <span>项目</span>
            <button
              type="button"
              onClick={onAddWorkspace}
              className="w-5 h-5 rounded text-fg-mute hover:text-fg hover:bg-surface-hi text-[14px] leading-none flex items-center justify-center"
              title="添加项目"
            ><VsIcon name="folderAdd" size={15} /></button>
          </div>
          <div ref={sidebarScrollRef} className="flex-1 overflow-y-auto pb-2">
            {pinnedSessions.length > 0 && (
              <div className="mb-2">
                <div className="px-4 pt-1 pb-1 text-[10px] font-semibold uppercase tracking-wider text-fg-mute">置顶</div>
                <div className="my-1">
                  {pinnedSessions.map((s) => {
                    const rowKey = pinnedSessionKey(s.workspace_hash || s.workspaceHash || '', s.id);
                    return (
                      <SessionRow
                        key={`pinned-${s.workspace_hash || ''}-${s.id}`}
                        s={s}
                        pinned
                        active={s.id === activeId}
                        pendingQuestion={sessionHasPendingQuestion(s, pendingQuestionSessionIds)}
                        dragging={pinnedDragState?.sourceKey === rowKey}
                        dropPlacement={pinnedDragState?.targetKey === rowKey ? pinnedDragState.placement : ''}
                        onPinnedPointerDown={handlePinnedPointerDown}
                        onSelect={(session) => {
                          if (suppressPinnedClickRef.current) return;
                          selectSession(workspaceForSession(session), session);
                        }}
                        onTogglePin={togglePinnedSession}
                        onArchive={archiveSession}
                        onRename={renameSession}
                      />
                    );
                  })}
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
                  sessionListExpanded={expandedSessionLists.has(ws.hash)}
                  onToggleSessionList={toggleSessionListExpanded}
                  activeId={activeId}
                  onSelect={(session) => selectSession(ws, session)}
                  onRename={onRename}
                  onActivate={onActivate}
                  onNewSession={createSessionInWorkspace}
                  onRemove={hasDesktopRemoveWorkspace() ? removeWorkspace : undefined}
                  onTogglePin={togglePinnedSession}
                  onArchive={archiveSession}
                  onRenameSession={renameSession}
                  pendingQuestionSessionIds={pendingQuestionSessionIds}
                  sessionsLoading={sessionLoadingWorkspaces.has(ws.hash) || !sessionLoadedWorkspaces.has(ws.hash)}
                />
              );
            })}
          </div>
          {pinnedDragGhost && (
            <div
              className="ace-sidebar-pinned-drag-ghost"
              style={{
                left: pinnedDragGhost.left,
                top: pinnedDragGhost.top,
                width: pinnedDragGhost.width,
              }}
            >
              <span className="w-1.5 h-1.5 rounded-full shrink-0 bg-fg-mute/45" />
              <span className="flex-1 min-w-0 truncate">{pinnedDragGhost.title}</span>
              <span className="text-[10px] text-fg-mute shrink-0">{pinnedDragGhost.timeText}</span>
            </div>
          )}
        </div>
        <CustomSidebarSection
          activeRef={activeRef}
          activeWorkspaceHash={activeWorkspaceHash}
          onOpenSettingsSection={onOpenSettingsSection}
        />
      </div>
    </aside>
  );
}
