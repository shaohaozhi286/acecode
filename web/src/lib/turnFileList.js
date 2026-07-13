// 每轮对话末尾「本轮改动文件」列表(TurnFileList)的纯逻辑。
//
// 数据来自 sessionChanges.js::collectTurnChangeSetsFromItems 产出的 groups
// (回合级 file_edit / file_write hunks 聚合);这里只负责三件事:
//   1. turnFileDisplayPath: 单一展示路径 —— 文件在会话 cwd 之内显示相对
//      路径,之外保持绝对路径(不再拆 name/parent 两段);
//   2. buildTurnFileItems: group → 展示条目(displayPath + 加删行数);
//   3. splitTurnFileItems: >阈值 时折叠,只露前 N 个,其余算 hiddenCount,
//      对应 UI 的「展开查看剩余 x 个文件」。

export const TURN_FILE_LIST_COLLAPSE_THRESHOLD = 3;

function normalizeSlashes(path) {
  return String(path || '').replace(/\\/g, '/');
}

function isDrivePath(path) {
  return /^[A-Za-z]:\//.test(path);
}

/**
 * 文件在 cwd 之内 → 相对路径;之外(或本来就是相对路径)→ 原样返回。
 * Windows 盘符路径做大小写不敏感前缀比较(NTFS 大小写不敏感,且后端
 * canonical 化后盘符大小写可能与 cwd 不一致);POSIX 路径保持大小写敏感。
 *
 * @param {string} file hunks 的 file 字段(相对或绝对,分隔符不定)
 * @param {string} cwd 会话工作目录
 * @returns {string} 展示用路径(斜杠归一化)
 */
export function turnFileDisplayPath(file, cwd) {
  const normFile = normalizeSlashes(file);
  if (!normFile) return '';
  const normCwd = normalizeSlashes(cwd).replace(/\/+$/, '');
  if (!normCwd) return normFile;
  const caseInsensitive = isDrivePath(normFile) || isDrivePath(normCwd);
  const fileCmp = caseInsensitive ? normFile.toLowerCase() : normFile;
  const cwdCmp = caseInsensitive ? normCwd.toLowerCase() : normCwd;
  if (fileCmp.startsWith(cwdCmp + '/')) {
    return normFile.slice(normCwd.length + 1) || normFile;
  }
  return normFile;
}

function finiteNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) ? n : 0;
}

/**
 * @param {Array<{file:string,totalAdditions?:number,totalDeletions?:number}>} groups
 * @param {string} cwd 会话工作目录(相对化展示路径用)
 * @returns {Array<{file:string, displayPath:string, additions:number, deletions:number}>}
 */
export function buildTurnFileItems(groups, cwd = '') {
  if (!Array.isArray(groups)) return [];
  const items = [];
  for (const group of groups) {
    const file = group && typeof group.file === 'string' ? group.file : '';
    if (!file) continue;
    items.push({
      file,
      displayPath: turnFileDisplayPath(file, cwd),
      additions: finiteNumber(group.totalAdditions),
      deletions: finiteNumber(group.totalDeletions),
    });
  }
  return items;
}

/**
 * 折叠切分:条目数 > threshold 且未展开时只保留前 threshold 个。
 * 注意边界:恰好等于 threshold 时不折叠(折叠只剩 1 个可见没有意义,
 * 且「展开查看剩余 0 个」是病句)。
 *
 * @param {Array<object>} items buildTurnFileItems 的输出
 * @param {boolean} expanded 用户是否已点开「展开查看剩余 x 个文件」
 * @param {number} threshold 折叠阈值(默认 3)
 * @returns {{visible:Array<object>, hiddenCount:number, collapsible:boolean}}
 *   collapsible 表示列表整体是否具备折叠能力(展开后据此显示「收起」)。
 */
export function splitTurnFileItems(items, expanded, threshold = TURN_FILE_LIST_COLLAPSE_THRESHOLD) {
  const list = Array.isArray(items) ? items : [];
  const limit = Number.isFinite(threshold) && threshold > 0
    ? Math.floor(threshold)
    : TURN_FILE_LIST_COLLAPSE_THRESHOLD;
  const collapsible = list.length > limit;
  if (!collapsible || expanded) {
    return { visible: list.slice(), hiddenCount: 0, collapsible };
  }
  return {
    visible: list.slice(0, limit),
    hiddenCount: list.length - limit,
    collapsible,
  };
}
