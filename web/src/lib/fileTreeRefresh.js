// SidePanel 文件树缓存刷新信号。
//
// 文件树会按 cwd 缓存目录列表。空项目第一次打开时根目录可能被缓存成
// []；随后 agent 创建文件后,仅切换 tab/session 不会重新请求目录。这个
// helper 从 transcript items 中提取“已完成工具调用”的稳定签名,供
// SidePanel 在工具完成后自动刷新当前 cwd 的文件树。

function safeString(value) {
  return value == null ? '' : String(value);
}

function completedToolSignature(item) {
  if (!item || item.kind !== 'tool' || !item.tool || !item.tool.isDone) return '';
  const tool = item.tool;
  const hunkCount = Array.isArray(tool.hunks) ? tool.hunks.length : 0;
  return [
    safeString(item.id),
    safeString(tool.tool),
    safeString(tool.toolCallId),
    tool.success === false ? '0' : '1',
    safeString(tool.summary?.object || tool.title || tool.displayOverride),
    hunkCount,
  ].join(':');
}

export function fileTreeRefreshKeyFromItems(items) {
  if (!Array.isArray(items)) return '';
  return items
    .map(completedToolSignature)
    .filter(Boolean)
    .join('|');
}

export function fileTreeReloadPaths(expandedDirs) {
  const paths = [''];
  const seen = new Set(paths);
  if (!expandedDirs || typeof expandedDirs[Symbol.iterator] !== 'function') return paths;
  for (const rawPath of expandedDirs) {
    const path = safeString(rawPath);
    if (!path || seen.has(path)) continue;
    seen.add(path);
    paths.push(path);
  }
  return paths;
}

const FILE_TREE_ROW_FIELDS = ['name', 'path', 'kind'];

export function fileTreeDirectoryEntriesEqual(currentEntries, incomingEntries) {
  if (currentEntries === incomingEntries) return true;
  if (!Array.isArray(currentEntries) || !Array.isArray(incomingEntries)) return false;
  if (currentEntries.length !== incomingEntries.length) return false;

  for (let index = 0; index < currentEntries.length; index += 1) {
    const current = currentEntries[index] || {};
    const incoming = incomingEntries[index] || {};
    for (const field of FILE_TREE_ROW_FIELDS) {
      if (safeString(current[field]) !== safeString(incoming[field])) return false;
    }
  }
  return true;
}

export function reconcileFileTreeDirectory(treeCache, path, incomingEntries) {
  const currentCache = treeCache instanceof Map ? treeCache : new Map();
  const directoryPath = safeString(path);
  const nextEntries = Array.isArray(incomingEntries) ? incomingEntries : [];
  if (
    currentCache.has(directoryPath)
    && fileTreeDirectoryEntriesEqual(currentCache.get(directoryPath), nextEntries)
  ) {
    return currentCache;
  }

  const nextCache = new Map(currentCache);
  nextCache.set(directoryPath, nextEntries);
  return nextCache;
}

export function fileTreeDirectoryRequestKey(cwd, path) {
  return `${safeString(cwd)}\u0000${safeString(path)}`;
}

export function beginFileTreeDirectoryRequest(inFlightRequests, cwd, path) {
  const requestKey = fileTreeDirectoryRequestKey(cwd, path);
  if (inFlightRequests.has(requestKey)) return null;
  inFlightRequests.add(requestKey);
  return requestKey;
}

export function finishFileTreeDirectoryRequest(inFlightRequests, requestKey) {
  inFlightRequests.delete(requestKey);
}
