// 桌面 4 宫格 / 9 宫格的纯数据装配工具。
// 详见 openspec/changes/fix-desktop-grid-show-pinned-only/design.md。
//
// 这里只放 pure data 转换,组件层(Grid4View / Grid9View / ExpandedOverlay)
// 调用本模块,Node assert 单测可独立覆盖所有边界。

// 取出 pinnedIds 列表中、当前 sessions 列表里实际存在的会话,
// 按 pinnedIds 顺序、最多 limit 条返回。
//   - sessions 里没有的 pinned id 跳过(对应已删除会话)
//   - pinnedIds 中的重复 id 去重(只取第一次出现)
//   - sessions 里的非 pinned 会话不会被混入
//   - sessions / pinnedIds / limit 任一为非法值时返回空数组
export function pickPinnedSessionsForGrid(sessions, pinnedIds, limit) {
  const cap = Number.isFinite(limit) && limit > 0 ? Math.floor(limit) : 0;
  if (cap <= 0) return [];
  if (!Array.isArray(sessions) || sessions.length === 0) return [];
  if (!Array.isArray(pinnedIds) || pinnedIds.length === 0) return [];

  const byId = new Map();
  for (const s of sessions) {
    if (!s || typeof s !== 'object') continue;
    const id = String(s.id || s.session_id || s.sessionId || '');
    if (!id) continue;
    if (!byId.has(id)) byId.set(id, s);
  }

  const out = [];
  const seen = new Set();
  for (const raw of pinnedIds) {
    if (out.length >= cap) break;
    const id = String(raw || '');
    if (!id || seen.has(id)) continue;
    seen.add(id);
    const session = byId.get(id);
    if (!session) continue; // 跳过指向已删除会话的 stale id
    out.push(session);
  }
  return out;
}

// 把"服务端给宫格的 session payload"(snake_case 为主)展开成 ChatView
// 期望的 sessionRef(camelCase)。两种命名同时给:能读到哪种用哪种;
// 都没有就给安全默认值,绝不留 undefined 拼进 URL 里去发 404。
//
// 历史 bug:ExpandedOverlay 之前直接 `session.workspaceHash`,但 server
// 字段是 `workspace_hash`,导致下游 ChatView 把 undefined 拼进
// `/api/workspaces/${workspaceHash}/...` URL,触发 404 workspace not found。
export function sessionRefFromGridPayload(session) {
  if (!session || typeof session !== 'object') return null;

  const sessionId = String(
    session.sessionId || session.session_id || session.id || ''
  );
  if (!sessionId) return null;

  const workspaceHash = String(
    session.workspaceHash || session.workspace_hash || ''
  );

  return {
    sessionId,
    id: session.id || sessionId,
    port: session.port,
    token: session.token,
    contextId: session.contextId || session.context_id || 'default',
    workspaceHash,
    cwd: session.cwd || '',
    active: !!(session.active ?? session.is_active),
    busy: !!(session.busy ?? session.is_busy),
    status: session.status || 'idle',
    displayTitle: session.displayTitle || session.display_title || '',
    title: session.title || '',
    summary: session.summary || '',
    message_count: Number(
      session.message_count ?? session.messageCount ?? 0
    ) || 0,
    created_at: session.created_at || session.createdAt || '',
    updated_at: session.updated_at || session.updatedAt || '',
  };
}
