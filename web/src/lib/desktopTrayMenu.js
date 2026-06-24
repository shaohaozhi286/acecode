// 桌面 tray 菜单数据推送。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3。
//
// 浏览器直连 daemon(无 acecode-desktop 桌面壳)模式下 window.aceDesktop_setTrayMenu
// 不存在 → pushTrayMenu no-op。
//
// 节流:同步 100ms debounce(任意 trailing 调用都会落到 native),避免 WS 高频
// session_status 帧把 bridge 调爆;payload 归一化在 buildTrayMenuPayload
// 这一纯函数里完成,便于单测。Native 负责"顶层 3 条 + More"的菜单布局。

import { sessionDisplayTitle, withNewSessionDisplayTitles } from './sessionTitle.js';
import {
  normalizePinnedIds,
  normalizePinnedOrderItems,
  pinnedSessionsForList,
} from './pinnedSessions.js';

export const SUBTITLE_LIMIT = 40;

// codepoint-safe truncate;> SUBTITLE_LIMIT 时尾部带 "…"。
function truncateSubtitle(s) {
  if (s == null) return '';
  const str = String(s);
  const cps = Array.from(str);
  if (cps.length <= SUBTITLE_LIMIT) return str;
  return cps.slice(0, SUBTITLE_LIMIT - 1).join('') + '…';
}

function safeTitle(s) {
  if (s == null) return '新会话1';
  const t = String(s).trim();
  return t || '新会话1';
}

function sessionId(session) {
  return String(session?.id || session?.session_id || session?.sessionId || '');
}

function sessionWorkspaceHash(session) {
  return String(session?.workspace_hash || session?.workspaceHash || '');
}

function sessionKey(session) {
  const id = sessionId(session);
  if (!id) return '';
  return `${sessionWorkspaceHash(session)}\u0000${id}`;
}

function pinnedEntries(pinnedByWorkspace) {
  if (pinnedByWorkspace instanceof Map) return Array.from(pinnedByWorkspace.entries());
  if (pinnedByWorkspace && typeof pinnedByWorkspace === 'object') return Object.entries(pinnedByWorkspace);
  return [];
}

function pinnedKeySet(pinnedByWorkspace) {
  const out = new Set();
  for (const [workspaceHash, ids] of pinnedEntries(pinnedByWorkspace)) {
    const ws = String(workspaceHash || '');
    for (const id of normalizePinnedIds(ids)) out.add(`${ws}\u0000${id}`);
  }
  return out;
}

function workspaceNameMap(workspaces) {
  const out = new Map();
  for (const ws of Array.isArray(workspaces) ? workspaces : []) {
    const hash = String(ws?.hash || ws?.workspace_hash || ws?.workspaceHash || '');
    if (!hash) continue;
    const name = String(ws?.name || ws?.cwd || hash || '').trim();
    if (name) out.set(hash, name);
  }
  return out;
}

function workspaceSubtitle(session, { workspaceName, workspaceByHash }) {
  const direct = String(session?.workspaceName || session?.workspace_name || '').trim();
  if (direct) return direct;
  const hash = sessionWorkspaceHash(session);
  const mapped = hash ? String(workspaceByHash.get(hash) || '').trim() : '';
  if (mapped) return mapped;
  return String(workspaceName || '').trim();
}

function activityTime(session) {
  const raw = session?.updated_at || session?.last_activity_at || session?.lastActivityAt || session?.created_at || '';
  const t = Date.parse(raw);
  return Number.isFinite(t) ? t : 0;
}

// 把单条 session 转成 tray item 的形态。
function toTrayItem(session, { workspaceName, workspaceByHash }) {
  return {
    session_id: sessionId(session),
    workspace_hash: sessionWorkspaceHash(session),
    title: safeTitle(sessionDisplayTitle(session, '')),
    // subtitle 对齐 Codex 托盘:优先显示 workspace 名。
    subtitle: truncateSubtitle(workspaceSubtitle(session, { workspaceName, workspaceByHash })),
  };
}

function uniqueSessions(list) {
  const out = [];
  const seen = new Set();
  for (const s of Array.isArray(list) ? list : []) {
    const key = sessionKey(s);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push(s);
  }
  return out;
}

// 把 sessions + pinned state + workspaceName 拼成发给 native 的 payload。
// 不在前端按 3 条预截断;native 需要完整数组来渲染各自的 More 子菜单。
export function buildTrayMenuPayload({
  sessions,
  pinnedSessionIds,
  pinnedByWorkspace,
  pinnedOrderItems,
  workspaceName,
  workspaces,
}) {
  const list = withNewSessionDisplayTitles(Array.isArray(sessions) ? sessions : []);
  const workspaceByHash = workspaceNameMap(workspaces);
  const pinEntries = pinnedEntries(pinnedByWorkspace);
  const hasWorkspacePins = pinEntries.length > 0;
  const fallbackPinnedIds = new Set(
    Array.isArray(pinnedSessionIds) ? pinnedSessionIds.map(String).filter(Boolean) : []
  );
  const pinnedKeys = pinnedKeySet(pinnedByWorkspace);

  const pinnedSessions = hasWorkspacePins
    ? pinnedSessionsForList(list, pinnedByWorkspace, normalizePinnedOrderItems(pinnedOrderItems))
    : list.filter((s) => fallbackPinnedIds.has(sessionId(s)));

  const pinned = [];
  const emittedPinned = new Set();
  for (const s of pinnedSessions) {
    const key = sessionKey(s);
    if (!key || emittedPinned.has(key)) continue;
    emittedPinned.add(key);
    pinned.push(toTrayItem(s, { workspaceName, workspaceByHash }));
  }

  const recent = [];
  const emittedRecent = new Set();
  const recentSource = uniqueSessions(list)
    .map((s, index) => ({ s, index }))
    .sort((a, b) => {
      const delta = activityTime(b.s) - activityTime(a.s);
      return delta || (a.index - b.index);
    })
    .map((entry) => entry.s);

  for (const s of recentSource) {
    const id = sessionId(s);
    const key = sessionKey(s);
    if (!id || !key) continue;
    if (emittedPinned.has(key) || pinnedKeys.has(key) || fallbackPinnedIds.has(id)) continue;
    if (emittedRecent.has(key)) continue;
    emittedRecent.add(key);
    recent.push(toTrayItem(s, { workspaceName, workspaceByHash }));
  }

  return {
    workspace_name: String(workspaceName || ''),
    pinned,
    recent,
  };
}

// 内部 debounce timer + 最后一次 payload。每次调度只保留最新值。
let _debounceTimer = null;
let _lastQueuedPayload = null;

// 把 setTimeout / clearTimeout 注入做成 testable 的可替换 hook(测试中可换 fake clock)。
let _setTimeout = (fn, ms) => setTimeout(fn, ms);
let _clearTimeout = (id) => clearTimeout(id);
export function __setTimerFnsForTest({ setTimeout: st, clearTimeout: ct }) {
  if (st) _setTimeout = st;
  if (ct) _clearTimeout = ct;
}
export function __resetTimerFnsForTest() {
  _setTimeout = (fn, ms) => setTimeout(fn, ms);
  _clearTimeout = (id) => clearTimeout(id);
}

function flushNow() {
  _debounceTimer = null;
  const p = _lastQueuedPayload;
  _lastQueuedPayload = null;
  if (!p) return;
  const fn = typeof window !== 'undefined' && window.aceDesktop_setTrayMenu;
  if (typeof fn !== 'function') return; // 浏览器模式 / bridge 不在 → no-op
  try {
    fn(JSON.stringify(p));
  } catch (e) {
    // 静默吞掉 — 桥本身在 desktop 端有 try/catch 兜底,这里再抛会污染前端 console。
  }
}

// 公开入口:把当前 sessions / pinned / workspaceName 100ms debounce 后推给 native。
// 多次连续调用合并成一次 — 仅最后一次的 payload 落到 native。
export function pushTrayMenu({
  sessions,
  pinnedSessionIds,
  pinnedByWorkspace,
  pinnedOrderItems,
  workspaceName,
  workspaces,
}) {
  _lastQueuedPayload = buildTrayMenuPayload({
    sessions,
    pinnedSessionIds,
    pinnedByWorkspace,
    pinnedOrderItems,
    workspaceName,
    workspaces,
  });
  if (_debounceTimer != null) _clearTimeout(_debounceTimer);
  _debounceTimer = _setTimeout(flushNow, 100);
}

// 测试用:取最近一次"待 flush"的 payload(同步,不触发 flush)。
export function __peekQueuedPayloadForTest() {
  return _lastQueuedPayload;
}
export function __resetForTest() {
  if (_debounceTimer != null) _clearTimeout(_debounceTimer);
  _debounceTimer = null;
  _lastQueuedPayload = null;
}
