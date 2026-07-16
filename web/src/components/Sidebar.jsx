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
  normalizePinnedOrderItems,
  pinSessionId,
  pinPinnedOrderItem,
  pinnedSessionsForList,
  reorderPinnedOrderItems,
  reorderPinnedSessionId,
  unpinPinnedOrderItem,
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
import { pickExistingWorkspace } from '../lib/workspacePicker.js';
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
  sessionListNeedsRevealExpansion,
  sessionMatchesRevealTarget,
  sidebarSessionHasWorktree,
  sidebarRevealTarget,
  sidebarSessionProjection,
  upsertSidebarSession,
} from '../lib/sidebarSessions.js';
import {
  DEFAULT_SIDEBAR_CUSTOM_EXPANDED,
  DEFAULT_SIDEBAR_SECTION_EXPANSION,
  SIDEBAR_CUSTOM_ITEMS,
  SIDEBAR_DISCLOSURE_ICON,
  SIDEBAR_NAV_ITEMS,
  SIDEBAR_SECTION_IDS,
  sidebarCustomMaxCount,
  sidebarSectionCounts,
  sidebarSectionIsVisible,
  sidebarSectionTitle,
  validateSidebarSectionExpansion,
} from '../lib/sidebarNavigation.js';
import {
  OPENCODE_IMPORT_HIGHLIGHT_MS,
  defaultOpencodeImportSelection,
  normalizeOpencodeImportPreview,
  opencodeImportedSessionHighlightKey,
  opencodeImportedSessionHighlightKeys,
  opencodeImportConfirmationText,
  opencodeImportProgress,
  toggleAllOpencodeImportSelection,
} from '../lib/opencodeImport.js';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

const SIDEBAR_SECTIONS_STORAGE_KEY = 'acecode.sidebarSectionsExpanded.v1';
const SIDEBAR_CUSTOM_STORAGE_KEY = 'acecode.sidebarCustomSectionExpanded.v2';
const PINNED_DRAG_START_PX = 5;
const PINNED_DRAG_EDGE_SCROLL_PX = 34;
const PINNED_DRAG_EDGE_SCROLL_STEP = 16;
const NO_WORKSPACE_SESSION_LIST_KEY = '__no_workspace__';

function pinnedSessionKey(workspaceHash, sessionId) {
  const ws = String(workspaceHash || '');
  const id = String(sessionId || '');
  return ws && id ? `${ws}\u0000${id}` : '';
}

function isNoWorkspaceSession(session) {
  return !!(session?.noWorkspace || session?.no_workspace);
}

function normalizeNoWorkspaceSession(session = {}) {
  return {
    ...session,
    noWorkspace: true,
    no_workspace: true,
    workspace_hash: '',
    workspaceHash: '',
    cwd: '',
  };
}

function normalizeWorkspaceSession(session = {}, workspace = {}) {
  if (isNoWorkspaceSession(session)) return normalizeNoWorkspaceSession(session);
  return {
    ...session,
    workspace_hash: session.workspace_hash || session.workspaceHash || workspace.hash || '',
    cwd: session.cwd || workspace.cwd || '',
  };
}

function sameStringArray(a = [], b = []) {
  if (a.length !== b.length) return false;
  return a.every((value, index) => value === b[index]);
}

function samePinnedOrderItems(a = [], b = []) {
  const left = normalizePinnedOrderItems(a);
  const right = normalizePinnedOrderItems(b);
  if (left.length !== right.length) return false;
  return left.every((value, index) => (
    value.workspace_hash === right[index].workspace_hash &&
    value.session_id === right[index].session_id
  ));
}

function pinnedDropTargetForPointer(clientY) {
  if (typeof document === 'undefined') return null;
  const rows = Array.from(document.querySelectorAll('[data-sidebar-pinned-key]'));
  if (rows.length === 0) return null;

  const first = rows[0];
  const last = rows[rows.length - 1];
  const firstRect = first.getBoundingClientRect();
  const lastRect = last.getBoundingClientRect();
  if (clientY <= firstRect.top) {
    return {
      targetWorkspaceHash: first.dataset.sidebarPinnedWorkspace || '',
      targetId: first.dataset.sidebarPinnedId || '',
      targetKey: first.dataset.sidebarPinnedKey || '',
      placement: 'before',
    };
  }
  if (clientY >= lastRect.bottom) {
    return {
      targetWorkspaceHash: last.dataset.sidebarPinnedWorkspace || '',
      targetId: last.dataset.sidebarPinnedId || '',
      targetKey: last.dataset.sidebarPinnedKey || '',
      placement: 'after',
    };
  }

  for (const row of rows) {
    const rect = row.getBoundingClientRect();
    if (clientY >= rect.top && clientY <= rect.bottom) {
      return {
        targetWorkspaceHash: row.dataset.sidebarPinnedWorkspace || '',
        targetId: row.dataset.sidebarPinnedId || '',
        targetKey: row.dataset.sidebarPinnedKey || '',
        placement: clientY < rect.top + rect.height / 2 ? 'before' : 'after',
      };
    }
  }
  return null;
}

function pinnedOrderItemsFromDom() {
  if (typeof document === 'undefined') return [];
  return normalizePinnedOrderItems(Array.from(document.querySelectorAll('[data-sidebar-pinned-key]')).map((row) => ({
    workspace_hash: row.dataset.sidebarPinnedWorkspace || '',
    session_id: row.dataset.sidebarPinnedId || '',
  })));
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

function SidebarDisclosure({ expanded, className = '' }) {
  const icon = SIDEBAR_DISCLOSURE_ICON;
  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      width={icon.width}
      height={icon.height}
      viewBox={icon.viewBox}
      fill="none"
      className={clsx('block shrink-0 transition-transform', className)}
      style={{ transform: expanded ? 'rotate(0deg)' : 'rotate(-90deg)' }}
      aria-hidden="true"
    >
      <path
        d={icon.path}
        stroke={icon.stroke}
        strokeWidth={icon.strokeWidth}
        strokeLinecap={icon.strokeLinecap}
        strokeLinejoin={icon.strokeLinejoin}
      />
    </svg>
  );
}

function SidebarNavItem({ item, onClick }) {
  return (
    <button
      type="button"
      data-tour-target={item.id === 'new-task' ? 'sidebar-new-task' : undefined}
      onClick={onClick}
      className="ace-sidebar-primary-text w-full flex items-center gap-[7px] px-3 py-[3px] rounded-md text-[14px] text-fg hover:bg-surface-hi transition text-left"
    >
      <span className="w-6 h-6 flex items-center justify-center shrink-0">
        <VsIcon name={item.icon} size={16} />
      </span>
      <span className="flex-1 min-w-0 truncate">{item.label}</span>
    </button>
  );
}

function validateBooleanPreference(value) {
  return typeof value === 'boolean';
}

function countObjectKeys(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return 0;
  return Object.keys(value).length;
}

const CUSTOM_SIDEBAR_ICON_FILES = Object.freeze({
  lightbulb: 'IntellisenseLightBulbSparkle',
  mcp: 'MCP',
  code: 'Code',
});

function CustomSidebarIcon({ icon }) {
  const file = CUSTOM_SIDEBAR_ICON_FILES[icon];
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

function CustomSidebarItem({ item, count, onClick }) {
  return (
    <button
      type="button"
      onClick={onClick}
      data-sidebar-custom-item={item.id}
      className="ace-sidebar-primary-text w-full flex items-center gap-[5px] px-3 py-[3px] text-[14px] text-fg hover:bg-surface-hi transition text-left"
    >
      <span className="w-6 h-6 flex items-center justify-center shrink-0">
        <CustomSidebarIcon icon={item.icon} />
      </span>
      <span className="flex-1 min-w-0 truncate">{item.label}</span>
      {Number.isFinite(count) && (
        <span className="text-[11px] text-fg-mute shrink-0 tabular-nums">{count}</span>
      )}
    </button>
  );
}

function CustomSidebarSection({ onOpenSettingsSection }) {
  const [expanded, setExpanded] = usePreference(
    SIDEBAR_CUSTOM_STORAGE_KEY,
    DEFAULT_SIDEBAR_CUSTOM_EXPANDED,
    validateBooleanPreference,
  );
  const [counts, setCounts] = useState({ skills: null, mcp: null, models: null });

  const refreshCounts = useCallback(async () => {
    const [skills, mcp, models] = await Promise.allSettled([
      api.listSkills(),
      api.getMcp(),
      api.listModels(),
    ]);
    setCounts((previous) => ({
      skills: skills.status === 'fulfilled' && Array.isArray(skills.value)
        ? skills.value.length
        : previous.skills,
      mcp: mcp.status === 'fulfilled' ? countObjectKeys(mcp.value) : previous.mcp,
      models: models.status === 'fulfilled' && Array.isArray(models.value)
        ? models.value.length
        : previous.models,
    }));
  }, []);

  useEffect(() => {
    refreshCounts().catch(() => {});
    const timer = window.setInterval(() => refreshCounts().catch(() => {}), 15000);
    return () => window.clearInterval(timer);
  }, [refreshCounts]);

  const maxCount = sidebarCustomMaxCount(counts);
  return (
    <div className="ace-sidebar-custom-section border-t border-border shrink-0 py-2">
      <button
        type="button"
        onClick={() => setExpanded((value) => !value)}
        data-sidebar-custom-section="true"
        className="w-full flex items-center px-3 py-1.5 text-[13px] text-fg-2 hover:text-fg transition"
        aria-expanded={expanded}
      >
        <span className="flex-1 min-w-0 text-left truncate">自定义</span>
        {maxCount != null && (
          <span className="mr-2 shrink-0 tabular-nums text-fg-mute">{maxCount}</span>
        )}
        <SidebarDisclosure expanded={expanded} />
      </button>
      {expanded && (
        <div className="ace-sidebar-custom-list pt-1">
          {SIDEBAR_CUSTOM_ITEMS.map((item) => (
            <CustomSidebarItem
              key={item.id}
              item={item}
              count={counts[item.id]}
              onClick={() => onOpenSettingsSection?.(item.settingsSection)}
            />
          ))}
        </div>
      )}
    </div>
  );
}

function SidebarSectionHeader({ sectionId, count, expanded, onToggle, actions = null }) {
  if (!sidebarSectionIsVisible(count)) return null;
  const title = sidebarSectionTitle(sectionId, count);
  return (
    <div
      data-sidebar-section={sectionId}
      className="ace-sidebar-section-text flex items-center gap-0.5 px-3 pt-3 pb-1 text-[13px] font-medium text-fg-mute"
    >
      <button
        type="button"
        onClick={onToggle}
        className="min-w-0 text-left hover:text-fg transition"
        aria-expanded={expanded}
      >
        <span className="block truncate">{title}</span>
      </button>
      <button
        type="button"
        onClick={onToggle}
        data-sidebar-section-disclosure={sectionId}
        className="w-5 h-6 rounded flex items-center justify-center shrink-0 hover:text-fg hover:bg-surface-hi transition"
        title={expanded ? `折叠${title}` : `展开${title}`}
        aria-label={expanded ? `折叠${title}` : `展开${title}`}
        aria-expanded={expanded}
      >
        <SidebarDisclosure expanded={expanded} />
      </button>
      {actions && (
        <span data-sidebar-section-actions={sectionId} className="ml-auto flex items-center shrink-0">
          {actions}
        </span>
      )}
    </div>
  );
}

function SessionAttentionIndicator({ attention, meta }) {
  return attention === 'in_progress' ? (
    <span className="ace-session-loading shrink-0" title={meta.label} aria-label={meta.label} />
  ) : (
    <span className={clsx('w-1.5 h-1.5 rounded-full shrink-0', meta.dot)} title={meta.label} />
  );
}

function SessionRow({
  s,
  active,
  pinned = false,
  pinEnabled = true,
  pendingQuestion = false,
  onSelect,
  onTogglePin,
  onArchive,
  onRename,
  onPinnedPointerDown,
  dragging = false,
  dropPlacement = '',
  importHighlighted = false,
}) {
  const attention = s.attention_state || s.read_state || 'read';
  const meta = attentionMeta(attention);
  const workspaceHash = s.workspace_hash || s.workspaceHash || '';
  const rowKey = pinned ? pinnedSessionKey(workspaceHash, s.id) : '';
  const title = sessionDisplayTitle(s, s.name || '');
  const worktreeSession = sidebarSessionHasWorktree(s);
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
        'ace-sidebar-session-row ace-sidebar-tree-row-grid ace-sidebar-primary-text group grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 my-px px-2 rounded-md text-[14px] transition',
        pinned && 'ace-sidebar-pinned-session-row',
        dragging && 'is-dragging',
        dropPlacement === 'before' && 'is-drop-before',
        dropPlacement === 'after' && 'is-drop-after',
        !pinned && importHighlighted && 'is-opencode-import-highlight',
        active
          ? 'bg-accent-soft/50 text-accent'
          : 'text-fg hover:bg-surface-hi',
        attention === 'unread' && !active && 'font-semibold',
      )}
      onPointerDown={pinned ? (event) => onPinnedPointerDown?.(event, s, title) : undefined}
      onClick={(event) => {
        if (event.defaultPrevented) return;
        if (event.target?.closest?.('[data-sidebar-row-control="true"], input, textarea, select')) return;
        onSelect?.(s);
      }}
    >
      <span className="relative flex w-6 h-7 items-center justify-center shrink-0">
        {pinned && pinEnabled ? (
          <>
            <button
              type="button"
              data-sidebar-row-control="true"
              onClick={(e) => {
                e.preventDefault();
                e.stopPropagation();
                onTogglePin?.(s, false);
              }}
              className="ace-session-pin-btn flex h-7 w-3 items-center justify-center text-accent"
              title="取消置顶"
              aria-label="取消置顶"
            >
              <VsIcon name="pin" size={12} className="-rotate-45" />
            </button>
            <span className="flex h-7 w-3 items-center justify-center">
              <SessionAttentionIndicator attention={attention} meta={meta} />
            </span>
          </>
        ) : (
          <>
            <span
              className={clsx(
                'absolute inset-0 flex items-center justify-center transition-opacity',
                pinEnabled && 'group-hover:opacity-0 group-focus-within:opacity-0',
              )}
            >
              <SessionAttentionIndicator attention={attention} meta={meta} />
            </span>
            {pinEnabled && (
              <button
                type="button"
                data-sidebar-row-control="true"
                onClick={(e) => {
                  e.preventDefault();
                  e.stopPropagation();
                  onTogglePin?.(s, true);
                }}
                className="ace-session-pin-btn absolute inset-0 rounded flex items-center justify-center opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 text-fg-mute hover:text-fg transition-colors"
                title="置顶"
                aria-label="置顶"
              >
                <VsIcon name="pin" size={12} className="-rotate-45" />
              </button>
            )}
          </>
        )}
      </span>
      {editing ? (
        <form
          className="flex items-center min-w-0 py-[3px]"
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
            className="ace-sidebar-primary-text flex-1 min-w-0 h-7 px-1 rounded border border-accent bg-surface text-[14px] outline-none"
          />
        </form>
      ) : (
        <button
          type="button"
          onClick={(e) => { e.preventDefault(); e.stopPropagation(); onSelect(s); }}
          className="min-w-0 py-[5px] bg-transparent text-left cursor-pointer"
        >
          <span className="block min-w-0 truncate">{title}</span>
        </button>
      )}
      <span className="flex min-w-0 items-center justify-end gap-1">
        {!editing && pendingQuestion && (
          <span
            className="shrink-0 rounded-full border border-ok-border bg-ok-bg px-2 py-[1px] text-[11px] font-medium leading-[18px] text-ok"
            title="等待用户回复 AskUserQuestion"
          >
            等待回复
          </span>
        )}
        {!editing && (
          <span className="ace-sidebar-meta-text text-[13px] text-fg-mute shrink-0">{relativeTime(s.updated_at || s.created_at)}</span>
        )}
        <button
          type="button"
          data-sidebar-row-control="true"
          onClick={(e) => {
            e.preventDefault();
            e.stopPropagation();
            onArchive?.(s);
          }}
          className={clsx(
            'w-5 h-7 rounded flex items-center justify-center shrink-0 text-fg-mute hover:text-fg hover:bg-surface-hi transition',
            worktreeSession
              ? 'opacity-100'
              : 'opacity-0 group-hover:opacity-100 group-focus-within:opacity-100',
          )}
          title="归档"
          aria-label={worktreeSession ? '工作树会话，归档' : '归档'}
        >
          {worktreeSession ? (
            <span className="relative block w-[14px] h-[14px]" aria-hidden="true">
              <VsIcon
                name="worktree"
                size={14}
                className="absolute inset-0 opacity-100 transition-opacity group-hover:opacity-0 group-focus-within:opacity-0"
              />
              <VsIcon
                name="archive"
                size={14}
                className="absolute inset-0 opacity-0 transition-opacity group-hover:opacity-100 group-focus-within:opacity-100"
              />
            </span>
          ) : (
            <VsIcon name="archive" size={14} />
          )}
        </button>
      </span>
    </div>
  );
}

function OpencodeImportSelectAllCheckbox({ checked, indeterminate, disabled, onChange }) {
  const ref = useRef(null);
  useEffect(() => {
    if (ref.current) ref.current.indeterminate = !!indeterminate;
  }, [indeterminate]);
  return (
    <input
      ref={ref}
      type="checkbox"
      checked={checked}
      disabled={disabled}
      onChange={onChange}
      className="mt-[2px] h-4 w-4 shrink-0 accent-accent"
      aria-label="全选"
    />
  );
}

function OpencodeImportDialog({
  dialog,
  onCancel,
  onConfirm,
  onClose,
  onToggleSession,
  onToggleAll,
}) {
  if (!dialog) return null;
  const phase = dialog.phase || 'confirm';
  const status = dialog.status || {};
  const sessions = Array.isArray(dialog.sessions) ? dialog.sessions : [];
  const selectedIds = Array.isArray(dialog.selectedSessionIds) ? dialog.selectedSessionIds : [];
  const selectedSet = useMemo(() => new Set(selectedIds), [selectedIds]);
  const selectedCount = selectedIds.filter((id) => sessions.some((session) => session.id === id)).length;
  const progress = opencodeImportProgress({
    total: status.total ?? selectedCount,
    imported: status.imported,
    failed: status.failed,
    skipped: status.skipped,
  });
  const running = phase === 'running';
  const done = phase === 'complete';
  const errored = phase === 'error';
  const selectionDisabled = running || done || errored;
  const allSelected = sessions.length > 0 && sessions.every((session) => selectedSet.has(session.id));
  const partlySelected = !allSelected && sessions.some((session) => selectedSet.has(session.id));

  return (
    <div className="fixed inset-0 z-[1000] flex items-center justify-center bg-[rgba(0,0,0,0.2)]">
      <div className="w-[min(520px,calc(100vw-32px))] rounded-lg border border-border bg-surface shadow-xl px-5 py-4">
        <div className="text-[14px] font-medium text-fg">
          {opencodeImportConfirmationText(selectedCount)}
        </div>
        <div className="mt-3 h-64 overflow-y-auto rounded-md border border-border bg-surface-alt">
          <label className="sticky top-0 z-10 flex cursor-pointer items-start gap-3 border-b border-border bg-surface-alt px-3 py-2 text-[12px] text-fg">
            <OpencodeImportSelectAllCheckbox
              checked={allSelected}
              indeterminate={partlySelected}
              disabled={selectionDisabled || sessions.length === 0}
              onChange={onToggleAll}
            />
            <span className="flex-1 min-w-0 font-medium">全选</span>
            <span className="shrink-0 tabular-nums text-fg-mute">{selectedCount}/{sessions.length}</span>
          </label>
          {sessions.map((session) => {
            const detail = [
              session.model || session.provider || '',
              session.message_count > 0 ? `${session.message_count} 条消息` : '',
            ].filter(Boolean).join(' · ');
            return (
              <label
                key={session.id}
                className={clsx(
                  'flex cursor-pointer items-start gap-3 border-b border-border px-3 py-2 text-[12px] last:border-b-0',
                  selectionDisabled ? 'cursor-default opacity-80' : 'hover:bg-surface-hi',
                )}
              >
                <input
                  type="checkbox"
                  checked={selectedSet.has(session.id)}
                  disabled={selectionDisabled}
                  onChange={() => onToggleSession?.(session.id)}
                  className="mt-[2px] h-4 w-4 shrink-0 accent-accent"
                />
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-fg">
                    {session.title}
                    {session.archived ? <span className="ml-1 text-fg-mute">[已归档]</span> : null}
                  </span>
                  {detail ? <span className="mt-0.5 block truncate text-[11px] text-fg-mute">{detail}</span> : null}
                </span>
              </label>
            );
          })}
        </div>
        {(running || done || errored) && (
          <div className="mt-4">
            <div className="h-2 rounded-full bg-surface-hi overflow-hidden">
              <div
                className={clsx('h-full transition-all', errored ? 'bg-danger' : 'bg-accent')}
                style={{ width: `${progress.percent}%` }}
              />
            </div>
            <div className="mt-2 flex items-center justify-between text-[12px] text-fg-mute">
              <span>{progress.imported}/{progress.total}</span>
              <span>{errored ? (dialog.error || status.error || '导入失败') : `${progress.percent}%`}</span>
            </div>
            {status.current_title && running && (
              <div className="mt-2 text-[12px] text-fg-mute truncate">{status.current_title}</div>
            )}
          </div>
        )}
        <div className="mt-5 flex items-center justify-end gap-2">
          {phase === 'confirm' && (
            <>
              <button
                type="button"
                onClick={onCancel}
                className="px-3 py-1.5 rounded-md text-[12px] text-fg-mute hover:text-fg hover:bg-surface-hi"
              >
                取消
              </button>
              <button
                type="button"
                onClick={onConfirm}
                disabled={selectedCount <= 0}
                className={clsx(
                  'px-3 py-1.5 rounded-md text-[12px] bg-accent text-white hover:opacity-90',
                  selectedCount <= 0 && 'cursor-not-allowed opacity-50 hover:opacity-50',
                )}
              >
                确认导入
              </button>
            </>
          )}
          {(done || errored) && (
            <button
              type="button"
              onClick={onClose}
              className="px-3 py-1.5 rounded-md text-[12px] bg-accent text-white hover:opacity-90"
            >
              完成
            </button>
          )}
        </div>
      </div>
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
  activeTarget,
  onSelect,
  onRename,
  onActivate,
  onNewSession,
  onImportOpencode,
  onRemove,
  onTogglePin,
  onArchive,
  onRenameSession,
  pendingQuestionSessionIds,
  sessionsLoading = false,
  opencodeImportCount = 0,
  opencodeImportedHighlightKeys = new Set(),
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
        case DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS:
          detail.handled = true;
          onImportOpencode?.(ws);
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
  }, [expanded, onActivate, onImportOpencode, onNewSession, onRemove, onToggle, ws]);

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
        data-desktop-workspace-opencode-import-count={opencodeImportCount > 0 ? String(opencodeImportCount) : undefined}
        className={clsx(
          'ace-sidebar-tree-row-grid ace-sidebar-primary-text group grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 px-2 py-[6px] rounded-md text-[14px] cursor-pointer transition',
          ws.active ? 'bg-accent-bg text-fg' : 'text-fg hover:bg-surface-hi',
        )}
        onClick={() => (ws.active ? onToggle(ws.hash) : onActivate(ws))}
      >
        <span className="w-6 h-6 flex items-center justify-center shrink-0">
          <VsIcon name={expanded ? 'folderOpen' : 'folder'} size={14} />
        </span>
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
            className="ace-sidebar-primary-text min-w-0 h-7 px-1 py-0 text-[14px] bg-surface border border-accent rounded outline-none"
          />
        ) : (
          <span className="flex min-w-0 items-center gap-1 font-medium">
            <span className="min-w-0 truncate">{ws.name || ws.hash}</span>
            <SidebarDisclosure expanded={expanded} />
          </span>
        )}
        <span className="flex items-center justify-end gap-1 shrink-0">
          <button
            type="button"
            onClick={(e) => { e.stopPropagation(); onNewSession(ws); }}
            className="ace-sidebar-workspace-action w-6 h-6 rounded hover:bg-surface-hi flex items-center justify-center shrink-0 opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 focus-visible:opacity-100 transition"
            title="在此工作区新建任务"
            aria-label="在此工作区新建任务"
          ><VsIcon name="newSession" size={16} /></button>
          {hasUnread && <span className="w-1.5 h-1.5 rounded-full shrink-0 bg-ok shadow-[0_0_4px_var(--ace-ok)]" title="有未读任务" />}
          <button
            type="button"
            onClick={(e) => { e.stopPropagation(); setEditing(true); }}
            className="w-6 h-6 rounded text-fg-mute hover:text-fg hover:bg-surface-hi opacity-0 group-hover:opacity-100 flex items-center justify-center shrink-0 transition"
            title="重命名"
          ><VsIcon name="edit" size={16} /></button>
          {onRemove && (
            <button
              type="button"
              onClick={(e) => { e.stopPropagation(); onRemove(ws); }}
              className="w-6 h-6 rounded text-fg-mute hover:text-danger hover:bg-danger-bg opacity-0 group-hover:opacity-100 flex items-center justify-center shrink-0 transition"
              title="从桌面工作区列表移除"
            ><VsIcon name="close" size={16} /></button>
          )}
        </span>
      </div>
      {expanded && (
        <div className="my-1">
          {sessions.length === 0 ? (
            <div className="ace-sidebar-tree-row-grid ace-sidebar-meta-text grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 px-2 py-[4px] text-[13px] text-fg-mute italic">
              <span aria-hidden="true" />
              <span>{sessionsLoading ? '加载中...' : '暂无任务'}</span>
            </div>
          ) : (
            <>
              {projectedSessions.visibleSessions.map((s) => (
                <SessionRow
                  key={s.id}
                  s={s}
                  active={sessionMatchesRevealTarget(s, activeTarget) || (!activeTarget?.sessionId && s.id === activeId)}
                  pendingQuestion={sessionHasPendingQuestion(s, pendingQuestionSessionIds)}
                  onSelect={onSelect}
                  onTogglePin={onTogglePin}
                  onArchive={onArchive}
                  onRename={onRenameSession}
                  importHighlighted={opencodeImportedHighlightKeys.has(opencodeImportedSessionHighlightKey(ws.hash, s.id))}
                />
              ))}
              {projectedSessions.collapsible && (
                <div className="ace-sidebar-tree-row-grid grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 px-2">
                  <span aria-hidden="true" />
                  <button
                    type="button"
                    onClick={() => onToggleSessionList?.(ws.hash)}
                    className="ace-sidebar-meta-text py-[5px] rounded-md text-left text-[13px] text-fg-mute hover:text-fg hover:bg-surface-hi transition"
                  >
                    {projectedSessions.action === 'expand' ? '展开显示' : '折叠显示'}
                  </button>
                </div>
              )}
            </>
          )}
        </div>
      )}
    </div>
  );
}

function NoWorkspaceSessionGroup({
  sessions,
  sessionsLoading = false,
  sessionListExpanded,
  onToggleSessionList,
  activeId,
  activeTarget,
  onSelect,
  onArchive,
  onRenameSession,
  pendingQuestionSessionIds,
}) {
  const projectedSessions = sidebarSessionProjection(sessions, sessionListExpanded);

  return (
    <div className="my-1">
      {sessions.length === 0 ? (
        <div className="ace-sidebar-tree-row-grid ace-sidebar-meta-text grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 px-2 py-[4px] text-[13px] text-fg-mute italic">
          <span aria-hidden="true" />
          <span>{sessionsLoading ? '加载中...' : '暂无任务'}</span>
        </div>
      ) : (
        <>
          {projectedSessions.visibleSessions.map((s) => (
            <SessionRow
              key={`no-workspace-${s.id}`}
              s={s}
              active={sessionMatchesRevealTarget(s, activeTarget) || (!activeTarget?.sessionId && s.id === activeId)}
              pinEnabled={false}
              pendingQuestion={sessionHasPendingQuestion(s, pendingQuestionSessionIds)}
              onSelect={onSelect}
              onArchive={onArchive}
              onRename={onRenameSession}
            />
          ))}
          {projectedSessions.collapsible && (
            <div className="ace-sidebar-tree-row-grid grid grid-cols-[24px_minmax(0,1fr)_auto] items-center gap-x-[5px] mx-1.5 px-2">
              <span aria-hidden="true" />
              <button
                type="button"
                onClick={() => onToggleSessionList?.(NO_WORKSPACE_SESSION_LIST_KEY)}
                className="ace-sidebar-meta-text py-[5px] rounded-md text-left text-[13px] text-fg-mute hover:text-fg hover:bg-surface-hi transition"
              >
                {projectedSessions.action === 'expand' ? '展开显示' : '折叠显示'}
              </button>
            </div>
          )}
        </>
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
  onNewTask,
  onNewLoop,
  onSearchTasks,
  onOpenSettingsSection,
  pendingQuestionSessionIds = new Set(),
}) {
  const [workspaces,  setWorkspaces]  = useState([]);
  const [sessions,    setSessions]    = useState([]);
  const [statusBySession, setStatusBySession] = useState(() => new Map());
  const [pinnedByWorkspace, setPinnedByWorkspace] = useState(() => new Map());
  const [pinnedOrderItems, setPinnedOrderItems] = useState([]);
  const [sessionLoadingWorkspaces, setSessionLoadingWorkspaces] = useState(() => new Set());
  const [sessionLoadedWorkspaces, setSessionLoadedWorkspaces] = useState(() => new Set());
  const [expanded,    setExpanded]    = useState(new Set());
  const [expandedSessionLists, setExpandedSessionLists] = useState(new Set());
  const [sectionExpansion, setSectionExpansion] = usePreference(
    SIDEBAR_SECTIONS_STORAGE_KEY,
    DEFAULT_SIDEBAR_SECTION_EXPANSION,
    validateSidebarSectionExpansion,
  );
  const [activeWorkspaceHash, setActiveWorkspaceHash] = useState('');
  const refreshingRef = useRef(false);
  const pendingRefreshHashRef = useRef('');
  const expandedRef = useRef(new Set());
  const workspaceCollapseAllRef = useRef(false);
  const pinnedByWorkspaceRef = useRef(new Map());
  const pinnedOrderItemsRef = useRef([]);
  const retainedSessionIdsRef = useRef(new Set());
  const sidebarScrollRef = useRef(null);
  const pinnedDragRef = useRef(null);
  const suppressPinnedClickRef = useRef(false);
  const [pinnedDragState, setPinnedDragState] = useState(null);
  const [pinnedDragGhost, setPinnedDragGhost] = useState(null);
  const [opencodeImportPreviews, setOpencodeImportPreviews] = useState(() => new Map());
  const opencodeImportPreviewsRef = useRef(new Map());
  const [opencodeImportDialog, setOpencodeImportDialog] = useState(null);
  const opencodeImportPollRef = useRef(0);
  const [opencodeImportedHighlightKeys, setOpencodeImportedHighlightKeys] = useState(() => new Set());
  const opencodeImportedHighlightTimersRef = useRef(new Map());
  const revealedSectionTargetRef = useRef('');
  const revealTarget = useMemo(() => sidebarRevealTarget(activeRef), [activeRef]);

  const updateExpanded = useCallback((updater) => {
    setExpanded((prev) => {
      const next = typeof updater === 'function' ? updater(prev) : updater;
      expandedRef.current = next;
      return next;
    });
  }, []);

  const toggleSidebarSection = useCallback((sectionId) => {
    setSectionExpansion((prev) => ({ ...prev, [sectionId]: !prev[sectionId] }));
  }, [setSectionExpansion]);

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

  const setOpencodeImportPreview = useCallback((hash, preview) => {
    if (!hash) return;
    const normalized = normalizeOpencodeImportPreview(preview || {});
    setOpencodeImportPreviews((prev) => {
      const next = new Map(prev);
      next.set(hash, normalized);
      opencodeImportPreviewsRef.current = next;
      return next;
    });
  }, []);

  const refreshOpencodeImportPreview = useCallback(async (ws) => {
    if (!ws?.hash || ws.hash === '__local__') return null;
    try {
      const preview = normalizeOpencodeImportPreview(await api.getOpencodeImportPreview(ws.hash));
      setOpencodeImportPreview(ws.hash, preview);
      return preview;
    } catch {
      const preview = normalizeOpencodeImportPreview({ available: false, count: 0 });
      setOpencodeImportPreview(ws.hash, preview);
      return preview;
    }
  }, [setOpencodeImportPreview]);

  useEffect(() => () => {
    if (opencodeImportPollRef.current) {
      window.clearTimeout(opencodeImportPollRef.current);
      opencodeImportPollRef.current = 0;
    }
  }, []);

  useEffect(() => () => {
    for (const timer of opencodeImportedHighlightTimersRef.current.values()) {
      window.clearTimeout(timer);
    }
    opencodeImportedHighlightTimersRef.current.clear();
  }, []);

  const setPinnedOrder = useCallback((updater) => {
    setPinnedOrderItems((prev) => {
      const rawNext = typeof updater === 'function' ? updater(prev) : updater;
      const next = normalizePinnedOrderItems(rawNext);
      pinnedOrderItemsRef.current = next;
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

  const applyPinnedReorder = useCallback(async ({
    workspaceHash,
    sourceId,
    targetWorkspaceHash,
    targetId,
    placement,
  }) => {
    if (!workspaceHash || !sourceId || !targetWorkspaceHash || !targetId) return;
    const previousPinned = normalizePinnedIds(pinnedByWorkspaceRef.current.get(workspaceHash) || []);
    const previousOrder = normalizePinnedOrderItems(pinnedOrderItemsRef.current);
    const baseOrder = pinnedOrderItemsFromDom();
    const currentOrder = baseOrder.length ? baseOrder : previousOrder;
    const sourceItem = { workspace_hash: workspaceHash, session_id: sourceId };
    const targetItem = { workspace_hash: targetWorkspaceHash, session_id: targetId };
    const nextOrder = reorderPinnedOrderItems(currentOrder, sourceItem, targetItem, placement);

    const sameWorkspace = workspaceHash === targetWorkspaceHash;
    const nextPinned = sameWorkspace
      ? reorderPinnedSessionId(previousPinned, sourceId, targetId, placement)
      : previousPinned;
    const pinnedChanged = !sameStringArray(previousPinned, nextPinned);
    const orderChanged = !samePinnedOrderItems(currentOrder, nextOrder);
    if (!pinnedChanged && !orderChanged) return;

    if (pinnedChanged) setPinnedWorkspaceIds(workspaceHash, nextPinned);
    if (orderChanged) setPinnedOrder(nextOrder);
    try {
      const saves = [];
      if (pinnedChanged) saves.push(api.setPinnedSessions(workspaceHash, nextPinned));
      if (orderChanged) saves.push(api.setPinnedSessionOrder(nextOrder));
      const results = await Promise.all(saves);
      let resultIndex = 0;
      if (pinnedChanged) {
        const saved = results[resultIndex++];
        setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || nextPinned));
      }
      if (orderChanged) {
        const saved = results[resultIndex++];
        setPinnedOrder(normalizePinnedOrderItems(saved?.items || nextOrder));
      }
    } catch (e) {
      if (pinnedChanged) setPinnedWorkspaceIds(workspaceHash, previousPinned);
      if (orderChanged) setPinnedOrder(previousOrder);
      toast({ kind: 'err', text: '置顶排序失败:' + (e.message || '') });
    }
  }, [setPinnedOrder, setPinnedWorkspaceIds]);

  const updatePinnedDragTarget = useCallback((clientY) => {
    const drag = pinnedDragRef.current;
    if (!drag) return;
    const target = pinnedDropTargetForPointer(clientY);
    if (!target) return;
    drag.targetWorkspaceHash = target.targetWorkspaceHash;
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
        targetWorkspaceHash: drag.targetWorkspaceHash || drag.workspaceHash,
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
      targetWorkspaceHash: workspaceHash,
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
    const previousOrder = normalizePinnedOrderItems(pinnedOrderItemsRef.current);
    const orderItem = { workspace_hash: workspaceHash, session_id: id };
    const nextOrder = shouldPin
      ? pinPinnedOrderItem(previousOrder, orderItem)
      : unpinPinnedOrderItem(previousOrder, orderItem);
    setPinnedWorkspaceIds(workspaceHash, next);
    setPinnedOrder(nextOrder);

    try {
      const [saved, savedOrder] = await Promise.all([
        api.setPinnedSessions(workspaceHash, next),
        api.setPinnedSessionOrder(nextOrder),
      ]);
      setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || next));
      setPinnedOrder(normalizePinnedOrderItems(savedOrder?.items || nextOrder));
      toast({ kind: 'ok', text: shouldPin ? '已置顶' : '已取消置顶' });
    } catch (e) {
      setPinnedWorkspaceIds(workspaceHash, previous);
      setPinnedOrder(previousOrder);
      toast({ kind: 'err', text: (shouldPin ? '置顶失败:' : '取消置顶失败:') + (e.message || '') });
    }
  }, [activeWorkspaceHash, setPinnedOrder, setPinnedWorkspaceIds]);

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

  const refresh = useCallback(async (preferredHash = null) => {
    const revealWorkspaceHash = revealTarget.noWorkspace ? '' : revealTarget.workspaceHash;
    const requestedHash = preferredHash == null
      ? (revealTarget.noWorkspace ? '' : (revealWorkspaceHash || activeWorkspaceHash))
      : preferredHash;
    if (refreshingRef.current) {
      pendingRefreshHashRef.current = requestedHash || (revealTarget.noWorkspace ? '' : activeWorkspaceHash) || pendingRefreshHashRef.current;
      return;
    }
    refreshingRef.current = true;
    try {
      const activeNoWorkspace = !!revealTarget.noWorkspace && !requestedHash;
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
      const shouldAutoExpandWorkspace = !workspaceCollapseAllRef.current;
      const chosen = activeNoWorkspace
        ? ''
        : ((requestedHash && availableHashes.has(requestedHash))
          ? requestedHash
          : (workspaceArr.find((w) => w.active)?.hash || workspaceArr[0]?.hash || ''));
      const withActive = workspaceArr.map((w) => ({ ...w, active: !!chosen && w.hash === chosen }));
      const expandedHashes = new Set(expandedRef.current);
      if (shouldAutoExpandWorkspace && chosen) expandedHashes.add(chosen);
      if (shouldAutoExpandWorkspace && revealWorkspaceHash && availableHashes.has(revealWorkspaceHash)) {
        expandedHashes.add(revealWorkspaceHash);
      }
      setActiveWorkspaceHash(chosen);
      setWorkspaces(withActive);
      withActive
        .filter((w) => w.hash && w.hash !== '__local__')
        .forEach((w) => refreshOpencodeImportPreview(w).catch(() => {}));
      const earlyVisibleWorkspaceHashes = withActive
        .filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || w.hash === revealWorkspaceHash)
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
      try {
        const orderState = await api.getPinnedSessionOrder();
        setPinnedOrder(normalizePinnedOrderItems(orderState?.items || []));
      } catch {
        setPinnedOrder(pinnedOrderItemsRef.current);
      }
      const pinnedWorkspaceHashes = new Set(Array.from(nextPinnedMap.entries())
        .filter(([, ids]) => ids.length > 0)
        .map(([hash]) => hash));

      updateExpanded((prev) => {
        const next = new Set(prev);
        if (shouldAutoExpandWorkspace) {
          for (const w of withActive) if (w.active) next.add(w.hash);
          if (revealWorkspaceHash && availableHashes.has(revealWorkspaceHash)) next.add(revealWorkspaceHash);
        }
        return next;
      });
      withActive
        .filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || pinnedWorkspaceHashes.has(w.hash) || w.hash === revealWorkspaceHash)
        .forEach((w) => connection.subscribeWorkspaceStatus(w.hash));

      const visibleWorkspaces = withActive.filter((w) => w.active || w.hash === '__local__' || expandedHashes.has(w.hash) || pinnedWorkspaceHashes.has(w.hash) || w.hash === revealWorkspaceHash);
      const visibleWorkspaceHashes = visibleWorkspaces.map((w) => w.hash).filter(Boolean);
      const visibleWorkspaceHashSet = new Set(visibleWorkspaceHashes);
      const hiddenWorkspaceHashes = withActive
        .map((w) => w.hash)
        .filter((hash) => hash && !visibleWorkspaceHashSet.has(hash));
      setSessionWorkspacesLoaded(hiddenWorkspaceHashes, false);
      setSessionWorkspaceLoading(hiddenWorkspaceHashes, false);
      setSessionWorkspaceLoading(visibleWorkspaceHashes, true);
      try {
        const noWorkspaceListPromise = api.listSessions().catch(() => []);
        const perWorkspace = await Promise.all(visibleWorkspaces.map(async (w) => {
          if (w.hash === '__local__') {
            const list = await api.listSessions();
            return (Array.isArray(list) ? list : [])
              .filter((s) => !isNoWorkspaceSession(s))
              .map((s) => normalizeWorkspaceSession(s, w));
          }
          const list = await api.listWorkspaceSessions(w.hash);
          return (Array.isArray(list) ? list : [])
            .filter((s) => !isNoWorkspaceSession(s))
            .map((s) => normalizeWorkspaceSession(s, w));
        }));
        const noWorkspaceRaw = await noWorkspaceListPromise;
        const noWorkspaceIncoming = (Array.isArray(noWorkspaceRaw) ? noWorkspaceRaw : [])
          .filter(isNoWorkspaceSession)
          .map(normalizeNoWorkspaceSession);
        const incoming = [
          ...perWorkspace.flat().filter((s) => !isNoWorkspaceSession(s)),
          ...noWorkspaceIncoming,
        ];
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
  }, [activeWorkspaceHash, refreshOpencodeImportPreview, revealTarget, setPinnedMap, setPinnedOrder, setSessionWorkspaceLoading, setSessionWorkspacesLoaded, syncRetainedSessionIds, updateExpanded]);

  const archiveSession = useCallback(async (session) => {
    const id = session?.id || session?.sessionId || session?.session_id || '';
    const noWorkspace = isNoWorkspaceSession(session);
    const workspaceHash = noWorkspace ? '' : (session?.workspace_hash || session?.workspaceHash || activeWorkspaceHash || '');
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
        const previousOrder = normalizePinnedOrderItems(pinnedOrderItemsRef.current);
        const nextOrder = unpinPinnedOrderItem(previousOrder, { workspace_hash: workspaceHash, session_id: id });
        setPinnedWorkspaceIds(workspaceHash, nextPinned);
        setPinnedOrder(nextOrder);
        try {
          const [saved, savedOrder] = await Promise.all([
            api.setPinnedSessions(workspaceHash, nextPinned),
            api.setPinnedSessionOrder(nextOrder),
          ]);
          setPinnedWorkspaceIds(workspaceHash, normalizePinnedIds(saved?.session_ids || nextPinned));
          setPinnedOrder(normalizePinnedOrderItems(savedOrder?.items || nextOrder));
        } catch {
          // Archiving already succeeded; stale pinned state will be pruned on next refresh.
        }
      }

      setSessions((prev) => prev.filter((item) => (item.id || item.session_id || item.sessionId) !== id));
      if (id === activeId) {
        onOpenHome?.(noWorkspace
          ? { noWorkspace: true, name: '不使用工作区' }
          : {
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
  }, [activeId, activeWorkspaceHash, onOpenHome, refresh, setPinnedOrder, setPinnedWorkspaceIds]);

  const renameSession = useCallback(async (session, title) => {
    const id = session?.id || session?.sessionId || session?.session_id || '';
    const noWorkspace = isNoWorkspaceSession(session);
    const workspaceHash = noWorkspace ? '' : (session?.workspace_hash || session?.workspaceHash || activeWorkspaceHash || '');
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
      const noWorkspace = !!(
        detail.noWorkspace || detail.session?.noWorkspace || detail.session?.no_workspace
      );
      const workspaceHash = noWorkspace ? '' : (detail.workspaceHash || activeWorkspaceHash);
      if (workspaceHash) {
        updateExpanded((prev) => new Set(prev).add(workspaceHash));
        connection.subscribeWorkspaceStatus(workspaceHash);
      }
      if (detail.session) {
        const session = noWorkspace
          ? normalizeNoWorkspaceSession(detail.session)
          : {
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
  const noWorkspaceSessions = useMemo(
    () => renderedSessions.filter(isNoWorkspaceSession).map(normalizeNoWorkspaceSession),
    [renderedSessions],
  );
  const workspaceSessions = useMemo(
    () => renderedSessions.filter((session) => !isNoWorkspaceSession(session)),
    [renderedSessions],
  );
  const pinnedSessions = useMemo(
    () => pinnedSessionsForList(workspaceSessions, pinnedByWorkspace, pinnedOrderItems),
    [pinnedByWorkspace, pinnedOrderItems, workspaceSessions],
  );
  const sectionCounts = useMemo(
    () => sidebarSectionCounts({ pinnedSessions, noWorkspaceSessions, workspaces }),
    [noWorkspaceSessions, pinnedSessions, workspaces],
  );

  useEffect(() => {
    if (!revealTarget.sessionId) return undefined;
    const pinnedTarget = pinnedSessions.some((session) => sessionMatchesRevealTarget(session, revealTarget));
    const targetSection = pinnedTarget
      ? SIDEBAR_SECTION_IDS.PINNED
      : (revealTarget.noWorkspace ? SIDEBAR_SECTION_IDS.TASKS : SIDEBAR_SECTION_IDS.WORKSPACES);
    const sectionTargetKey = `${targetSection}\u0000${revealTarget.workspaceHash || ''}\u0000${revealTarget.sessionId}`;
    if (revealedSectionTargetRef.current !== sectionTargetKey) {
      revealedSectionTargetRef.current = sectionTargetKey;
      setSectionExpansion((prev) => (
        prev[targetSection] ? prev : { ...prev, [targetSection]: true }
      ));
    }
    const listKey = revealTarget.noWorkspace ? NO_WORKSPACE_SESSION_LIST_KEY : revealTarget.workspaceHash;
    const sourceSessions = revealTarget.noWorkspace
      ? noWorkspaceSessions
      : workspaceSessions.filter((session) => (
        !revealTarget.workspaceHash ||
        (session.workspace_hash || session.workspaceHash || '') === revealTarget.workspaceHash
      ));

    if (!revealTarget.noWorkspace && revealTarget.workspaceHash) {
      updateExpanded((prev) => {
        if (prev.has(revealTarget.workspaceHash)) return prev;
        return new Set(prev).add(revealTarget.workspaceHash);
      });
    }

    if (listKey && sessionListNeedsRevealExpansion(
      sourceSessions,
      revealTarget,
      expandedSessionLists.has(listKey),
    )) {
      setExpandedSessionLists((prev) => {
        if (prev.has(listKey)) return prev;
        return new Set(prev).add(listKey);
      });
    }

    if (typeof window === 'undefined' || typeof document === 'undefined') return undefined;
    const frame = window.requestAnimationFrame(() => {
      const rows = Array.from(document.querySelectorAll('.ace-sidebar-session-row[data-desktop-session-id]'));
      const matches = rows.filter((row) => {
        if (row.getAttribute('data-desktop-session-id') !== revealTarget.sessionId) return false;
        const rowWorkspace = row.getAttribute('data-desktop-session-workspace') || '';
        if (revealTarget.noWorkspace) return !rowWorkspace;
        if (!revealTarget.workspaceHash) return !!rowWorkspace;
        return rowWorkspace === revealTarget.workspaceHash;
      });
      const row = matches.find((item) => item.getAttribute('data-desktop-session-pinned') !== 'true') || matches[0];
      row?.scrollIntoView?.({ block: 'nearest' });
    });
    return () => window.cancelAnimationFrame?.(frame);
  }, [expandedSessionLists, noWorkspaceSessions, pinnedSessions, revealTarget, setSectionExpansion, updateExpanded, workspaceSessions]);

  // 把已加载的跨 workspace sessions / pinned order / workspaceName 推到桌面 tray 菜单。
  // pushTrayMenu 内部 100ms debounce + 无 bridge 时 no-op。
  // 设计:openspec/changes/enhance-desktop-tray-menu。
  useEffect(() => {
    const activeWs = workspaces.find((w) => w.hash === activeWorkspaceHash);
    pushTrayMenu({
      sessions: workspaceSessions,
      pinnedByWorkspace,
      pinnedOrderItems,
      workspaceName: activeWs?.name || '',
      workspaces,
    });
  }, [workspaceSessions, pinnedByWorkspace, pinnedOrderItems, activeWorkspaceHash, workspaces]);

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
    if (willExpand) workspaceCollapseAllRef.current = false;
    expandedRef.current = next;
    setExpanded(next);
    if (willExpand) {
      setSessionWorkspaceLoading([hash], true);
      refresh(hash).catch(() => {});
    }
  };

  const onActivate = async (ws) => {
    workspaceCollapseAllRef.current = false;
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

  const openOpencodeImportDialog = useCallback(async (ws) => {
    if (!ws?.hash) return;
    let preview = opencodeImportPreviewsRef.current.get(ws.hash);
    if (!preview) {
      preview = await refreshOpencodeImportPreview(ws);
    }
    const count = Number(preview?.count || 0);
    if (count <= 0) {
      toast({ kind: 'info', text: '没有可导入的 opencode 会话' });
      return;
    }
    const sessions = Array.isArray(preview?.sessions) ? preview.sessions : [];
    const selectedSessionIds = defaultOpencodeImportSelection(sessions);
    setOpencodeImportDialog({
      workspace: ws,
      count: selectedSessionIds.length,
      sessions,
      selectedSessionIds,
      phase: 'confirm',
      status: { total: selectedSessionIds.length, imported: 0, failed: 0, skipped: 0 },
      error: '',
    });
  }, [refreshOpencodeImportPreview]);

  const toggleOpencodeImportSession = useCallback((sessionId) => {
    if (!sessionId) return;
    setOpencodeImportDialog((prev) => {
      if (!prev || prev.phase !== 'confirm') return prev;
      const selected = new Set(Array.isArray(prev.selectedSessionIds) ? prev.selectedSessionIds : []);
      if (selected.has(sessionId)) selected.delete(sessionId);
      else selected.add(sessionId);
      const selectedSessionIds = Array.from(selected);
      return {
        ...prev,
        count: selectedSessionIds.length,
        selectedSessionIds,
        status: { ...(prev.status || {}), total: selectedSessionIds.length },
      };
    });
  }, []);

  const toggleAllOpencodeImportSessions = useCallback(() => {
    setOpencodeImportDialog((prev) => {
      if (!prev || prev.phase !== 'confirm') return prev;
      const selectedSessionIds = toggleAllOpencodeImportSelection(
        Array.isArray(prev.sessions) ? prev.sessions : [],
        Array.isArray(prev.selectedSessionIds) ? prev.selectedSessionIds : [],
      );
      return {
        ...prev,
        count: selectedSessionIds.length,
        selectedSessionIds,
        status: { ...(prev.status || {}), total: selectedSessionIds.length },
      };
    });
  }, []);

  const flashOpencodeImportedSessions = useCallback((workspaceHash, sessionIds) => {
    const keys = opencodeImportedSessionHighlightKeys(workspaceHash, sessionIds);
    if (keys.length === 0) return;

    setOpencodeImportedHighlightKeys((prev) => {
      const next = new Set(prev);
      for (const key of keys) next.add(key);
      return next;
    });

    for (const key of keys) {
      const existing = opencodeImportedHighlightTimersRef.current.get(key);
      if (existing) window.clearTimeout(existing);
      const timer = window.setTimeout(() => {
        opencodeImportedHighlightTimersRef.current.delete(key);
        setOpencodeImportedHighlightKeys((prev) => {
          if (!prev.has(key)) return prev;
          const next = new Set(prev);
          next.delete(key);
          return next;
        });
      }, OPENCODE_IMPORT_HIGHLIGHT_MS);
      opencodeImportedHighlightTimersRef.current.set(key, timer);
    }
  }, []);

  const pollOpencodeImportJob = useCallback((ws, jobId) => {
    if (!ws?.hash || !jobId) return;
    if (opencodeImportPollRef.current) {
      window.clearTimeout(opencodeImportPollRef.current);
      opencodeImportPollRef.current = 0;
    }

    const tick = async () => {
      try {
        const status = await api.getOpencodeImportJob(ws.hash, jobId);
        const state = status?.state || 'running';
        setOpencodeImportDialog((prev) => prev
          ? {
              ...prev,
              phase: state === 'complete' ? 'complete' : (state === 'failed' ? 'error' : 'running'),
              status,
              error: status?.error || '',
            }
          : prev);
        if (state === 'pending' || state === 'running') {
          opencodeImportPollRef.current = window.setTimeout(tick, 400);
          return;
        }
        if (state === 'complete') {
          toast({ kind: 'ok', text: 'opencode 会话导入完成' });
        } else {
          toast({ kind: 'err', text: 'opencode 会话导入失败:' + (status?.error || '') });
        }
        await refresh(ws.hash);
        if (state === 'complete') {
          flashOpencodeImportedSessions(ws.hash, status?.session_ids || []);
        }
        await refreshOpencodeImportPreview(ws);
      } catch (e) {
        setOpencodeImportDialog((prev) => prev
          ? { ...prev, phase: 'error', error: e.message || '导入失败' }
          : prev);
      }
    };

    tick();
  }, [flashOpencodeImportedSessions, refresh, refreshOpencodeImportPreview]);

  const confirmOpencodeImport = useCallback(async () => {
    const ws = opencodeImportDialog?.workspace;
    if (!ws?.hash) return;
    const selectedSessionIds = Array.isArray(opencodeImportDialog?.selectedSessionIds)
      ? opencodeImportDialog.selectedSessionIds
      : [];
    if (selectedSessionIds.length <= 0) {
      toast({ kind: 'info', text: '请选择要导入的 opencode 会话' });
      return;
    }
    if (opencodeImportPollRef.current) {
      window.clearTimeout(opencodeImportPollRef.current);
      opencodeImportPollRef.current = 0;
    }
    setOpencodeImportDialog((prev) => prev ? { ...prev, phase: 'running', error: '' } : prev);
    try {
      const status = await api.startOpencodeImport(ws.hash, selectedSessionIds);
      const jobId = status?.job_id;
      if (!jobId) throw new Error('missing import job id');
      setOpencodeImportDialog((prev) => prev ? { ...prev, status: status || prev.status } : prev);
      pollOpencodeImportJob(ws, jobId);
    } catch (e) {
      setOpencodeImportDialog((prev) => prev
        ? { ...prev, phase: 'error', error: e.message || '启动导入失败' }
        : prev);
    }
  }, [opencodeImportDialog, pollOpencodeImportJob]);

  const closeOpencodeImportDialog = useCallback(() => {
    if (opencodeImportPollRef.current) {
      window.clearTimeout(opencodeImportPollRef.current);
      opencodeImportPollRef.current = 0;
    }
    setOpencodeImportDialog(null);
  }, []);

  const selectSession = async (ws, session) => {
    if (!session?.id) return;
    const noWorkspace = isNoWorkspaceSession(session) || !!ws?.noWorkspace;
    if (!noWorkspace) workspaceCollapseAllRef.current = false;
    if (!session.active) {
      try {
        if (noWorkspace) {
          await api.resumeSession(session.id);
        } else if (ws?.hash && ws.hash !== '__local__') {
          await api.resumeWorkspaceSession(ws.hash, session.id);
        } else {
          await api.resumeSession(session.id);
        }
        refresh().catch(() => {});
      } catch (e) {
        if (!noWorkspace && hasDesktopBridge() && ws?.hash) {
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
    const selectedSession = noWorkspace ? normalizeNoWorkspaceSession(session) : session;
    if (noWorkspace) {
      setActiveWorkspaceHash('');
      setWorkspaces((prev) => prev.map((item) => ({ ...item, active: false })));
    }
    markSessionRead(selectedSession);
    onSelect?.({
      workspaceHash: noWorkspace ? '' : (session.workspace_hash || ws?.hash),
      noWorkspace,
      contextId: 'default',
      sessionId: session.id,
      displayTitle: session.displayTitle || session.display_title,
      port: ws?.port,
      token: ws?.token,
      cwd: noWorkspace ? '' : (session.cwd || ws?.cwd),
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
      toast({ kind: 'ok', text: '已从桌面工作区列表移除' });
      await refresh(nextHash);
    } catch (e) {
      toast({ kind: 'err', text: '移除工作区失败:' + (e.message || '') });
    }
  };

  const createSessionInWorkspace = async (ws) => {
    if (!ws?.hash) return;
    workspaceCollapseAllRef.current = false;
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
      toast({ kind: 'err', text: '新建任务失败:' + (e.message || '') });
    }
  };

  const onAddWorkspace = async () => {
    try {
      const ws = await pickExistingWorkspace({ api });
      if (ws == null) return;
      await refresh(ws.hash);
      await onActivate(ws);
    } catch (e) {
      if (!hasDesktopBridge() && (e.status === 404 || e.status === 501)) {
        toast({ kind: 'info', text: '需在 desktop webapp 中使用' });
      } else {
        toast({ kind: 'err', text: '添加工作区失败:' + (e.message || '') });
      }
    }
  };

  const collapseAllWorkspaces = useCallback(() => {
    workspaceCollapseAllRef.current = true;
    updateExpanded(new Set());
  }, [updateExpanded]);

  const sidebarNavCallbacks = {
    onNewTask,
    onNewLoop,
    onSearchTasks,
  };
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
    <>
      <aside
        data-tour-target="sidebar"
        className={[
          'ace-sidebar bg-surface-alt border-r border-border flex flex-col font-sans shrink-0 overflow-hidden',
          'transition-[width,min-width] duration-250',
          collapsed ? 'w-0 min-w-0' : '',
        ].join(' ')}
        style={collapsed ? undefined : { width, minWidth: width }}
      >
      <div className="ace-sidebar-content flex-1 flex flex-col min-h-0">
        <div className="ace-sidebar-main flex-1 flex flex-col min-h-0">
          <div className="ace-sidebar-fixed-nav shrink-0 px-1.5 pt-2 pb-2 border-b border-border">
            {SIDEBAR_NAV_ITEMS.map((item) => (
              <SidebarNavItem
                key={item.id}
                item={item}
                onClick={sidebarNavCallbacks[item.callback]}
              />
            ))}
          </div>
          <div ref={sidebarScrollRef} className="ace-sidebar-scroll flex-1 overflow-y-auto pb-2">
            <SidebarSectionHeader
              sectionId={SIDEBAR_SECTION_IDS.PINNED}
              count={sectionCounts.pinned}
              expanded={sectionExpansion.pinned}
              onToggle={() => toggleSidebarSection(SIDEBAR_SECTION_IDS.PINNED)}
            />
            {sidebarSectionIsVisible(sectionCounts.pinned) && sectionExpansion.pinned && (
              <div className="my-1">
                {pinnedSessions.map((s) => {
                  const rowKey = pinnedSessionKey(s.workspace_hash || s.workspaceHash || '', s.id);
                  return (
                    <SessionRow
                      key={`pinned-${s.workspace_hash || ''}-${s.id}`}
                      s={s}
                      pinned
                      active={sessionMatchesRevealTarget(s, revealTarget) || (!revealTarget.sessionId && s.id === activeId)}
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
            )}
            <SidebarSectionHeader
              sectionId={SIDEBAR_SECTION_IDS.TASKS}
              count={sectionCounts.tasks}
              expanded={sectionExpansion.tasks}
              onToggle={() => toggleSidebarSection(SIDEBAR_SECTION_IDS.TASKS)}
            />
            {sidebarSectionIsVisible(sectionCounts.tasks) && sectionExpansion.tasks && (
              <NoWorkspaceSessionGroup
                sessions={noWorkspaceSessions}
                sessionsLoading={false}
                sessionListExpanded={expandedSessionLists.has(NO_WORKSPACE_SESSION_LIST_KEY)}
                onToggleSessionList={toggleSessionListExpanded}
                activeId={activeId}
                activeTarget={revealTarget}
                onSelect={(session) => selectSession({ noWorkspace: true }, session)}
                onArchive={archiveSession}
                onRenameSession={renameSession}
                pendingQuestionSessionIds={pendingQuestionSessionIds}
              />
            )}
            <SidebarSectionHeader
              sectionId={SIDEBAR_SECTION_IDS.WORKSPACES}
              count={sectionCounts.workspaces}
              expanded={sectionExpansion.workspaces}
              onToggle={() => toggleSidebarSection(SIDEBAR_SECTION_IDS.WORKSPACES)}
              actions={(
                <>
                  <button
                    data-sidebar-collapse-all-workspaces="true"
                    type="button"
                    onClick={collapseAllWorkspaces}
                    className="ace-sidebar-heading-collapse-btn"
                    title="全部收缩工作区"
                    aria-label="全部收缩工作区"
                  >
                    <VsIcon name="collapseAll" size={16} />
                  </button>
                  <button
                    data-tour-target="sidebar-add-project"
                    type="button"
                    onClick={onAddWorkspace}
                    className="w-6 h-6 rounded text-fg-mute hover:text-fg hover:bg-surface-hi flex items-center justify-center shrink-0 transition"
                    title="添加工作区"
                    aria-label="添加工作区"
                  >
                    <VsIcon name="folderAdd" size={16} />
                  </button>
                </>
              )}
            />
            {sidebarSectionIsVisible(sectionCounts.workspaces) && sectionExpansion.workspaces && (
              <div className="my-1">
                {workspaces.map((ws) => {
                  const items = filterPinnedSessions(
                    workspaceSessions.filter((s) => s.workspace_hash ? s.workspace_hash === ws.hash : !!ws.active),
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
                      activeTarget={revealTarget}
                      onSelect={(session) => selectSession(ws, session)}
                      onRename={onRename}
                      onActivate={onActivate}
                      onNewSession={createSessionInWorkspace}
                      onImportOpencode={openOpencodeImportDialog}
                      onRemove={hasDesktopRemoveWorkspace() ? removeWorkspace : undefined}
                      onTogglePin={togglePinnedSession}
                      onArchive={archiveSession}
                      onRenameSession={renameSession}
                      pendingQuestionSessionIds={pendingQuestionSessionIds}
                      sessionsLoading={sessionLoadingWorkspaces.has(ws.hash) || !sessionLoadedWorkspaces.has(ws.hash)}
                      opencodeImportCount={opencodeImportPreviews.get(ws.hash)?.count || 0}
                      opencodeImportedHighlightKeys={opencodeImportedHighlightKeys}
                    />
                  );
                })}
              </div>
            )}
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
        <CustomSidebarSection onOpenSettingsSection={onOpenSettingsSection} />
      </div>
      </aside>
      <OpencodeImportDialog
        dialog={opencodeImportDialog}
        onCancel={closeOpencodeImportDialog}
        onConfirm={confirmOpencodeImport}
        onClose={closeOpencodeImportDialog}
        onToggleSession={toggleOpencodeImportSession}
        onToggleAll={toggleAllOpencodeImportSessions}
      />
    </>
  );
}
