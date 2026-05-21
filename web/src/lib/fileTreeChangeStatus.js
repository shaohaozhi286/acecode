const STATUS_LABELS = {
  M: '已修改',
  U: '新增',
  D: '已删除',
};

const STATUS_PRIORITY = {
  U: 1,
  M: 2,
  D: 3,
};

function finiteNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) ? n : 0;
}

export function normalizeTreePath(path) {
  return String(path || '')
    .replace(/\\/g, '/')
    .replace(/^\.\/+/, '')
    .replace(/\/+/g, '/')
    .replace(/^\/+/, '')
    .replace(/\/+$/, '');
}

function normalizeStatus(status) {
  const s = String(status || '').toUpperCase();
  return Object.prototype.hasOwnProperty.call(STATUS_LABELS, s) ? s : '';
}

function strongerStatus(current, candidate) {
  const a = normalizeStatus(current);
  const b = normalizeStatus(candidate);
  if (!b) return a;
  if (!a || STATUS_PRIORITY[b] > STATUS_PRIORITY[a]) return b;
  return a;
}

function parentPathFor(path) {
  const normalized = normalizeTreePath(path);
  const idx = normalized.lastIndexOf('/');
  return idx < 0 ? '' : normalized.slice(0, idx);
}

function baseNameFor(path) {
  const normalized = normalizeTreePath(path);
  const idx = normalized.lastIndexOf('/');
  return idx < 0 ? normalized : normalized.slice(idx + 1);
}

function isDescendantPath(child, parent) {
  const c = normalizeTreePath(child);
  const p = normalizeTreePath(parent);
  if (!c) return false;
  if (!p) return c.includes('/');
  return c.startsWith(`${p}/`);
}

export function fileChangeStatusLabel(status) {
  return STATUS_LABELS[normalizeStatus(status)] || '';
}

export function fileChangeStatusTitle(status, isDir = false) {
  const label = fileChangeStatusLabel(status);
  if (!label) return '';
  return isDir ? `目录内有变更: ${label}` : label;
}

export function reviewStatusForGroup(group) {
  const explicit = normalizeStatus(group?.status || group?.review_status);
  if (explicit) return explicit;

  const additions = finiteNumber(group?.totalAdditions);
  const deletions = finiteNumber(group?.totalDeletions);
  const hunks = Array.isArray(group?.hunks) ? group.hunks : [];
  const hasHunks = hunks.length > 0;
  const oldSideEmpty = hasHunks && hunks.every((hunk) => finiteNumber(hunk?.old_count) === 0);
  const newSideEmpty = hasHunks && hunks.every((hunk) => finiteNumber(hunk?.new_count) === 0);

  if (additions > 0 && deletions === 0 && oldSideEmpty) return 'U';
  if (deletions > 0 && additions === 0 && newSideEmpty) return 'D';
  if (additions > 0 || deletions > 0 || hasHunks) return 'M';
  return '';
}

export function buildReviewStatusMap(groups) {
  const statuses = new Map();
  if (!Array.isArray(groups)) return statuses;
  for (const group of groups) {
    const path = normalizeTreePath(group?.file);
    const status = reviewStatusForGroup(group);
    if (!path || !status) continue;
    statuses.set(path, strongerStatus(statuses.get(path), status));
  }
  return statuses;
}

export function statusForTreeEntry(entry, statusByPath) {
  if (!entry || !(statusByPath instanceof Map)) return '';
  const path = normalizeTreePath(entry.path);
  let status = normalizeStatus(statusByPath.get(path));
  if (entry.kind !== 'dir') return status;

  for (const [changedPath, changedStatus] of statusByPath.entries()) {
    if (isDescendantPath(changedPath, path)) {
      status = strongerStatus(status, changedStatus);
    }
  }
  return status;
}

export function entriesWithReviewRows(entries, parentPath, statusByPath) {
  const out = Array.isArray(entries) ? entries.map((entry) => ({ ...entry })) : [];
  if (!(statusByPath instanceof Map) || statusByPath.size === 0) return out;

  const currentParent = normalizeTreePath(parentPath);
  const existing = new Set(out.map((entry) => normalizeTreePath(entry?.path)));
  let changed = false;

  for (const [changedPath, status] of statusByPath.entries()) {
    if (parentPathFor(changedPath) !== currentParent) continue;
    if (existing.has(changedPath)) continue;

    const name = baseNameFor(changedPath);
    if (!name) continue;
    out.push({
      name,
      path: changedPath,
      kind: 'file',
      review_status: status,
      review_synthetic: true,
    });
    existing.add(changedPath);
    changed = true;
  }

  if (!changed) return out;
  return out.sort((a, b) => {
    if (a.kind !== b.kind) return a.kind === 'dir' ? -1 : 1;
    return String(a.name || '').localeCompare(String(b.name || ''));
  });
}
