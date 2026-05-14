export const SIDEBAR_SESSION_COLLAPSE_LIMIT = 5;

export function sidebarSessionProjection(sessions = [], expanded = false, limit = SIDEBAR_SESSION_COLLAPSE_LIMIT) {
  const list = Array.isArray(sessions) ? sessions : [];
  const max = Number.isFinite(limit) && limit > 0 ? Math.floor(limit) : SIDEBAR_SESSION_COLLAPSE_LIMIT;
  const collapsible = list.length > max;
  const visibleSessions = collapsible && !expanded ? list.slice(0, max) : list;
  return {
    visibleSessions,
    collapsible,
    action: collapsible ? (expanded ? 'collapse' : 'expand') : '',
    hiddenCount: collapsible && !expanded ? list.length - max : 0,
  };
}
