import { transcriptTimestampMs } from './timestamps.js';
import { formatCount } from './format.js';

function isUserMessage(item) {
  return item?.kind === 'msg' && item.role === 'user';
}

function isAssistantMessage(item) {
  return item?.kind === 'msg' && item.role === 'assistant';
}

function assistantHasText(item) {
  return isAssistantMessage(item) && String(item.content || '').trim().length > 0;
}

function isEmptyAssistantMessage(item) {
  return isAssistantMessage(item) && !assistantHasText(item);
}

function isStreamingAssistant(item) {
  return isAssistantMessage(item) && !!item.streaming;
}

function isToolItem(item) {
  return item?.kind === 'tool';
}

function isAskUserQuestionResultTool(item) {
  return isToolItem(item)
    && Array.isArray(item.tool?.askUserQuestionResult?.items)
    && item.tool.askUserQuestionResult.items.length > 0;
}

function isToolTranscriptMessage(item) {
  if (item?.kind !== 'msg') return false;
  const role = String(item.role || '').toLowerCase();
  return role === 'tool_call' || role === 'tool_result' || role === 'tool';
}

function isToolCallTranscriptMessage(item) {
  return item?.kind === 'msg' && String(item.role || '').toLowerCase() === 'tool_call';
}

function transcriptToolName(item) {
  const direct = item?.tool || item?.tool_name || item?.name || item?.metadata?.tool || item?.metadata?.tool_name;
  if (direct) return String(direct).trim().toLowerCase();
  const content = String(item?.content || '');
  const match = content.match(/\[Tool:\s*([^\]\s]+)\s*\]/i);
  return match ? String(match[1] || '').trim().toLowerCase() : '';
}

function isTaskCompleteToolCallMessage(item) {
  if (!isToolTranscriptMessage(item)) return false;
  const role = String(item.role || '').toLowerCase();
  if (role !== 'tool_call') return false;
  const name = transcriptToolName(item);
  return name === 'task_complete' || name === 'complete';
}

function isToolTranscriptResultMessage(item) {
  if (item?.kind !== 'msg') return false;
  const role = String(item.role || '').toLowerCase();
  return role === 'tool_result' || role === 'tool';
}

function toolCallIdForItem(item) {
  const raw = item?.tool?.toolCallId
    ?? item?.tool?.tool_call_id
    ?? item?.toolCallId
    ?? item?.tool_call_id
    ?? item?.metadata?.toolCallId
    ?? item?.metadata?.tool_call_id;
  const id = String(raw || '').trim();
  return id || '';
}

function isTaskCompleteTool(item) {
  const tool = item?.tool || {};
  if (!isToolItem(item)) return false;
  const name = normalizedToolName(tool);
  const verb = normalizedVerb(tool);
  const object = summaryObject(tool, '').toLowerCase();
  return tool.isTaskComplete
    || name === 'task_complete'
    || name === 'complete'
    || (verb === 'complete' && (object === 'task' || !!metricText(tool.summary?.metrics, 'summary')));
}

function isSuccessfulTaskComplete(item) {
  if (!isTaskCompleteTool(item)) return false;
  const tool = item.tool || {};
  return tool.isDone === true && tool.success !== false;
}

function isCompletedCollapsibleTool(item) {
  return isToolItem(item)
    && !isTaskCompleteTool(item)
    && !isAskUserQuestionResultTool(item)
    && item.tool?.isDone === true;
}

function isProcessedActivityItem(item) {
  if (isStreamingAssistant(item)) return false;
  if (isTaskCompleteTool(item)) return false;
  if (isAssistantMessage(item)) return true;
  if (isToolTranscriptMessage(item)) return true;
  return isCompletedCollapsibleTool(item);
}

function itemTimestamp(item) {
  return transcriptTimestampMs(item);
}

function formatDurationMs(durationMs) {
  if (!Number.isFinite(durationMs) || durationMs <= 0) return '';
  const seconds = Math.max(0, Math.round(durationMs / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const rest = seconds % 60;
  return `${minutes}m ${rest}s`;
}

function metricValue(metrics, label) {
  if (!Array.isArray(metrics)) return 0;
  for (const metric of metrics) {
    if (Array.isArray(metric) && String(metric[0] || '') === label) {
      const n = Number(metric[1]);
      return Number.isFinite(n) ? n : 0;
    }
    if (metric && typeof metric === 'object' && String(metric.label || '') === label) {
      const n = Number(metric.value);
      return Number.isFinite(n) ? n : 0;
    }
  }
  return 0;
}

function metricText(metrics, label) {
  if (!Array.isArray(metrics)) return '';
  const wanted = String(label || '').toLowerCase();
  for (const metric of metrics) {
    if (Array.isArray(metric) && String(metric[0] || '').toLowerCase() === wanted) {
      return cleanSummaryText(metric[1]);
    }
    if (metric && typeof metric === 'object') {
      const key = String(metric.label || metric.key || metric.name || '').toLowerCase();
      if (key === wanted) return cleanSummaryText(metric.value);
    }
  }
  return '';
}

function cleanSummaryText(value) {
  return String(value ?? '').replace(/\s+/g, ' ').trim();
}

function rawSummaryText(value) {
  return String(value ?? '').trim();
}

function metricMarkdownText(metrics, label) {
  if (!Array.isArray(metrics)) return '';
  const wanted = String(label || '').toLowerCase();
  for (const metric of metrics) {
    if (Array.isArray(metric) && String(metric[0] || '').toLowerCase() === wanted) {
      return rawSummaryText(metric[1]);
    }
    if (metric && typeof metric === 'object') {
      const key = String(metric.label || metric.key || metric.name || '').toLowerCase();
      if (key === wanted) return rawSummaryText(metric.value);
    }
  }
  return '';
}

function normalizedVerb(tool) {
  return String(tool?.summary?.verb || '').trim().toLowerCase();
}

function normalizedToolName(tool) {
  return String(tool?.tool || '').trim().toLowerCase();
}

function summaryObject(tool, fallback) {
  const object = String(tool?.summary?.object || '').trim();
  return object || fallback;
}

function countObject(set, fallbackPrefix, index, object) {
  set.add(object || `${fallbackPrefix}:${index}`);
}

function countTranscriptOnlyTools(items) {
  const resultRows = items.filter((item) => isToolTranscriptResultMessage(item)).length;
  if (resultRows > 0) return resultRows;
  return items.filter((item) => isToolTranscriptMessage(item)).length;
}

function isFileToolName(name) {
  return name === 'file_read' || name === 'file_edit' || name === 'file_write';
}

function summarizeToolItems(items) {
  const created = new Set();
  const edited = new Set();
  const read = new Set();
  let commands = 0;
  let nonFileTools = 0;

  items.forEach((item, index) => {
    if (!isToolItem(item) || isTaskCompleteTool(item)) return;
    const tool = item.tool || {};
    const verb = normalizedVerb(tool);
    const name = normalizedToolName(tool);
    const object = summaryObject(tool, String(item.id || index));
    const mayDescribeFile = !name || isFileToolName(name);
    let countedAsFile = false;

    if (mayDescribeFile && verb === 'created') {
      countObject(created, 'created', index, object);
      countedAsFile = true;
    } else if (mayDescribeFile && (verb === 'wrote' || verb === 'edited' || verb === 'edit')) {
      countObject(edited, 'edited', index, object);
      countedAsFile = true;
    } else if (mayDescribeFile && verb === 'read') {
      countObject(read, 'read', index, object);
      countedAsFile = true;
    } else if (verb === 'ran' || name === 'bash') {
      commands += 1;
    } else if (name === 'file_write') {
      const additions = metricValue(tool.summary?.metrics, '+');
      if (additions > 0 && metricValue(tool.summary?.metrics, '-') === 0) {
        countObject(created, 'created', index, object);
      } else {
        countObject(edited, 'edited', index, object);
      }
      countedAsFile = true;
    } else if (name === 'file_edit') {
      countObject(edited, 'edited', index, object);
      countedAsFile = true;
    } else if (name === 'file_read') {
      countObject(read, 'read', index, object);
      countedAsFile = true;
    }

    if (!countedAsFile && !isFileToolName(name)) {
      nonFileTools += 1;
    }
  });

  if (nonFileTools === 0 && created.size === 0 && edited.size === 0 && read.size === 0 && commands === 0) {
    nonFileTools = countTranscriptOnlyTools(items);
  }

  const parts = [];
  if (created.size > 0) parts.push(`已创建 ${formatCount(created.size, 'files')}`);
  if (edited.size > 0) parts.push(`已编辑 ${formatCount(edited.size, 'files')}`);
  if (read.size > 0) parts.push(`读取 ${formatCount(read.size, 'files')}`);
  if (commands > 0) parts.push(formatCount(commands, 'commandsRun'));
  if (nonFileTools > 0) parts.push(formatCount(nonFileTools, 'toolsCalled'));
  return parts.length > 0 ? parts.join('，') : '已调用工具';
}

function hasCollapsibleToolActivity(items) {
  return items.some(isCompletedCollapsibleTool) || items.some(isToolTranscriptMessage);
}

function coveredIdsForItem(item) {
  if (Array.isArray(item?.coveredItemIds)) {
    return item.coveredItemIds.filter((id) => id !== undefined && id !== null);
  }
  return item?.id !== undefined && item?.id !== null ? [item.id] : [];
}

function coveredIds(items) {
  return items.flatMap(coveredIdsForItem);
}

function collapsedId(mode, items) {
  const ids = coveredIds(items);
  const first = ids.length ? ids[0] : 'start';
  const last = ids.length ? ids[ids.length - 1] : 'end';
  return `activity:${mode}:${first}:${last}`;
}

function collapsedTimestamps(items, fallbackEndItem) {
  const stamps = items.map(itemTimestamp).filter(Boolean);
  const fallbackEnd = itemTimestamp(fallbackEndItem);
  const startTs = stamps.length ? stamps[0] : 0;
  const endTs = fallbackEnd || (stamps.length ? stamps[stamps.length - 1] : 0);
  return { startTs, endTs };
}

function formatPersistedDurationMs(durationMs) {
  if (!Number.isFinite(durationMs) || durationMs < 0) return '';
  const seconds = Math.max(0, Math.round(durationMs / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const rest = seconds % 60;
  return `${minutes}m ${rest}s`;
}

function persistedTurnDurationMs(items) {
  for (const item of items || []) {
    const direct = Number(item?.turnDurationMs);
    if (Number.isFinite(direct) && direct >= 0) return direct;
    const nested = Number(item?.turnTiming?.durationMs ?? item?.metadata?.turnTiming?.durationMs);
    if (Number.isFinite(nested) && nested >= 0) return nested;
  }
  return null;
}

function makeToolSummaryItem(items) {
  const { startTs, endTs } = collapsedTimestamps(items);
  return {
    kind: 'activity_summary',
    mode: 'tools',
    id: collapsedId('tools', items),
    title: summarizeToolItems(items),
    collapsedItems: items.slice(),
    coveredItemIds: coveredIds(items),
    ts: startTs || endTs || Date.now(),
  };
}

function makeProcessedDetailItems(items) {
  if (!Array.isArray(items) || items.length === 0) return [];
  return projectGenericTurn(items, { deferTrailingToolSummary: false });
}

function makeProcessedItem(items, endItem) {
  const { startTs, endTs } = collapsedTimestamps(items, endItem);
  const persistedDuration = persistedTurnDurationMs(items);
  const duration = persistedDuration != null
    ? formatPersistedDurationMs(persistedDuration)
    : (startTs && endTs ? formatDurationMs(endTs - startTs) : '');
  return {
    kind: 'activity_summary',
    mode: 'processed',
    id: collapsedId('processed', items),
    title: duration ? `已处理 ${duration}` : '已处理',
    collapsedItems: items.slice(),
    detailItems: makeProcessedDetailItems(items),
    coveredItemIds: coveredIds(items),
    ts: startTs || endTs || Date.now(),
  };
}

function taskCompleteSummaryText(item) {
  return cleanSummaryText(taskCompleteSummaryMarkdownText(item)) || '已完成';
}

function taskCompleteSummaryMarkdownText(item) {
  const tool = item?.tool || {};
  const summary = tool.summary || {};
  const metricSummary = metricMarkdownText(summary.metrics, 'summary');
  if (metricSummary) return metricSummary;

  const object = rawSummaryText(summary.object);
  if (object && cleanSummaryText(object).toLowerCase() !== 'task') return object;

  const output = rawSummaryText(tool.output);
  if (output) return output;

  const title = rawSummaryText(tool.title || tool.displayOverride);
  if (title && cleanSummaryText(title).toLowerCase() !== 'task_complete') return title;

  return '已完成';
}

function makeCompletionSummaryItem(item) {
  const summary = taskCompleteSummaryMarkdownText(item);
  const titleSummary = cleanSummaryText(summary) || '已完成';
  const id = item?.id ?? (itemTimestamp(item) || 'unknown');
  return {
    kind: 'completion_summary',
    id: `completion:${id}`,
    title: `总结：${titleSummary}`,
    summary,
    sourceItem: item,
    coveredItemIds: coveredIds([item]),
    ts: itemTimestamp(item) || Date.now(),
  };
}

function nearestStructuredToolIndex(indexes, fromIndex) {
  let best = -1;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (const index of indexes || []) {
    const distance = Math.abs(index - fromIndex);
    if (distance < bestDistance) {
      best = index;
      bestDistance = distance;
    }
  }
  return best;
}

function nearbyNonEmptyIndex(items, fromIndex, step) {
  for (let i = fromIndex; i >= 0 && i < items.length; i += step) {
    if (isEmptyAssistantMessage(items[i])) continue;
    return i;
  }
  return -1;
}

function sameOrMissingCallId(a, b) {
  const aId = toolCallIdForItem(a);
  const bId = toolCallIdForItem(b);
  return !aId || !bId || aId === bId;
}

function collectCoveredIds(items) {
  const ids = [];
  const seen = new Set();
  for (const item of items) {
    for (const id of coveredIdsForItem(item)) {
      if (seen.has(id)) continue;
      seen.add(id);
      ids.push(id);
    }
  }
  return ids;
}

function attachCoveredItems(item, extras) {
  const coveredItemIds = collectCoveredIds([
    ...(Array.isArray(extras) ? extras : []),
    item,
  ].sort((a, b) => itemTimestamp(a) - itemTimestamp(b)));
  if (coveredItemIds.length <= coveredIdsForItem(item).length) return item;
  return { ...item, coveredItemIds };
}

function objectMetadata(item) {
  const metadata = item?.metadata;
  return metadata && typeof metadata === 'object' && !Array.isArray(metadata) ? metadata : {};
}

function compactNoticeId(item) {
  if (item?.kind !== 'msg') return '';
  const metadata = objectMetadata(item);
  if (metadata.compact_notice !== true) return '';
  return typeof metadata.compact_notice_id === 'string'
    ? metadata.compact_notice_id.trim()
    : '';
}

function collapseCompletedCompactNoticeGroups(items) {
  const groups = new Map();
  items.forEach((item, index) => {
    const id = compactNoticeId(item);
    if (!id) return;
    const group = groups.get(id) || {
      id,
      indexes: [],
      items: [],
      complete: false,
    };
    group.indexes.push(index);
    group.items.push(item);
    group.complete = group.complete
      || objectMetadata(item).compact_notice_complete === true;
    groups.set(id, group);
  });

  const replacements = new Map();
  const removed = new Set();
  for (const group of groups.values()) {
    if (!group.complete || group.indexes.length === 0) continue;
    const first = group.items[0];
    const syntheticId = `compact-notice:${group.id}`;
    replacements.set(group.indexes[0], {
      ...first,
      kind: 'msg',
      id: syntheticId,
      messageId: syntheticId,
      role: 'system',
      content: group.items.map((item) => String(item?.content ?? '')).join('\n\n'),
      contentParts: [],
      metadata: {
        ...objectMetadata(first),
        compact_notice: true,
        compact_notice_id: group.id,
        compact_notice_stage: 'complete',
        compact_notice_complete: true,
        compact_label: 'Context compacted',
      },
      coveredItemIds: collectCoveredIds(group.items),
      ts: itemTimestamp(first) || Date.now(),
    });
    group.indexes.forEach((index) => removed.add(index));
  }

  const out = [];
  items.forEach((item, index) => {
    if (replacements.has(index)) out.push(replacements.get(index));
    else if (!removed.has(index)) out.push(item);
  });
  return out;
}

function suppressStructuredToolWrappers(items) {
  const structuredById = new Map();
  const structuredIndexes = [];
  items.forEach((item, index) => {
    if (!isToolItem(item)) return;
    structuredIndexes.push(index);
    const id = toolCallIdForItem(item);
    if (!id) return;
    const indexes = structuredById.get(id) || [];
    indexes.push(index);
    structuredById.set(id, indexes);
  });

  const hiddenIndexes = new Set();
  const coveredExtras = new Map();
  const hideWrapperForTool = (wrapperIndex, toolIndex) => {
    if (wrapperIndex < 0 || toolIndex < 0 || hiddenIndexes.has(wrapperIndex)) return;
    hiddenIndexes.add(wrapperIndex);
    const extras = coveredExtras.get(toolIndex) || [];
    extras.push(items[wrapperIndex]);
    coveredExtras.set(toolIndex, extras);
  };

  items.forEach((item, index) => {
    if (!isToolTranscriptMessage(item)) return;
    const id = toolCallIdForItem(item);
    if (!id || !structuredById.has(id)) return;
    hideWrapperForTool(index, nearestStructuredToolIndex(structuredById.get(id), index));
  });

  items.forEach((item, index) => {
    if (!isToolItem(item)) return;

    const previousIndex = nearbyNonEmptyIndex(items, index - 1, -1);
    const previous = previousIndex >= 0 ? items[previousIndex] : null;
    if (
      isToolCallTranscriptMessage(previous)
      && !toolCallIdForItem(previous)
      && sameOrMissingCallId(previous, item)
    ) {
      hideWrapperForTool(previousIndex, index);
    }

    const nextIndex = nearbyNonEmptyIndex(items, index + 1, 1);
    const next = nextIndex >= 0 ? items[nextIndex] : null;
    if (
      isToolTranscriptResultMessage(next)
      && !toolCallIdForItem(next)
      && sameOrMissingCallId(item, next)
    ) {
      hideWrapperForTool(nextIndex, index);
    }
  });

  const claimStructuredTool = (usedIndexes, name) => {
    const wanted = String(name || '').trim().toLowerCase();
    const fallback = [];
    for (const index of structuredIndexes) {
      if (usedIndexes.has(index)) continue;
      const toolName = normalizedToolName(items[index]?.tool);
      if (wanted && toolName === wanted) return index;
      if (!wanted || !toolName) fallback.push(index);
    }
    return fallback.length > 0 ? fallback[0] : -1;
  };

  if (structuredIndexes.length > 0) {
    const usedCallTools = new Set();
    const matchedCallTools = [];
    items.forEach((item, index) => {
      if (!isToolCallTranscriptMessage(item) || toolCallIdForItem(item)) return;
      const toolIndex = claimStructuredTool(usedCallTools, transcriptToolName(item));
      if (toolIndex < 0) return;
      usedCallTools.add(toolIndex);
      matchedCallTools.push(toolIndex);
      hideWrapperForTool(index, toolIndex);
    });

    let resultOrdinal = 0;
    items.forEach((item, index) => {
      if (!isToolTranscriptResultMessage(item) || toolCallIdForItem(item)) return;
      if (resultOrdinal >= matchedCallTools.length) return;
      hideWrapperForTool(index, matchedCallTools[resultOrdinal]);
      resultOrdinal += 1;
    });
  }

  if (hiddenIndexes.size === 0 && coveredExtras.size === 0) return items;

  return items
    .map((item, index) => {
      if (hiddenIndexes.has(index)) return null;
      const extras = coveredExtras.get(index);
      return extras ? attachCoveredItems(item, extras) : item;
    })
    .filter(Boolean);
}

const LEGACY_INVOCATION_LABEL = '工具调用 / 返回';
const MISSING_TOOL_REQUEST_TEXT = '请求未记录（旧记录未保存工具调用参数）';

function legacyRequestText(call, result) {
  const content = String(call?.content || '').trim();
  if (content) return content;
  const name = transcriptToolName(result);
  return name ? `[Tool: ${name}] ${MISSING_TOOL_REQUEST_TEXT}` : MISSING_TOOL_REQUEST_TEXT;
}

function legacyResultText(result) {
  const content = String(result?.content || '').trim();
  return content || '空内容';
}

function legacyInvocationContent(call, result) {
  return [
    '工具调用',
    legacyRequestText(call, result),
    '',
    '工具返回',
    legacyResultText(result),
  ].join('\n');
}

function makeLegacyInvocationItem(call, result, betweenItems) {
  const coveredItems = [call, ...(betweenItems || []), result].filter(Boolean);
  return {
    ...result,
    role: 'tool_result',
    content: legacyInvocationContent(call, result),
    coveredItemIds: collectCoveredIds(coveredItems),
    ts: itemTimestamp(call) || itemTimestamp(result),
    metadata: {
      ...objectMetadata(result),
      legacyToolInvocation: true,
      compact_label: LEGACY_INVOCATION_LABEL,
    },
  };
}

function coalesceAdjacentLegacyWrappers(items) {
  const out = [];
  for (let i = 0; i < items.length; i += 1) {
    const item = items[i];
    if (!isToolCallTranscriptMessage(item) || isTaskCompleteToolCallMessage(item)) {
      out.push(item);
      continue;
    }

    const betweenItems = [];
    let resultIndex = i + 1;
    while (resultIndex < items.length && isEmptyAssistantMessage(items[resultIndex])) {
      betweenItems.push(items[resultIndex]);
      resultIndex += 1;
    }

    const result = items[resultIndex];
    if (
      resultIndex < items.length
      && isToolTranscriptResultMessage(result)
      && sameOrMissingCallId(item, result)
    ) {
      out.push(makeLegacyInvocationItem(item, result, betweenItems));
      i = resultIndex;
      continue;
    }

    out.push(item);
  }
  return out;
}

function coalesceIdMatchedLegacyWrappers(items) {
  const resultIndexesById = new Map();
  items.forEach((item, index) => {
    if (!isToolTranscriptResultMessage(item)) return;
    if (objectMetadata(item).legacyToolInvocation) return;
    const id = toolCallIdForItem(item);
    if (!id) return;
    const indexes = resultIndexesById.get(id) || [];
    indexes.push(index);
    resultIndexesById.set(id, indexes);
  });
  if (resultIndexesById.size === 0) return items;

  const usedResultIndexes = new Set();
  const out = [];
  for (let i = 0; i < items.length; i += 1) {
    if (usedResultIndexes.has(i)) continue;
    const item = items[i];
    if (!isToolCallTranscriptMessage(item) || isTaskCompleteToolCallMessage(item)) {
      out.push(item);
      continue;
    }

    const id = toolCallIdForItem(item);
    if (!id) {
      out.push(item);
      continue;
    }

    const resultIndex = (resultIndexesById.get(id) || []).find((index) => index > i && !usedResultIndexes.has(index));
    if (resultIndex == null) {
      out.push(item);
      continue;
    }

    out.push(makeLegacyInvocationItem(item, items[resultIndex], []));
    usedResultIndexes.add(resultIndex);
  }
  return out;
}

function coalesceResultOnlyLegacyWrappers(items) {
  return items.map((item) => {
    if (!isToolTranscriptResultMessage(item)) return item;
    if (objectMetadata(item).legacyToolInvocation) return item;
    return makeLegacyInvocationItem(null, item, []);
  });
}

function normalizeToolInvocationItems(items) {
  if (!Array.isArray(items) || items.length === 0) return [];
  return coalesceResultOnlyLegacyWrappers(
    coalesceAdjacentLegacyWrappers(
      coalesceIdMatchedLegacyWrappers(suppressStructuredToolWrappers(items)),
    ),
  );
}

function isFinalCollapseSkippable(item) {
  return isEmptyAssistantMessage(item)
    || isToolTranscriptMessage(item)
    || isTaskCompleteToolCallMessage(item);
}

function findLastSignificantIndex(items) {
  for (let i = items.length - 1; i >= 0; i -= 1) {
    if (isFinalCollapseSkippable(items[i])) continue;
    return i;
  }
  return -1;
}

function findPreviousSignificantIndex(items, beforeIndex) {
  for (let i = beforeIndex - 1; i >= 0; i -= 1) {
    if (isFinalCollapseSkippable(items[i])) continue;
    return i;
  }
  return -1;
}

function preservedTurnPrefix(items) {
  return items.filter((item) => isUserMessage(item));
}

function appendFinalProcessedItems(out, items, beforeIndex, endItem) {
  const processed = [];
  const flushProcessed = () => {
    pushProcessedSummary(out, processed, endItem);
    processed.length = 0;
  };

  for (let i = 0; i < beforeIndex; i += 1) {
    const item = items[i];
    if (isUserMessage(item)) continue;
    if (isEmptyAssistantMessage(item)) continue;
    if (isTaskCompleteTool(item) || isTaskCompleteToolCallMessage(item)) continue;
    if (isProcessedActivityItem(item)) {
      processed.push(item);
    } else {
      flushProcessed();
      out.push(item);
    }
  }

  flushProcessed();
}

function pushProcessedSummary(out, processed, endItem) {
  if (processed.length > 0) {
    out.push(makeProcessedItem(processed, endItem));
  }
}

function projectFinalCollapsedTurn(items, options = {}) {
  const lastIndex = findLastSignificantIndex(items);
  if (lastIndex < 0) return null;

  const last = items[lastIndex];
  if (isSuccessfulTaskComplete(last)) {
    const out = preservedTurnPrefix(items);
    const previousIndex = findPreviousSignificantIndex(items, lastIndex);
    const previous = previousIndex >= 0 ? items[previousIndex] : null;
    if (assistantHasText(previous)) {
      appendFinalProcessedItems(out, items, previousIndex, last);
      out.push(previous);
    } else {
      appendFinalProcessedItems(out, items, lastIndex, last);
    }
    out.push(makeCompletionSummaryItem(last));
    return out;
  }

  if (!options.deferTrailingToolSummary && assistantHasText(last) && !isStreamingAssistant(last)) {
    const out = preservedTurnPrefix(items);
    appendFinalProcessedItems(out, items, lastIndex, last);
    out.push(last);
    return out;
  }

  return null;
}

function flushToolBuffer(out, buffer, { collapse = true } = {}) {
  if (buffer.length > 0) {
    if (collapse && hasCollapsibleToolActivity(buffer)) {
      out.push(makeToolSummaryItem(buffer));
    } else if (!collapse && buffer.some(isCompletedCollapsibleTool)) {
      out.push(...buffer.filter((item) => (
        !isToolTranscriptMessage(item) && !isEmptyAssistantMessage(item)
      )));
    } else {
      out.push(...buffer);
    }
    buffer.length = 0;
  }
}

function flushWrapperNoiseBeforeStructuredTool(out, buffer) {
  if (buffer.length === 0) return;
  const onlyWrapperNoise = buffer.every((item) => (
    isToolTranscriptMessage(item) || isEmptyAssistantMessage(item)
  ));
  if (onlyWrapperNoise) {
    buffer.length = 0;
    return;
  }
  flushToolBuffer(out, buffer);
}

function projectGenericTurn(items, options = {}) {
  const out = [];
  const tools = [];
  let suppressTaskCompleteResult = false;

  for (const item of items) {
    if (suppressTaskCompleteResult) {
      if (isToolTranscriptResultMessage(item) || isEmptyAssistantMessage(item)) {
        if (isToolTranscriptResultMessage(item)) suppressTaskCompleteResult = false;
        continue;
      }
      suppressTaskCompleteResult = false;
    }

    if (isTaskCompleteToolCallMessage(item)) {
      continue;
    }

    if (isTaskCompleteTool(item)) {
      flushToolBuffer(out, tools);
      out.push(makeCompletionSummaryItem(item));
      suppressTaskCompleteResult = true;
      continue;
    }

    if (isCompletedCollapsibleTool(item) || isToolTranscriptMessage(item) || isEmptyAssistantMessage(item)) {
      tools.push(item);
      continue;
    }

    if (isToolItem(item)) flushWrapperNoiseBeforeStructuredTool(out, tools);
    else flushToolBuffer(out, tools);
    out.push(item);
  }

  flushToolBuffer(out, tools, { collapse: !options.deferTrailingToolSummary });
  return out;
}

function findFinalAssistantIndexBeforeTask(items, taskIndex) {
  for (let i = taskIndex - 1; i >= 0; i -= 1) {
    const item = items[i];
    if (isEmptyAssistantMessage(item) || isToolTranscriptMessage(item)) continue;
    if (assistantHasText(item) && !item.streaming) return i;
    return -1;
  }
  return -1;
}

function projectCompletionTurn(items, options = {}) {
  const taskIndex = items.findIndex(isSuccessfulTaskComplete);
  if (taskIndex < 0) return projectGenericTurn(items, options);

  const finalAssistantIndex = findFinalAssistantIndexBeforeTask(items, taskIndex);
  if (finalAssistantIndex < 0) return projectGenericTurn(items, options);

  const out = [];
  const processed = [];

  const flushProcessed = () => {
    if (processed.length > 0) {
      out.push(makeProcessedItem(processed, items[taskIndex]));
      processed.length = 0;
    }
  };

  for (let i = 0; i < finalAssistantIndex; i += 1) {
    const item = items[i];
    if (isProcessedActivityItem(item)) {
      processed.push(item);
    } else {
      flushProcessed();
      out.push(item);
    }
  }

  flushProcessed();
  out.push(items[finalAssistantIndex]);

  const tools = [];
  let suppressTaskCompleteResult = false;
  for (let i = finalAssistantIndex + 1; i < items.length; i += 1) {
    const item = items[i];
    if (suppressTaskCompleteResult) {
      if (isToolTranscriptResultMessage(item) || isEmptyAssistantMessage(item)) {
        if (isToolTranscriptResultMessage(item)) suppressTaskCompleteResult = false;
        continue;
      }
      suppressTaskCompleteResult = false;
    }

    if (isTaskCompleteToolCallMessage(item)) {
      continue;
    }

    if (isTaskCompleteTool(item)) {
      flushToolBuffer(out, tools);
      out.push(makeCompletionSummaryItem(item));
      suppressTaskCompleteResult = true;
      continue;
    }
    if (isCompletedCollapsibleTool(item) || isToolTranscriptMessage(item) || isEmptyAssistantMessage(item)) {
      tools.push(item);
      continue;
    }
    if (isToolItem(item)) flushWrapperNoiseBeforeStructuredTool(out, tools);
    else flushToolBuffer(out, tools);
    out.push(item);
  }
  flushToolBuffer(out, tools, { collapse: !options.deferTrailingToolSummary });

  return out;
}

// ── 子代理调用分组(spawn_subagent / wait_subagent) ──────────────────
//
// 把一轮里连续的子代理工具项合并成一个 subagent_group 项,渲染成
// 「调用了 N 个智能体」的可折叠卡片(展开列各智能体标题,点击打开子会话
// transcript)。分组后该项对下游 projection 是不透明的,不进「已处理」/
// 「调用 N 个工具」的通用折叠,始终独立成行。
//
// 关键:按子会话 id(subagent_session_id)去重 —— fan-out 场景里
// spawn A/B/C + wait A/B/C 共 6 个工具项只对应 3 个子会话。wait_subagent
// 作为管道细节被折叠进来,不单独展示。

const SUBAGENT_TOOL_NAMES = new Set(['spawn_subagent', 'wait_subagent']);

function subagentSessionIdOfItem(item) {
  const tool = item?.tool || {};
  const fromMeta = tool.metadata && typeof tool.metadata === 'object'
    ? tool.metadata.subagent_session_id
    : '';
  const fromArgs = tool.args && typeof tool.args === 'object'
    ? tool.args.session_id
    : '';
  return String(fromMeta || fromArgs || '').trim();
}

// 识别子代理工具项。工具名兜底给实时事件(tool_start/tool_end 带 tool 名);
// metadata.subagent_session_id 兜底给 reload —— 持久化的 tool_result 消息不带
// 工具名字段(重建出 tool.tool=''),但子会话 id 一直落在 metadata 里。
function isSubagentToolItem(item) {
  if (!isToolItem(item)) return false;
  if (SUBAGENT_TOOL_NAMES.has(normalizedToolName(item.tool))) return true;
  return !!subagentSessionIdOfItem(item);
}

function subagentPromptOfItem(item) {
  const tool = item?.tool || {};
  const fromArgs = tool.args && typeof tool.args === 'object' ? tool.args.prompt : '';
  const fromSummary = tool.summary?.object;
  return String(fromArgs || fromSummary || tool.displayOverride || '').trim();
}

function makeSubagentGroupItem(run) {
  const agents = [];
  const seenBySession = new Map();
  run.forEach((item) => {
    const sessionId = subagentSessionIdOfItem(item);
    const prompt = subagentPromptOfItem(item);
    const done = item.tool?.isDone === true;
    if (sessionId && seenBySession.has(sessionId)) {
      const agent = agents[seenBySession.get(sessionId)];
      if (prompt && !agent.prompt) agent.prompt = prompt;
      if (done) agent.done = true;
      return;
    }
    const agent = { sessionId, prompt, itemId: item.id, done };
    if (sessionId) seenBySession.set(sessionId, agents.length);
    agents.push(agent);
  });
  const stamps = run.map(itemTimestamp).filter(Boolean);
  return {
    kind: 'subagent_group',
    id: collapsedId('subagents', run),
    agents,
    coveredItemIds: coveredIds(run),
    ts: stamps.length ? stamps[0] : Date.now(),
  };
}

// 整轮合并:一轮里所有子代理项(不管是否连续)按子会话 id 去重成**一个**分组,
// 落在第一个子代理项的位置,其余从流里移除。这样 fan-out 的 spawn(点火)与
// wait(收结果)即便被中间的 assistant 推理隔开,也归到同一个「调用了 N 个
// 智能体」里 —— 每个智能体的「开始」和「结束/结果」合并成一条,而不是先后
// 各出现一个 N 个智能体的分组。
function groupSubagentTools(items) {
  if (!Array.isArray(items) || items.length === 0) return items;
  const subagentIndices = [];
  items.forEach((item, i) => { if (isSubagentToolItem(item)) subagentIndices.push(i); });
  if (subagentIndices.length === 0) return items;

  const subagentItems = subagentIndices.map((i) => items[i]);
  // 全部还没拿到任一子会话 id(如唯一一次 spawn 仍在运行 / 失败)→ 不分组,
  // 原样渲染,避免出现「调用了 0 个智能体」;等 id 就绪后再分组。
  if (!subagentItems.some((it) => subagentSessionIdOfItem(it))) return items;

  const groupItem = makeSubagentGroupItem(subagentItems);
  const firstIndex = subagentIndices[0];
  const removed = new Set(subagentIndices);
  const out = [];
  items.forEach((item, i) => {
    if (i === firstIndex) out.push(groupItem);
    else if (!removed.has(i)) out.push(item);
  });
  return out;
}

function projectTurn(items, options = {}) {
  if (!Array.isArray(items) || items.length === 0) return [];
  const normalizedItems = groupSubagentTools(normalizeToolInvocationItems(items));
  const finalCollapsed = projectFinalCollapsedTurn(normalizedItems, options);
  if (finalCollapsed) return finalCollapsed;
  return projectCompletionTurn(normalizedItems, options);
}

export function projectCollapsedTranscriptItems(items, options = {}) {
  const source = collapseCompletedCompactNoticeGroups(
    Array.isArray(items) ? items : [],
  );
  if (source.length === 0) return [];

  const out = [];
  let turn = [];

  const flushTurn = (turnOptions = {}) => {
    if (turn.length > 0) {
      out.push(...projectTurn(turn, turnOptions));
      turn = [];
    }
  };

  for (const item of source) {
    if (isUserMessage(item)) {
      flushTurn({ deferTrailingToolSummary: false });
      turn.push(item);
    } else {
      turn.push(item);
    }
  }
  flushTurn({ deferTrailingToolSummary: !!options.deferTrailingToolSummary });

  return out;
}

export const __test__ = {
  isCompletedCollapsibleTool,
  isEmptyAssistantMessage,
  isToolTranscriptMessage,
  isTaskCompleteTool,
  isSuccessfulTaskComplete,
  normalizeToolInvocationItems,
  summarizeToolItems,
  formatDurationMs,
  taskCompleteSummaryText,
  isSubagentToolItem,
  groupSubagentTools,
  collapseCompletedCompactNoticeGroups,
};
