// 桌面 tray 菜单数据推送。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3。
//
// 浏览器直连 daemon(无 acecode-desktop 桌面壳)模式下 window.aceDesktop_setTrayMenu
// 不存在 → pushTrayMenu no-op。
//
// 节流:同步 100ms debounce(任意 trailing 调用都会落到 native),避免 WS 高频
// session_status 帧把 bridge 调爆;subtitle / pinned 切片在 buildTrayMenuPayload
// 这一纯函数里完成,便于单测。

import { sessionDisplayTitle, withNewSessionDisplayTitles } from './sessionTitle.js';

export const PINNED_LIMIT = 5;
export const RECENT_LIMIT_INCLUDING_MORE = 14;
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

// 把单条 session 转成 tray item 的形态。
function toTrayItem(session, { workspaceName }) {
  return {
    session_id: String(session.id || session.session_id || ''),
    workspace_hash: String(session.workspace_hash || session.workspaceHash || ''),
    title: safeTitle(sessionDisplayTitle(session, '')),
    // subtitle 优先用 summary,其次 workspaceName。空字符串视为没有副标题(native 单行渲染)。
    subtitle: truncateSubtitle(session.summary || workspaceName || ''),
  };
}

// 把 sessions + pinnedSessionIds + workspaceName 拼成发给 native 的 payload。
// sessions 顺序以"按 last_activity 倒序"为前提(由调用方提供)。pinned 按 sessions
// 列表的相对顺序保留,只取 pinnedSessionIds 集合内的;recent 取剩下的前 14 条。
export function buildTrayMenuPayload({ sessions, pinnedSessionIds, workspaceName }) {
  const pins = new Set(
    Array.isArray(pinnedSessionIds) ? pinnedSessionIds.map(String) : []
  );
  const list = withNewSessionDisplayTitles(Array.isArray(sessions) ? sessions : []);
  const pinned = [];
  const recent = [];
  for (const s of list) {
    if (!s) continue;
    const id = String(s.id || s.session_id || '');
    if (!id) continue;
    if (pins.has(id)) {
      if (pinned.length < PINNED_LIMIT) pinned.push(toTrayItem(s, { workspaceName }));
    } else {
      if (recent.length < RECENT_LIMIT_INCLUDING_MORE) recent.push(toTrayItem(s, { workspaceName }));
    }
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
export function pushTrayMenu({ sessions, pinnedSessionIds, workspaceName }) {
  _lastQueuedPayload = buildTrayMenuPayload({ sessions, pinnedSessionIds, workspaceName });
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
