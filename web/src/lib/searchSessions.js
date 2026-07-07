// SearchPalette 的纯逻辑层:打分排序 + 日历分组相对时间。
//
// 加权规则(design.md D2):
//   title 子串 + 前缀匹配:+1000
//   title 包含子串(非前缀):+500
//   summary 包含子串:+200
//   user-message content match:+150
//   workspaceName 包含子串:+100
//   title 中所有查询字符按顺序出现(fuzzy 兜底):+50
//   同档内按 updated_at 时间衰减加 0~50 浮动(越新越高)
// 不命中(分数 = 0)→ 过滤掉。
// 大小写不敏感;CJK 字符按 codepoint 子串匹配。

import { sessionDisplayTitle } from './sessionTitle.js';

function lower(s) {
  return typeof s === 'string' ? s.toLowerCase() : '';
}

// no_workspace 会话的 key 只用 id:后端搜索结果里它们的 cwd/workspace_hash
// 被有意置空,而 mergeAllWorkspaceSessions 会给列表数据注入 w.cwd/w.hash,
// 两侧字段值不同 —— 掺入 cwd 会让同一会话算出两个 key、在面板里重复出现。
function sessionMergeKey(session = {}) {
  const workspace = session.no_workspace ? 'no-workspace' : (session.workspace_hash || '');
  return `${workspace}::${session.id || ''}`;
}

export function shouldSearchUserMessages(query) {
  return lower(query || '').trim().length > 0;
}

export function mergeSessionContentMatches(sessions = [], matches = []) {
  const merged = [];
  const byKey = new Map();
  for (const session of sessions || []) {
    const copy = { ...session };
    const key = sessionMergeKey(copy);
    byKey.set(key, copy);
    merged.push(copy);
  }
  for (const match of matches || []) {
    if (!match || !match.id) continue;
    const key = sessionMergeKey(match);
    const existing = byKey.get(key);
    if (existing) {
      existing.search_match = match.search_match || null;
      continue;
    }
    const copy = { ...match };
    byKey.set(key, copy);
    merged.push(copy);
  }
  return merged;
}

// fuzzy:query 中的每个字符按顺序在 target 中出现一次即可。
function fuzzyHit(target, query) {
  if (!query) return true;
  if (!target) return false;
  let i = 0;
  for (const ch of target) {
    if (ch === query[i]) {
      i++;
      if (i === query.length) return true;
    }
  }
  return i === query.length;
}

// 把 updated_at 转成 0~50 的衰减分数:24h 内 50,1 周内 30,1 月内 15,更早 0。
export function freshnessScore(updatedAt, now = Date.now()) {
  const ms = typeof updatedAt === 'number' ? updatedAt : Date.parse(updatedAt || '');
  if (!Number.isFinite(ms)) return 0;
  const dayMs = 24 * 3600 * 1000;
  const diff = Math.max(0, now - ms);
  if (diff < dayMs) return 50;
  if (diff < 7 * dayMs) return 30;
  if (diff < 30 * dayMs) return 15;
  return 0;
}

// 给单条 session 打分。query 为 lowercase 后的字符串,空串视为"显示全部"。
// 调用方传入 currentWorkspaceHash 不影响分数,仅用于显示决定;留参以便后续扩展。
export function scoreSession(session, query, now = Date.now()) {
  const title = lower(session?.title || sessionDisplayTitle(session, ''));
  const summary = lower(session?.summary);
  const wsName = lower(session?.workspaceName);
  const searchMatch = session?.search_match;
  const fresh = freshnessScore(session?.updated_at || session?.created_at, now);

  if (!query) {
    return 1 + fresh; // 空查询人人有份,按新鲜度排
  }

  let s = 0;
  if (title) {
    if (title.startsWith(query)) s = Math.max(s, 1000);
    else if (title.includes(query)) s = Math.max(s, 500);
    else if (fuzzyHit(title, query)) s = Math.max(s, 50);
  }
  if (summary && summary.includes(query)) s = Math.max(s, 200);
  if (searchMatch && searchMatch.kind === 'user_message') {
    const snippet = lower(searchMatch.snippet);
    const attachments = Array.isArray(searchMatch.attachments)
      ? lower(searchMatch.attachments.join('\n'))
      : '';
    if (!snippet || snippet.includes(query) || attachments.includes(query)) {
      s = Math.max(s, 150);
    }
  }
  if (wsName && wsName.includes(query)) s = Math.max(s, 100);

  return s > 0 ? s + fresh : 0;
}

// 排序:分数降序,同分按 updated_at 降序。返回新数组,不修改输入。
export function rankSessions(sessions, query, now = Date.now()) {
  const q = lower(query || '').trim();
  const scored = [];
  for (const s of sessions || []) {
    const score = scoreSession(s, q, now);
    if (score > 0) scored.push({ session: s, score });
  }
  scored.sort((a, b) => {
    if (b.score !== a.score) return b.score - a.score;
    const at = Date.parse(a.session.updated_at || a.session.created_at || '') || 0;
    const bt = Date.parse(b.session.updated_at || b.session.created_at || '') || 0;
    return bt - at;
  });
  return scored.map((x) => x.session);
}

// 日历分桶相对时间:今天 / 昨天 / 本周 / 上周 / 上月 / 更早。
// 用本地日历(不是固定窗口)以贴近用户直觉:周一 0:01 看到的"昨天"=周日。
export function searchRelativeTime(updatedAt, now = Date.now()) {
  const ms = typeof updatedAt === 'number' ? updatedAt : Date.parse(updatedAt || '');
  if (!Number.isFinite(ms)) return '';
  const target = new Date(ms);
  const cur = new Date(now);
  const startOfDay = (d) => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  const todayStart = startOfDay(cur);
  const targetDayStart = startOfDay(target);
  const dayMs = 86_400_000;

  if (targetDayStart === todayStart) return '今天';
  if (todayStart - targetDayStart === dayMs) return '昨天';

  // 本周 = 当前 ISO 周(周一为首日)。getDay() 周日=0,转成周一=0 偏移。
  const dayOfWeek = (d) => (d.getDay() + 6) % 7;
  const weekStart = todayStart - dayOfWeek(cur) * dayMs;
  if (targetDayStart >= weekStart) return '本周';
  if (targetDayStart >= weekStart - 7 * dayMs) return '上周';

  // 月份:同年同月 → 本月已被本周覆盖,只剩"上月"与"更早"。
  const lastMonth = new Date(cur.getFullYear(), cur.getMonth() - 1, 1).getTime();
  if (targetDayStart >= lastMonth) return '上月';
  return '更早';
}
