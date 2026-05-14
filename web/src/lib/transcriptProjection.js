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

function isToolTranscriptMessage(item) {
  if (item?.kind !== 'msg') return false;
  const role = String(item.role || '').toLowerCase();
  return role === 'tool_call' || role === 'tool_result' || role === 'tool';
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
  const value = Number(item?.ts ?? item?.timestamp_ms ?? item?.timestampMs ?? 0);
  return Number.isFinite(value) && value > 0 ? value : 0;
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

function summarizeToolItems(items) {
  const created = new Set();
  const edited = new Set();
  const read = new Set();
  let commands = 0;
  let totalTools = 0;

  items.forEach((item, index) => {
    if (!isToolItem(item) || isTaskCompleteTool(item)) return;
    const tool = item.tool || {};
    const verb = normalizedVerb(tool);
    const name = normalizedToolName(tool);
    const object = summaryObject(tool, String(item.id || index));
    totalTools += 1;

    if (verb === 'created') {
      countObject(created, 'created', index, object);
    } else if (verb === 'wrote' || verb === 'edited' || verb === 'edit') {
      countObject(edited, 'edited', index, object);
    } else if (verb === 'read') {
      countObject(read, 'read', index, object);
    } else if (verb === 'ran' || name === 'bash') {
      commands += 1;
    } else if (name === 'file_write') {
      const additions = metricValue(tool.summary?.metrics, '+');
      if (additions > 0 && metricValue(tool.summary?.metrics, '-') === 0) {
        countObject(created, 'created', index, object);
      } else {
        countObject(edited, 'edited', index, object);
      }
    } else if (name === 'file_edit') {
      countObject(edited, 'edited', index, object);
    } else if (name === 'file_read') {
      countObject(read, 'read', index, object);
    }
  });

  if (totalTools === 0) {
    totalTools = countTranscriptOnlyTools(items);
  }

  const parts = [];
  if (created.size > 0) parts.push(`已创建 ${created.size} 个文件`);
  if (edited.size > 0) parts.push(`已编辑 ${edited.size} 个文件`);
  if (read.size > 0) parts.push(`读取 ${read.size} 个文件`);
  if (commands > 0) parts.push(`已运行 ${commands} 条命令`);
  if (totalTools > 0) parts.push(`调用 ${totalTools} 个工具`);
  return parts.length > 0 ? parts.join('，') : '已调用工具';
}

function hasCollapsibleToolActivity(items) {
  return items.some(isCompletedCollapsibleTool) || items.some(isToolTranscriptMessage);
}

function coveredIds(items) {
  return items
    .map((item) => item?.id)
    .filter((id) => id !== undefined && id !== null);
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

function makeProcessedItem(items, endItem) {
  const { startTs, endTs } = collapsedTimestamps(items, endItem);
  const duration = startTs && endTs ? formatDurationMs(endTs - startTs) : '';
  return {
    kind: 'activity_summary',
    mode: 'processed',
    id: collapsedId('processed', items),
    title: duration ? `已处理 ${duration}` : '已处理',
    collapsedItems: items.slice(),
    coveredItemIds: coveredIds(items),
    ts: startTs || endTs || Date.now(),
  };
}

function taskCompleteSummaryText(item) {
  const tool = item?.tool || {};
  const summary = tool.summary || {};
  const metricSummary = metricText(summary.metrics, 'summary');
  if (metricSummary) return metricSummary;

  const object = cleanSummaryText(summary.object);
  if (object && object.toLowerCase() !== 'task') return object;

  const output = cleanSummaryText(tool.output);
  if (output) return output;

  const title = cleanSummaryText(tool.title || tool.displayOverride);
  if (title && title.toLowerCase() !== 'task_complete') return title;

  return '已完成';
}

function makeCompletionSummaryItem(item) {
  const summary = taskCompleteSummaryText(item);
  const id = item?.id ?? (itemTimestamp(item) || 'unknown');
  return {
    kind: 'completion_summary',
    id: `completion:${id}`,
    title: `总结：${summary}`,
    summary,
    sourceItem: item,
    coveredItemIds: coveredIds([item]),
    ts: itemTimestamp(item) || Date.now(),
  };
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

function finalProcessedItemsBefore(items, beforeIndex) {
  const processed = [];
  for (let i = 0; i < beforeIndex; i += 1) {
    const item = items[i];
    if (isUserMessage(item)) continue;
    if (isEmptyAssistantMessage(item)) continue;
    if (isTaskCompleteTool(item) || isTaskCompleteToolCallMessage(item)) continue;
    processed.push(item);
  }
  return processed;
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
    if (assistantHasText(previous) && !isStreamingAssistant(previous)) {
      pushProcessedSummary(out, finalProcessedItemsBefore(items, previousIndex), last);
      out.push(previous);
    } else {
      pushProcessedSummary(out, finalProcessedItemsBefore(items, lastIndex), last);
    }
    out.push(makeCompletionSummaryItem(last));
    return out;
  }

  if (!options.deferTrailingToolSummary && assistantHasText(last) && !isStreamingAssistant(last)) {
    const out = preservedTurnPrefix(items);
    pushProcessedSummary(out, finalProcessedItemsBefore(items, lastIndex), last);
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
  const finalCollapsed = projectFinalCollapsedTurn(items, options);
  if (finalCollapsed) return finalCollapsed;
  return projectCompletionTurn(items, options);
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
  summarizeToolItems,
  formatDurationMs,
  taskCompleteSummaryText,
};
