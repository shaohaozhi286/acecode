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
