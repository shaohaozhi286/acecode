import { transcriptTimestampMs } from './timestamps.js';

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
  if (created.size > 0) parts.push(`已创建 ${created.size} 个文件`);
  if (edited.size > 0) parts.push(`已编辑 ${edited.size} 个文件`);
  if (read.size > 0) parts.push(`读取 ${read.size} 个文件`);
  if (commands > 0) parts.push(`已运行 ${commands} 条命令`);
  if (nonFileTools > 0) parts.push(`调用 ${nonFileTools} 个工具`);
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

function suppressStructuredToolWrappers(items) {
  const structuredById = new Map();
  items.forEach((item, index) => {
    if (!isToolItem(item)) return;
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

function projectTurn(items, options = {}) {
  if (!Array.isArray(items) || items.length === 0) return [];
  const normalizedItems = normalizeToolInvocationItems(items);
  const finalCollapsed = projectFinalCollapsedTurn(normalizedItems, options);
  if (finalCollapsed) return finalCollapsed;
  return projectCompletionTurn(normalizedItems, options);
}

export function projectCollapsedTranscriptItems(items, options = {}) {
  const source = Array.isArray(items) ? items : [];
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
};
