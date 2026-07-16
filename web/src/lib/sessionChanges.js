// 审查视图的核心数据源:把当前 session 的 messages 数组里所有
// `tool_end` 携带的 hunks 字段聚合成 per-file 的变更列表。
//
// 协议参考:enhance-webui-chat-rendering 给 tool_end payload 加了
// `hunks: [{old_start, old_count, new_start, new_count, lines: [...]}]`,
// 文件名和加删行来自 tool summary。前端聚合时会把这些字段回填到 hunk 上。
//
// **已知 limitation**: 用户授权 agent 用 `bash sed` / `awk` / `git checkout` 改的
// 文件**抓不到**(无 hunks metadata)。前端面板的空态 / 头部应该明示此限制。
// 后续 follow-up 思路见 openspec/changes/add-webui-side-panel/proposal.md。
//
// 输入(messages 形状,REST `/messages` 与 WS `tool_end` 都满足):
//   [
//     { role:'tool', file:'src/foo.cpp', additions:1, deletions:0, hunks:[...] },
//     ...
//   ]
//   message 没有 hunks 字段就跳过(普通 user/assistant/system 消息不参与)。
//
// 输出:
//   [
//     {
//       file: 'src/foo.cpp',
//       hunks: [hunk1, hunk2, ...],     // 同名多次编辑全部追加,按消息顺序
//       totalAdditions: N,
//       totalDeletions: M,
//     },
//     ...
//   ]
// 顺序 = 文件名首次出现的消息顺序(稳定),便于 UI 跟踪 agent 的改动轨迹。

function finiteNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) ? n : 0;
}

/**
 * @param {Array<{hunks?: Array<{file:string, additions?:number, deletions?:number, [k:string]:any}>}>} messages
 * @returns {Array<{file:string, hunks:Array<object>, totalAdditions:number, totalDeletions:number}>}
 */
export function aggregateHunksFromMessages(messages) {
  if (!Array.isArray(messages)) return [];

  // file → group{file, hunks[], totalAdditions, totalDeletions}
  // 用 Map 保持插入顺序(= 文件名首次出现顺序)
  const groups = new Map();

  for (const m of messages) {
    if (!m || !Array.isArray(m.hunks)) continue;
    const messageFile = typeof m.file === 'string' ? m.file : '';
    const messageAdditions = finiteNumber(m.additions);
    const messageDeletions = finiteNumber(m.deletions);
    let messageStatsApplied = false;
    const hasPerHunkStats = m.hunks.some((h) => h && (h.additions != null || h.deletions != null));

    for (const h of m.hunks) {
      const file = (h && typeof h.file === 'string' && h.file)
        ? h.file
        : messageFile;
      if (!h || !file) continue;
      let g = groups.get(file);
      if (!g) {
        g = {
          file,
          hunks: [],
          totalAdditions: 0,
          totalDeletions: 0,
        };
        groups.set(file, g);
      }
      g.hunks.push(h.file ? h : { ...h, file });
      if (h.additions != null || h.deletions != null) {
        g.totalAdditions += finiteNumber(h.additions);
        g.totalDeletions += finiteNumber(h.deletions);
      }
      if (!hasPerHunkStats && !messageStatsApplied && file === messageFile) {
        g.totalAdditions += messageAdditions;
        g.totalDeletions += messageDeletions;
        messageStatsApplied = true;
      }
    }
  }

  return Array.from(groups.values());
}

function metricValue(metrics, label) {
  if (!Array.isArray(metrics)) return 0;
  const item = metrics.find((m) => m && String(m.label || '') === label);
  return finiteNumber(item?.value);
}

function toolFileName(tool) {
  const summaryObject = tool?.summary && typeof tool.summary.object === 'string'
    ? tool.summary.object.trim()
    : '';
  if (summaryObject) return summaryObject;
  for (const key of ['displayOverride', 'title']) {
    const value = typeof tool?.[key] === 'string' ? tool[key].trim() : '';
    if (value) return value;
  }
  return 'change';
}

function hunkMessageFromToolItem(item) {
  if (item?.kind !== 'tool'
    || !Array.isArray(item.tool?.hunks)
    || item.tool.hunks.length === 0) {
    return null;
  }
  const file = toolFileName(item.tool);
  const metrics = item.tool?.summary?.metrics || [];
  return {
    file,
    additions: metricValue(metrics, '+'),
    deletions: metricValue(metrics, '-'),
    hunks: item.tool.hunks.map((hunk) => (
      hunk?.file ? hunk : { ...hunk, file }
    )),
  };
}

/**
 * 从 ChatView 的 transcript items 中抽取可参与 diff 汇总的 tool hunks。
 * @param {Array<{kind?:string, tool?:{hunks?:Array<object>}}>} items
 * @returns {Array<{hunks:Array<object>}>}
 */
export function collectHunkMessagesFromItems(items) {
  if (!Array.isArray(items)) return [];
  return items
    .map((item) => hunkMessageFromToolItem(item))
    .filter(Boolean);
}

function normalizedChangedPath(value) {
  return typeof value === 'string'
    ? value.trim().replace(/\\/g, '/').replace(/^\/\/\?\//, '')
    : '';
}

function successfulToolChangedPaths(item) {
  const tool = item?.kind === 'tool' ? item.tool : null;
  if (!tool?.isDone || tool.success !== true) return [];

  const paths = [];
  const add = (value) => {
    const path = normalizedChangedPath(value);
    if (path) paths.push(path);
  };
  for (const hunk of Array.isArray(tool.hunks) ? tool.hunks : []) add(hunk?.file);

  // file_write/edit 的空文件写入等场景可能没有 hunk，回退到调用参数或摘要。
  if (paths.length === 0 && (tool.tool === 'file_write' || tool.tool === 'file_edit')) {
    add(tool.summary?.object);
    add(tool.args?.path);
    add(tool.args?.file_path);
    add(tool.args?.file);
  }
  return paths;
}

/**
 * 提取最后一个用户回合之后成功修改过的文件。只用于 busy true -> false 的
 * 一次性自动刷新，失败工具与历史回合不得触发当前详情重载。
 */
export function latestTurnSuccessfulChangedFiles(items) {
  if (!Array.isArray(items)) return [];
  let start = -1;
  for (let index = items.length - 1; index >= 0; index -= 1) {
    const item = items[index];
    if (item?.kind === 'msg' && item.role === 'user') {
      start = index;
      break;
    }
  }
  if (start < 0) return [];

  const result = [];
  const seen = new Set();
  for (let index = start + 1; index < items.length; index += 1) {
    for (const path of successfulToolChangedPaths(items[index])) {
      if (seen.has(path)) continue;
      seen.add(path);
      result.push(path);
    }
  }
  return result;
}

/**
 * @param {Array<{file:string,totalAdditions?:number,totalDeletions?:number}>} groups
 * @returns {{fileCount:number,totalAdditions:number,totalDeletions:number,hasChanges:boolean}}
 */
export function summarizeChangeGroups(groups) {
  const list = Array.isArray(groups) ? groups : [];
  const summary = {
    fileCount: list.length,
    totalAdditions: 0,
    totalDeletions: 0,
    hasChanges: list.length > 0,
  };
  for (const group of list) {
    summary.totalAdditions += finiteNumber(group?.totalAdditions);
    summary.totalDeletions += finiteNumber(group?.totalDeletions);
  }
  return summary;
}

function previewText(content) {
  return String(content || '').replace(/\s+/g, ' ').trim().slice(0, 80);
}

function hashString(value) {
  const text = String(value || '');
  let hash = 2166136261;
  for (let i = 0; i < text.length; i += 1) {
    hash ^= text.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(36);
}

function hunkLinesSignature(hunk) {
  if (!Array.isArray(hunk?.lines)) return '';
  return hashString(hunk.lines.map((line) => [
    line?.kind || '',
    line?.text ?? '',
  ].join('\u0000')).join('\u0001'));
}

function hunkSignature(hunk) {
  if (!hunk || typeof hunk !== 'object') return '';
  return [
    hunk.file || '',
    hunk.old_start ?? '',
    hunk.old_count ?? '',
    hunk.new_start ?? '',
    hunk.new_count ?? '',
    Array.isArray(hunk.lines) ? hunk.lines.length : 0,
    hunkLinesSignature(hunk),
  ].join(':');
}

export function changeGroupsSignature(groups) {
  const list = Array.isArray(groups) ? groups : [];
  return list.map((group) => [
    group.file || '',
    finiteNumber(group.totalAdditions),
    finiteNumber(group.totalDeletions),
    Array.isArray(group.hunks) ? group.hunks.map(hunkSignature).join(',') : '',
  ].join('@')).join('|');
}

export function changeSetSignature(changeSet) {
  if (!changeSet || typeof changeSet !== 'object') return '';
  const anchor = changeSet.userMessageId || changeSet.userItemId || changeSet.afterItemId || 'orphan';
  return `${anchor}::${changeGroupsSignature(changeSet.groups)}`;
}

export function collectTurnChangeSetsFromItems(items) {
  if (!Array.isArray(items)) return [];

  const sets = [];
  let current = null;

  const flush = () => {
    if (!current || current.messages.length === 0) {
      current = null;
      return;
    }
    const groups = aggregateHunksFromMessages(current.messages);
    const summary = summarizeChangeGroups(groups);
    if (summary.hasChanges) {
      const set = {
        key: '',
        userItemId: current.userItemId,
        userMessageId: current.userMessageId,
        afterItemId: current.afterItemId,
        title: current.preview || '本轮变更',
        groups,
        summary,
      };
      set.key = changeSetSignature(set);
      sets.push(set);
    }
    current = null;
  };

  for (const item of items) {
    if (item?.kind === 'msg' && item.role === 'user') {
      flush();
      current = {
        userItemId: item.id,
        userMessageId: item.messageId || '',
        preview: previewText(item.content),
        afterItemId: item.id,
        messages: [],
      };
      continue;
    }

    const hunkMessage = hunkMessageFromToolItem(item);
    if (!hunkMessage) continue;
    if (!current) {
      current = {
        userItemId: '',
        userMessageId: '',
        preview: '',
        afterItemId: item.id,
        messages: [],
      };
    }
    current.afterItemId = item.id;
    current.messages.push(hunkMessage);
  }

  flush();
  return sets;
}
