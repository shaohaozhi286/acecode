import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createApi } from './api.js';
import { connection } from './connection.js';
import { attachmentsFromContentParts, normalizeAttachmentList } from './messageAttachments.js';
import { sessionDisplayTitle, titleFromMessages } from './sessionTitle.js';
import { transcriptTimestampMs } from './timestamps.js';

export function messageKey(role, content) {
  return `${role || ''}\u0000${content || ''}`;
}

function normalizeSessionRef(sessionRef) {
  if (!sessionRef) return null;
  if (typeof sessionRef === 'string') return { sessionId: sessionRef };
  const sessionId = sessionRef.sessionId || sessionRef.id || '';
  if (!sessionId) return null;
  return {
    ...sessionRef,
    sessionId,
    workspaceHash: sessionRef.workspaceHash || sessionRef.workspace_hash || '',
  };
}

function cloneToolMap(toolMap) {
  if (toolMap instanceof Map) return new Map(toolMap);
  if (toolMap && typeof toolMap === 'object') return new Map(Object.entries(toolMap));
  return new Map();
}

function cloneTurnTimings(turnTimings) {
  const out = new Map();
  const entries = turnTimings instanceof Map
    ? turnTimings.entries()
    : Object.entries(turnTimings && typeof turnTimings === 'object' ? turnTimings : {});
  for (const [key, value] of entries) {
    if (!key || !value || typeof value !== 'object') continue;
    out.set(String(key), { ...value });
  }
  return out;
}

function cloneTokenUsage(tokenUsage) {
  if (!tokenUsage || typeof tokenUsage !== 'object') return null;
  return { ...tokenUsage };
}

function cloneGoal(goal) {
  if (!goal || typeof goal !== 'object') return null;
  return { ...goal };
}

function cloneJsonLike(value) {
  if (value == null) return null;
  if (Array.isArray(value)) return value.map((item) => cloneJsonLike(item));
  if (typeof value === 'object') {
    const out = {};
    for (const [key, item] of Object.entries(value)) out[key] = cloneJsonLike(item);
    return out;
  }
  return value;
}

function normalizeTodoStatus(status) {
  const value = String(status || '').trim().toLowerCase();
  if (value === 'pending' || value === 'in_progress' || value === 'completed' || value === 'cancelled') return value;
  return 'pending';
}

function normalizeTodos(todos) {
  if (!Array.isArray(todos)) return [];
  return todos
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => ({
      id: String(item.id ?? '').trim() || '?',
      content: String(item.content ?? '').trim() || '(no description)',
      status: normalizeTodoStatus(item.status),
    }));
}

function cloneTodos(todos) {
  return normalizeTodos(todos);
}

function normalizeTodoSummary(summary, todos = []) {
  const fallback = { total: todos.length, pending: 0, in_progress: 0, completed: 0, cancelled: 0 };
  for (const item of todos) {
    fallback[item.status] = (fallback[item.status] || 0) + 1;
  }
  if (!summary || typeof summary !== 'object') return fallback;
  return {
    total: Math.max(0, Number(summary.total) || fallback.total),
    pending: Math.max(0, Number(summary.pending) || 0),
    in_progress: Math.max(0, Number(summary.in_progress ?? summary.inProgress) || 0),
    completed: Math.max(0, Number(summary.completed) || 0),
    cancelled: Math.max(0, Number(summary.cancelled) || 0),
  };
}

function cloneState(state) {
  return {
    ...state,
    items: Array.isArray(state.items) ? state.items : [],
    toolMap: cloneToolMap(state.toolMap),
    turnTimings: cloneTurnTimings(state.turnTimings),
    tokenUsage: cloneTokenUsage(state.tokenUsage),
    goal: cloneGoal(state.goal),
    todos: cloneTodos(state.todos),
    todoSummary: state.todoSummary && typeof state.todoSummary === 'object' ? { ...state.todoSummary } : null,
    activity: state.activity && typeof state.activity === 'object' ? { ...state.activity } : null,
  };
}

function readUsageInt(payload, snakeKey, camelKey) {
  const raw = payload?.[snakeKey] ?? payload?.[camelKey];
  const value = Number(raw);
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.trunc(value));
}

function normalizeUsagePayload(payload, timestampMs) {
  const hasDataRaw = payload?.has_data ?? payload?.hasData;
  return {
    promptTokens: readUsageInt(payload, 'prompt_tokens', 'promptTokens'),
    completionTokens: readUsageInt(payload, 'completion_tokens', 'completionTokens'),
    totalTokens: readUsageInt(payload, 'total_tokens', 'totalTokens'),
    hasData: hasDataRaw === true,
    timestampMs: Number(timestampMs) || Date.now(),
  };
}

function allocateItemId(state) {
  const id = state.nextItemId || 1;
  state.nextItemId = id + 1;
  return id;
}

function eventTs(msg) {
  return transcriptTimestampMs(msg) || Date.now();
}

function clientMessageIdFromMetadata(metadata) {
  if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) return '';
  return typeof metadata.client_message_id === 'string'
    ? metadata.client_message_id.trim()
    : '';
}

function eventSeq(msg) {
  return typeof msg?.seq === 'number' && Number.isFinite(msg.seq) ? msg.seq : null;
}

function isStaleSequencedEvent(state, msg) {
  const seq = eventSeq(msg);
  return seq !== null && seq <= (state?.lastSeq || 0);
}

function markEventSeqApplied(state, msg) {
  const seq = eventSeq(msg);
  if (seq !== null && seq > (state.lastSeq || 0)) {
    state.lastSeq = seq;
  }
}

function terminationNoticeText(payload = {}) {
  const source = payload.source || '';
  const reason = String(payload.reason || payload.message || '').trim();
  if (source === 'user') return reason || '用户已终止本轮任务';
  return reason ? `任务已终止：${reason}` : '任务已终止';
}

function isAbortLikeReason(reason) {
  return /abort|cancel|interrupt|terminat|用户.*终止|已终止|取消|中断/i.test(String(reason || ''));
}

function appendTerminationNotice(next, msg, payload = {}) {
  const text = terminationNoticeText(payload);
  const last = next.items[next.items.length - 1];
  if (last?.kind === 'termination_notice') {
    if (last.content === text) return;
    if (last.source === 'user' && payload.source !== 'user' && isAbortLikeReason(payload.reason || payload.message)) {
      return;
    }
  }
  next.items = [
    ...next.items,
    {
      kind: 'termination_notice',
      id: allocateItemId(next),
      source: payload.source || 'server',
      content: text,
      ts: eventTs(msg),
    },
  ];
}

function normalizeSummaryMetrics(metrics) {
  if (!Array.isArray(metrics)) return [];
  return metrics
    .map((metric) => {
      if (Array.isArray(metric) && metric.length >= 2) {
        return { label: String(metric[0] ?? ''), value: String(metric[1] ?? '') };
      }
      if (metric && typeof metric === 'object') {
        return {
          label: String(metric.label ?? ''),
          value: String(metric.value ?? ''),
        };
      }
      return null;
    })
    .filter((metric) => metric && metric.label);
}

function normalizePersistedToolSummary(metadata) {
  const raw = metadata?.tool_summary;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  return {
    verb: typeof raw.verb === 'string' ? raw.verb : '',
    object: typeof raw.object === 'string' ? raw.object : '',
    icon: typeof raw.icon === 'string' ? raw.icon : '',
    metrics: normalizeSummaryMetrics(raw.metrics),
  };
}

function normalizeAskUserQuestionResult(metadataOrResult) {
  const raw = metadataOrResult?.ask_user_question_result || metadataOrResult?.askUserQuestionResult || metadataOrResult;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const items = Array.isArray(raw.items) ? raw.items : [];
  const normalized = items
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => ({
      question: String(item.question ?? item.q ?? ''),
      answer: String(item.answer ?? item.a ?? ''),
    }))
    .filter((item) => item.question || item.answer);
  return normalized.length > 0 ? { items: normalized } : null;
}

function readRuntimeTurnCount(data) {
  const raw = data?.turn_count ?? data?.turnCount;
  const value = Number(raw);
  if (!Number.isFinite(value)) return null;
  return Math.max(0, Math.trunc(value));
}

function normalizeTurnTimingRecord(message) {
  const metadata = message?.metadata;
  const raw = metadata && typeof metadata === 'object' && !Array.isArray(metadata)
    ? metadata.turn_timing
    : null;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const userMessageUuid = String(raw.user_message_uuid || raw.userMessageUuid || '').trim();
  if (!userMessageUuid) return null;
  const startedAtMs = Number(raw.started_at_ms ?? raw.startedAtMs);
  const completedAtMs = Number(raw.completed_at_ms ?? raw.completedAtMs);
  const durationMs = Number(raw.duration_ms ?? raw.durationMs);
  const status = String(raw.status || '').trim();
  return {
    userMessageUuid,
    startedAtMs: Number.isFinite(startedAtMs) ? Math.max(0, Math.trunc(startedAtMs)) : 0,
    completedAtMs: Number.isFinite(completedAtMs) ? Math.max(0, Math.trunc(completedAtMs)) : 0,
    durationMs: Number.isFinite(durationMs) ? Math.max(0, Math.trunc(durationMs)) : 0,
    status,
  };
}

function normalizePersistedToolHunks(metadata) {
  const raw = metadata?.tool_hunks;
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((hunk) => hunk && typeof hunk === 'object' && !Array.isArray(hunk))
    .map((hunk) => ({ ...hunk }));
}

function stringFromPersistedValue(value) {
  if (value == null) return '';
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint') {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function persistedToolCallId(raw) {
  const id = raw?.id ?? raw?.tool_call_id ?? raw?.toolCallId ?? raw?.call_id ?? '';
  return String(id || '').trim();
}

function persistedToolCallIndex(raw, fallbackIndex) {
  const value = raw?.tool_index ?? raw?.toolIndex ?? raw?.index;
  const number = Number(value);
  if (Number.isFinite(number)) return Math.trunc(number);
  return fallbackIndex;
}

function normalizePersistedToolCall(raw, fallbackIndex) {
  const call = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
  const fn = call.function && typeof call.function === 'object' && !Array.isArray(call.function)
    ? call.function
    : null;
  const rawName = fn?.name ?? call.name ?? call.tool ?? call.tool_name ?? '';
  const rawArgs = fn && Object.prototype.hasOwnProperty.call(fn, 'arguments')
    ? fn.arguments
    : call.arguments ?? call.args ?? call.input ?? '';
  return {
    name: String(rawName || '').trim(),
    argumentsText: stringFromPersistedValue(rawArgs),
    toolCallId: persistedToolCallId(call),
    toolIndex: persistedToolCallIndex(call, fallbackIndex),
  };
}

function persistedToolCallMessageId(message, messageIndex, call) {
  const parentId = String(message?.id || `message-${messageIndex}`);
  const suffix = call.toolCallId || `index-${call.toolIndex}`;
  return `${parentId}:tool_call:${suffix}`;
}

function persistedToolCallContent(call) {
  return `[Tool: ${call.name}] ${call.argumentsText}`;
}

function messageToolCallFields(m) {
  const toolCallId = String((m?.tool_call_id ?? m?.toolCallId ?? '') || '').trim();
  const hasToolIndex = m?.tool_index != null || m?.toolIndex != null;
  const toolIndex = hasToolIndex ? (m.tool_index ?? m.toolIndex) : null;
  return { toolCallId, hasToolIndex, toolIndex };
}

function messageOrdinalValue(value, fallback = null) {
  if (value !== undefined && value !== null && value !== '') {
    const n = Number(value);
    if (Number.isInteger(n) && n >= 0) return n;
  }
  if (fallback !== undefined && fallback !== null && fallback !== '') {
    const f = Number(fallback);
    if (Number.isInteger(f) && f >= 0) return f;
  }
  return null;
}

function messageOrdinal(m, fallback = null) {
  return messageOrdinalValue(m?.__messageOrdinal ?? m?.message_ordinal ?? m?.messageOrdinal, fallback);
}

function genericHistoryMessageItem(next, m, extra = {}) {
  const fields = messageToolCallFields(m);
  const item = {
    kind: 'msg',
    id: allocateItemId(next),
    messageId: extra.messageId || m?.id || '',
    role: extra.role || m?.role || 'system',
    content: extra.content ?? m?.content ?? '',
    contentParts: extra.contentParts || (Array.isArray(m?.content_parts) ? m.content_parts : []),
    metadata: extra.metadata ?? m?.metadata,
    ts: extra.ts || transcriptTimestampMs(m) || Date.now(),
  };
  const ordinal = messageOrdinalValue(extra.messageOrdinal, messageOrdinal(m));
  if (ordinal !== null) item.messageOrdinal = ordinal;
  const toolCallId = extra.toolCallId ?? fields.toolCallId;
  const hasToolCallId = Object.prototype.hasOwnProperty.call(extra, 'toolCallId') || !!fields.toolCallId;
  const hasToolIndex = Object.prototype.hasOwnProperty.call(extra, 'toolIndex') || fields.hasToolIndex;
  const toolIndex = Object.prototype.hasOwnProperty.call(extra, 'toolIndex') ? extra.toolIndex : fields.toolIndex;
  if (hasToolCallId) {
    item.tool_call_id = toolCallId || '';
    item.toolCallId = toolCallId || '';
  }
  if (hasToolIndex) {
    item.tool_index = toolIndex;
    item.toolIndex = toolIndex;
  }
  return item;
}

function historyItemFromMessage(next, m, messageOrdinal = null) {
  const metadata = m?.metadata && typeof m.metadata === 'object' ? m.metadata : null;
  const ts = transcriptTimestampMs(m) || Date.now();
  if ((m?.role || '') === 'tool' && metadata) {
    const summary = normalizePersistedToolSummary(metadata);
    const hunks = normalizePersistedToolHunks(metadata);
    const askUserQuestionResult = normalizeAskUserQuestionResult(metadata);
    const attachments = attachmentsFromContentParts(m.content_parts);
    if (summary || hunks.length > 0 || attachments.length > 0 || askUserQuestionResult) {
      return {
        kind: 'tool',
        id: allocateItemId(next),
        messageId: m.id || '',
        tool: {
          isTaskComplete: false,
          isDone: true,
          success: true,
          tool: m.tool || '',
          toolCallId: m.tool_call_id || m.toolCallId || '',
          toolIndex: m.tool_index ?? m.toolIndex ?? null,
          args: null,
          startedAtMs: ts,
          displayOverride: '',
          title: summary?.object || m.content || '工具调用',
          tailLines: [],
          currentPartial: '',
          totalLines: 0,
          totalBytes: 0,
          elapsed: 0,
          summary,
          output: m.content || '',
          hunks,
          attachments,
          metadata,
          askUserQuestionResult,
        },
        ts,
      };
    }
  } else if ((m?.role || '') === 'tool') {
    const attachments = attachmentsFromContentParts(m.content_parts);
    if (attachments.length > 0) {
      return {
        kind: 'tool',
        id: allocateItemId(next),
        messageId: m.id || '',
        tool: {
          isTaskComplete: false,
          isDone: true,
          success: true,
          tool: m.tool || '',
          toolCallId: m.tool_call_id || m.toolCallId || '',
          toolIndex: m.tool_index ?? m.toolIndex ?? null,
          args: null,
          startedAtMs: ts,
          displayOverride: '',
          title: m.content || attachments[0]?.name || '工具调用',
          tailLines: [],
          currentPartial: '',
          totalLines: 0,
          totalBytes: 0,
          elapsed: 0,
          summary: null,
          output: m.content || '',
          hunks: [],
          attachments,
        },
        ts,
      };
    }
  }

  return genericHistoryMessageItem(next, m, { ts, messageOrdinal });
}

function historyItemsFromMessage(next, m, messageIndex) {
  const role = m?.role || '';
  const rawOrdinal = messageOrdinal(m, messageIndex);
  if (role !== 'assistant') return [historyItemFromMessage(next, m, rawOrdinal)];

  const toolCalls = Array.isArray(m?.tool_calls) ? m.tool_calls : [];
  if (toolCalls.length === 0) return [historyItemFromMessage(next, m, rawOrdinal)];

  const items = [];
  const content = m?.content ?? '';
  const contentParts = Array.isArray(m?.content_parts) ? m.content_parts : [];
  if (String(content || '').trim() || contentParts.length > 0) {
    items.push(historyItemFromMessage(next, m, rawOrdinal));
  }

  for (let i = 0; i < toolCalls.length; i += 1) {
    const call = normalizePersistedToolCall(toolCalls[i], i);
    items.push(genericHistoryMessageItem(next, m, {
      messageId: persistedToolCallMessageId(m, messageIndex, call),
      role: 'tool_call',
      content: persistedToolCallContent(call),
      contentParts: [],
      metadata: {
        ...(m?.metadata && typeof m.metadata === 'object' && !Array.isArray(m.metadata) ? m.metadata : {}),
        tool_call_id: call.toolCallId,
        tool_index: call.toolIndex,
      },
      ts: transcriptTimestampMs(m) || Date.now(),
      toolCallId: call.toolCallId,
      toolIndex: call.toolIndex,
      messageOrdinal: rawOrdinal,
    }));
  }

  return items;
}

function historyItemsFromMessages(next, messages) {
  const items = [];
  for (let i = 0; i < messages.length; i += 1) {
    items.push(...historyItemsFromMessage(next, messages[i], i));
  }
  return items;
}

function visibleTranscriptMessages(messages) {
  if (!Array.isArray(messages)) return [];
  return messages
    .map((m, index) => (m && typeof m === 'object' ? { ...m, __messageOrdinal: index } : m))
    .filter((m) => !m?.is_meta && !m?.metadata?.hidden_goal_context);
}

function splitTranscriptMessages(messages) {
  const visible = visibleTranscriptMessages(messages);
  const transcriptMessages = [];
  const turnTimings = new Map();
  for (const message of visible) {
    const timing = normalizeTurnTimingRecord(message);
    if (timing) {
      turnTimings.set(timing.userMessageUuid, timing);
      continue;
    }
    transcriptMessages.push(message);
  }
  return { messages: transcriptMessages, turnTimings };
}

function itemUserMessageUuid(item) {
  return String(item?.messageId || item?.id || item?.metadata?.user_message_uuid || '').trim();
}

function applyTurnTimingsToItems(items, turnTimings) {
  if (!(turnTimings instanceof Map) || turnTimings.size === 0) return items;
  let activeTiming = null;
  return items.map((item) => {
    if (item?.kind === 'msg' && item.role === 'user') {
      activeTiming = turnTimings.get(itemUserMessageUuid(item)) || null;
      return item;
    }
    if (!activeTiming) return item;
    if (item?.kind !== 'msg' && item?.kind !== 'tool') return item;
    return {
      ...item,
      turnTiming: activeTiming,
      turnDurationMs: activeTiming.durationMs,
    };
  });
}

function toolKey(payload = {}) {
  if (payload.tool_call_id || payload.call_id || payload.id) {
    return payload.tool_call_id || payload.call_id || payload.id;
  }
  if (payload.tool_index !== undefined && payload.tool_index !== null) {
    return `${payload.tool || '_tool'}#${payload.tool_index}`;
  }
  return payload.tool || '_anon';
}

function finalizeStreaming(next) {
  if (next.streamingId == null) return next;
  const currentStreamingId = next.streamingId;
  next.streamingId = null;
  next.items = next.items.map((item) => item.id === currentStreamingId
    ? { ...item, streaming: false }
    : item);
  return next;
}

function trailingAssistantDraftIndex(items) {
  if (!Array.isArray(items)) return -1;
  for (let index = items.length - 1; index >= 0; index -= 1) {
    const item = items[index];
    if (!item) continue;
    if (item.kind === 'msg' && item.role === 'assistant') {
      return item.streamDraft && !item.messageId ? index : -1;
    }
    if (item.kind === 'msg' || item.kind === 'tool' || item.kind === 'termination_notice') {
      return -1;
    }
  }
  return -1;
}

function replaceAssistantItemWithFinal(item, payload, msg) {
  return {
    ...item,
    role: 'assistant',
    content: payload.content || item.content || '',
    contentParts: Array.isArray(payload.content_parts) ? payload.content_parts : item.contentParts,
    messageId: payload.id || item.messageId || '',
    metadata: payload.metadata ?? item.metadata,
    ts: eventTs(msg),
    streaming: false,
    streamDraft: false,
  };
}

export function replaySinceForLiveCatchup({ isLive = false, loadedSeq = 0 } = {}) {
  if (!isLive) return null;
  return loadedSeq > 0 ? loadedSeq : 1;
}

// 末尾 assistant 消息的文本(从尾部往前找第一条 assistant msg)。用于
// "内容回退" 探测与防回退保护:实时流式累积到完整内容后,任何把它替换成
// 更短文本的状态变更都是可疑的截断。
export function lastAssistantText(state) {
  const items = Array.isArray(state?.items) ? state.items : [];
  for (let i = items.length - 1; i >= 0; i -= 1) {
    const it = items[i];
    if (it?.kind === 'msg' && it.role === 'assistant') return String(it.content || '');
  }
  return '';
}

// 内容回退探测器(防回归诊断):某次状态变更把末尾 assistant 文本变短时报警,
// 标明来源路径、长度、前后预览,方便定位 "消息显示不全" 究竟由哪条路径造成。
// transcript_replace(重试/compact 的有意重置)会合法地变短,调用方应跳过。
// 低噪声:只在真的变短时打。
export function detectAssistantTailRegression(source, prevState, nextState) {
  const prev = lastAssistantText(prevState);
  const next = lastAssistantText(nextState);
  if (prev.length > next.length) {
    const tail = (s) => (s.length > 24 ? `${s.slice(0, 12)}…${s.slice(-12)}` : s);
    // eslint-disable-next-line no-console
    console.warn(
      `[ace-transcript] assistant tail shrank via ${source}: ${prev.length}→${next.length} chars`
      + ` lastSeq ${Number(prevState?.lastSeq) || 0}→${Number(nextState?.lastSeq) || 0}`
      + ` | was "${tail(prev)}" now "${tail(next)}"`,
    );
    return true;
  }
  return false;
}

// 防回退保护:REST 历史快照(loadedState)可能比实时 WS 已累积的当前回合更旧
// —— 快照里 messages 尚未含进行中的 assistant、events 只回放了部分 token。
// 当实时态(liveState)seq 不落后于快照、且末尾 assistant 文本更完整时,保留
// 实时文本,不让更旧的快照把界面截断。只增不减:返回值的末尾 assistant 文本
// 长度永远 >= 二者中较长的那个,因此即便误判也不会比直接用快照更差。
export function preserveLiveAssistantTailOnLoad(loadedState, liveState) {
  if (!loadedState) return loadedState;
  const liveItems = Array.isArray(liveState?.items) ? liveState.items : [];
  if (liveItems.length === 0) return loadedState;
  const liveSeq = Number(liveState?.lastSeq) || 0;
  const loadedSeq = Number(loadedState?.lastSeq) || 0;
  if (liveSeq <= loadedSeq) return loadedState; // 快照不比实时旧 → 用快照
  const liveTail = lastAssistantText(liveState);
  const loadedTail = lastAssistantText(loadedState);
  if (!liveTail || liveTail.length <= loadedTail.length) return loadedState;

  const items = loadedState.items.slice();
  let idx = -1;
  for (let i = items.length - 1; i >= 0; i -= 1) {
    if (items[i]?.kind === 'msg' && items[i].role === 'assistant') { idx = i; break; }
    if (items[i]?.kind === 'msg' || items[i]?.kind === 'tool') break;
  }
  if (idx >= 0) {
    items[idx] = { ...items[idx], content: liveTail };
  } else {
    const liveDraft = liveItems[liveItems.length - 1];
    const nextId = Number(loadedState.nextItemId) || 1;
    items.push({
      kind: 'msg',
      id: nextId,
      role: 'assistant',
      content: liveTail,
      contentParts: Array.isArray(liveDraft?.contentParts) ? liveDraft.contentParts : [],
      ts: liveDraft?.ts || Date.now(),
      streaming: liveState?.streamingId != null,
      streamDraft: true,
    });
    return {
      ...loadedState,
      items,
      nextItemId: nextId + 1,
      lastSeq: Math.max(loadedSeq, liveSeq),
    };
  }
  return {
    ...loadedState,
    items,
    lastSeq: Math.max(loadedSeq, liveSeq),
  };
}

export function applyTranscriptReplayEvents(state, events = []) {
  let next = cloneState(state || createTranscriptState());
  const effects = [];
  const seenMessageIds = new Set(next.items
    .filter((item) => item?.kind === 'msg' && item.messageId)
    .map((item) => item.messageId));
  const seenMessages = new Set(next.items
    .filter((item) => item?.kind === 'msg')
    .map((item) => messageKey(item.role || 'system', item.content || '')));
  let pendingStreamEvents = [];
  const flushPendingStreamEvents = () => {
    for (const ev of pendingStreamEvents) {
      const reduced = reduceTranscriptEvent(next, ev);
      next = reduced.state;
      effects.push(...reduced.effects);
    }
    pendingStreamEvents = [];
  };

  for (const ev of (Array.isArray(events) ? events : [])) {
    if (isStaleSequencedEvent(next, ev)) continue;
    if (ev?.type === 'token' || ev?.type === 'reasoning') {
      pendingStreamEvents.push(ev);
      continue;
    }
    if (ev?.type === 'message') {
      const p = ev.payload || {};
      const incomingMessageId = p.id || '';
      if (incomingMessageId && seenMessageIds.has(incomingMessageId)) {
        pendingStreamEvents = [];
        const reduced = reduceTranscriptEvent(next, ev);
        next = reduced.state;
        effects.push(...reduced.effects);
        continue;
      }
      const key = messageKey(p.role || 'system', p.content || '');
      if (seenMessages.has(key)) {
        pendingStreamEvents = [];
        markEventSeqApplied(next, ev);
        continue;
      }
      if (incomingMessageId) seenMessageIds.add(incomingMessageId);
      seenMessages.add(key);
    }
    flushPendingStreamEvents();
    const reduced = reduceTranscriptEvent(next, ev);
    next = reduced.state;
    effects.push(...reduced.effects);
  }
  flushPendingStreamEvents();

  return { state: next, effects };
}

export function createTranscriptState(overrides = {}) {
  return {
    items: [],
    busy: false,
    turns: 0,
    title: '',
    status: 'idle',
    lastSeq: 0,
    isLive: false,
    loadState: 'idle',
    streamingId: null,
    toolMap: new Map(),
    turnTimings: new Map(),
    nextItemId: 1,
    error: '',
    tokenUsage: null,
    goal: null,
    todos: [],
    todoSummary: null,
    activity: null,
    // turnHadAssistantText / lastAssistantText 用于桌面通知:在 busy=true→false
    // 转换且本回合产生过 assistant 文本时,emit turn_completed effect。reducer 之外
    // 的代码不应直接读 / 写它们。见
    // openspec/changes/add-windows-wintoast-completion-notifications。
    turnHadAssistantText: false,
    lastAssistantText: '',
    ...overrides,
    toolMap: cloneToolMap(overrides.toolMap),
    turnTimings: cloneTurnTimings(overrides.turnTimings),
    tokenUsage: cloneTokenUsage(overrides.tokenUsage),
    goal: cloneGoal(overrides.goal),
    todos: cloneTodos(overrides.todos),
    todoSummary: overrides.todoSummary && typeof overrides.todoSummary === 'object' ? { ...overrides.todoSummary } : null,
  };
}

export function resetTranscriptForSession(state, { title = '', isLive = false } = {}) {
  return createTranscriptState({
    title,
    isLive,
    loadState: state?.loadState || 'idle',
  });
}

export function reduceTranscriptEvent(state, msg) {
  const current = state || createTranscriptState();
  if (isStaleSequencedEvent(current, msg)) {
    return { state: current, effects: [] };
  }

  const next = cloneState(current);
  const effects = [];
  const t = msg?.type || '';
  const p = msg?.payload || {};

  markEventSeqApplied(next, msg);

  switch (t) {
    case 'transcript_replace': {
      finalizeStreaming(next);
      next.toolMap = new Map();
      const { messages, turnTimings } = splitTranscriptMessages(p.messages);
      next.turnTimings = turnTimings;
      next.items = applyTurnTimingsToItems(historyItemsFromMessages(next, messages), turnTimings);
      const restoredTitle = titleFromMessages(messages);
      if (restoredTitle) next.title = restoredTitle;
      next.tokenUsage = null;
      next.error = '';
      break;
    }
    case 'agent_progress': {
      const phase = p.phase || '';
      const label = p.label || '';
      if (!phase && !label) break;
      next.activity = {
        phase,
        label: label || phase,
        detail: p.detail || '',
        tool: p.tool || '',
        toolCallId: p.tool_call_id || p.call_id || p.id || '',
        toolIndex: p.tool_index ?? null,
        startedAtMs: Number(p.started_at_ms) || eventTs(msg),
        timestampMs: eventTs(msg),
      };
      break;
    }
    case 'queued_input_accepted': {
      const clientMessageId = typeof p.client_message_id === 'string'
        ? p.client_message_id.trim()
        : '';
      if (!clientMessageId) break;
      const alreadyVisible = next.items.some((item) => (
        item.kind === 'msg' &&
        item.role === 'user' &&
        clientMessageIdFromMetadata(item.metadata) === clientMessageId
      ));
      if (alreadyVisible) break;
      finalizeStreaming(next);
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: '',
          role: 'user',
          content: String(p.content || ''),
          contentParts: [],
          metadata: {
            client_message_id: clientMessageId,
            optimistic_queued_input: true,
          },
          ts: eventTs(msg),
        },
      ];
      break;
    }
    case 'message': {
      const role = p.role || 'system';
      if (role === 'error') {
        // Legacy/current daemon provider failures are visible message events.
        // Clear any earlier assistant text so their terminal busy=false cannot
        // be mistaken for a completed turn.
        next.turnHadAssistantText = false;
        next.lastAssistantText = '';
      }
      const timing = normalizeTurnTimingRecord(p);
      if (timing) {
        next.turnTimings.set(timing.userMessageUuid, timing);
        next.items = applyTurnTimingsToItems(next.items, next.turnTimings);
        break;
      }
      if (role === 'assistant' && next.streamingId != null) {
        const currentStreamingId = next.streamingId;
        next.streamingId = null;
        const finalContent = p.content || '';
        if (finalContent && finalContent.trim()) {
          next.turnHadAssistantText = true;
          next.lastAssistantText = finalContent;
        }
        next.items = next.items.map((item) => item.id === currentStreamingId
          ? replaceAssistantItemWithFinal(item, p, msg)
          : item);
        break;
      }
      if (role === 'assistant' && (p.content || '').trim()) {
        const draftIndex = trailingAssistantDraftIndex(next.items);
        if (draftIndex >= 0) {
          next.items = next.items.map((item, index) => (index === draftIndex
            ? replaceAssistantItemWithFinal(item, p, msg)
            : item));
          break;
        }
      }
      finalizeStreaming(next);
      const incomingContent = p.content || '';
      if (role === 'assistant' && incomingContent && incomingContent.trim()) {
        next.turnHadAssistantText = true;
        next.lastAssistantText = incomingContent;
      }
      // 按持久消息 id 幂等(dedupe-message-events-by-id):home auto_start 流程中
      // GET /messages 快照与 WS 回放存在竞争窗口 —— daemon 先读事件回放、后读
      // 消息快照,而回合线程先 append 消息(中间隔落盘 I/O)、后 emit 事件,
      // 交错时快照已含 user 消息但其 message 事件 seq 高于水位、随后才经 WS
      // 送达。seq 水位只保证通道内幂等,跨通道要靠消息 id:命中已有条目时
      // 原位更新(事件可能带更完整的 content_parts / metadata),不追加,
      // 否则用户气泡出现两次。不同 id 相同文本(用户故意连发)不受影响。
      const incomingMessageId = p.id || '';
      const existingIndex = incomingMessageId
        ? next.items.findIndex((item) => item.kind === 'msg' && item.messageId === incomingMessageId)
        : -1;
      if (existingIndex >= 0) {
        next.items = next.items.map((item, index) => (index === existingIndex
          ? {
              ...item,
              role,
              content: incomingContent || item.content || '',
              contentParts: Array.isArray(p.content_parts) ? p.content_parts : item.contentParts,
              metadata: p.metadata ?? item.metadata,
              ts: eventTs(msg),
            }
          : item));
        break;
      }
      const incomingClientMessageId = role === 'user'
        ? clientMessageIdFromMetadata(p.metadata)
        : '';
      const optimisticIndex = incomingClientMessageId
        ? next.items.findIndex((item) => (
            item.kind === 'msg' &&
            item.role === 'user' &&
            !item.messageId &&
            item.metadata?.optimistic_queued_input === true &&
            clientMessageIdFromMetadata(item.metadata) === incomingClientMessageId
          ))
        : -1;
      if (optimisticIndex >= 0) {
        next.items = next.items.map((item, index) => (index === optimisticIndex
          ? {
              ...item,
              messageId: incomingMessageId,
              role,
              content: incomingContent,
              contentParts: Array.isArray(p.content_parts) ? p.content_parts : [],
              metadata: p.metadata,
              ts: eventTs(msg),
            }
          : item));
        break;
      }
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: incomingMessageId,
          role,
          content: incomingContent,
          contentParts: Array.isArray(p.content_parts) ? p.content_parts : [],
          metadata: p.metadata,
          ts: eventTs(msg),
        },
      ];
      break;
    }
    case 'token': {
      const text = p.text || '';
      if (text && text.trim()) {
        next.turnHadAssistantText = true;
      }
      if (next.streamingId == null) {
        if (!text.trim()) break;
        const id = allocateItemId(next);
        next.streamingId = id;
        next.items = [
          ...next.items,
          {
            kind: 'msg',
            id,
            role: 'assistant',
            content: text,
            ts: eventTs(msg),
            streaming: true,
            streamDraft: true,
          },
        ];
        next.lastAssistantText = text;
      } else {
        const currentStreamingId = next.streamingId;
        next.items = next.items.map((item) => {
          if (item.id !== currentStreamingId) return item;
          const merged = (item.content || '') + text;
          next.lastAssistantText = merged;
          return { ...item, content: merged, ts: eventTs(msg) };
        });
      }
      break;
    }
    case 'tool_start': {
      finalizeStreaming(next);
      const id = allocateItemId(next);
      next.toolMap.set(toolKey(p), id);
      const tool = {
        isTaskComplete: !!p.is_task_complete,
        isDone: false,
        success: null,
        tool: p.tool || '',
        toolCallId: p.tool_call_id || p.call_id || p.id || '',
        toolIndex: p.tool_index ?? null,
        args: cloneJsonLike(p.args),
        startedAtMs: eventTs(msg),
        displayOverride: p.display_override || '',
        title: p.display_override || p.command_preview || `${p.tool || ''}  ${JSON.stringify(p.args || {})}`,
        tailLines: [],
        currentPartial: '',
        totalLines: 0,
        totalBytes: 0,
        elapsed: 0,
        summary: p.is_task_complete ? { object: (p.args && p.args.summary) || '完成' } : null,
        output: '',
        hunks: [],
        attachments: [],
        metadata: null,
        askUserQuestionResult: null,
      };
      next.items = [...next.items, { kind: 'tool', id, tool, ts: eventTs(msg) }];
      break;
    }
    case 'tool_update': {
      const id = next.toolMap.get(toolKey(p));
      if (!id) break;
      next.items = next.items.map((item) => {
        if (item.id !== id || item.kind !== 'tool') return item;
        return {
          ...item,
          ts: eventTs(msg),
          tool: {
            ...item.tool,
            tailLines: p.tail_lines || item.tool.tailLines,
            currentPartial: p.current_partial || '',
            totalLines: p.total_lines || item.tool.totalLines,
            totalBytes: p.total_bytes || item.tool.totalBytes,
            elapsed: p.elapsed_seconds || item.tool.elapsed,
            toolCallId: p.tool_call_id || item.tool.toolCallId || '',
            toolIndex: p.tool_index ?? item.tool.toolIndex ?? null,
          },
        };
      });
      break;
    }
    case 'tool_end': {
      const key = toolKey(p);
      const id = next.toolMap.get(key);
      next.toolMap.delete(key);
      if (!id) break;
      next.items = next.items.map((item) => {
        if (item.id !== id || item.kind !== 'tool') return item;
        return {
          ...item,
          ts: eventTs(msg),
          tool: {
            ...item.tool,
            isDone: true,
            success: !!p.success,
            summary: p.summary || item.tool.summary,
            output: p.output || '',
            hunks: Array.isArray(p.hunks) ? p.hunks : [],
            attachments: normalizeAttachmentList(p.attachments),
            metadata: p.metadata || item.tool.metadata || null,
            askUserQuestionResult: normalizeAskUserQuestionResult(p.metadata) || item.tool.askUserQuestionResult || null,
            elapsed: p.elapsed_seconds || item.tool.elapsed,
            toolCallId: p.tool_call_id || item.tool.toolCallId || '',
            toolIndex: p.tool_index ?? item.tool.toolIndex ?? null,
          },
        };
      });
      break;
    }
    case 'usage': {
      next.tokenUsage = normalizeUsagePayload(p, eventTs(msg));
      break;
    }
    case 'goal_updated': {
      next.goal = cloneGoal(p.goal);
      break;
    }
    case 'goal_cleared': {
      next.goal = null;
      break;
    }
    case 'todo_updated': {
      const todos = normalizeTodos(p.todos);
      next.todos = todos;
      next.todoSummary = normalizeTodoSummary(p.summary, todos);
      break;
    }
    case 'session_updated': {
      if (Object.prototype.hasOwnProperty.call(p, 'title')) {
        next.title = p.title || '';
      }
      break;
    }
    case 'busy_changed': {
      const wasBusy = !!state?.busy;
      const outcome = typeof p.outcome === 'string' ? p.outcome : '';
      const completedOutcome = !outcome || outcome === 'completed';
      next.busy = !!p.busy;
      next.status = next.busy ? 'running' : 'idle';
      if (next.busy && !wasBusy) {
        // 回合开始 → 重置桌面通知用的回合标记
        next.turnHadAssistantText = false;
        next.lastAssistantText = '';
      }
      if (!next.busy) {
        next.activity = null;
        finalizeStreaming(next);
        if (wasBusy) next.turns = (next.turns || 0) + 1;
        if (wasBusy && completedOutcome && next.turnHadAssistantText) {
          effects.push({
            type: 'turn_completed',
            payload: { final_assistant_text: next.lastAssistantText || '' },
          });
        }
        // The daemon emits busy_changed(false) followed by done. Consume the
        // marker here so the trailing done frame cannot emit a duplicate toast.
        if (wasBusy) {
          next.turnHadAssistantText = false;
          next.lastAssistantText = '';
        }
      }
      break;
    }
    case 'done': {
      const wasBusy = !!state?.busy;
      const outcome = typeof p.outcome === 'string' ? p.outcome : '';
      const completedOutcome = !outcome || outcome === 'completed';
      next.busy = false;
      next.status = 'idle';
      next.activity = null;
      finalizeStreaming(next);
      if (wasBusy && completedOutcome && next.turnHadAssistantText) {
        effects.push({
          type: 'turn_completed',
          payload: { final_assistant_text: next.lastAssistantText || '' },
        });
      }
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      break;
    }
    case 'error':
      next.busy = false;
      next.status = 'error';
      next.error = p.reason || '';
      next.activity = null;
      finalizeStreaming(next);
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      appendTerminationNotice(next, msg, { ...p, source: p.source || 'server' });
      effects.push({ type: 'error', payload: p });
      break;
    case 'turn_aborted':
      next.busy = false;
      next.status = 'idle';
      next.activity = null;
      finalizeStreaming(next);
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      appendTerminationNotice(next, msg, { ...p, source: 'user' });
      break;
    case 'permission_request':
      effects.push({ type: 'permission_request', payload: p });
      break;
    case 'question_request':
      effects.push({ type: 'question_request', payload: p });
      break;
    default:
      break;
  }

  return { state: next, effects };
}

export function loadTranscriptHistory(state, data = {}) {
  const current = state || createTranscriptState();
  let next = createTranscriptState({
    title: current.title || '',
    status: current.status || 'idle',
    isLive: !!current.isLive,
    lastSeq: 0,
    loadState: 'loaded',
  });
  const effects = [];
  const { messages: msgs, turnTimings } = splitTranscriptMessages(data.messages);

  next.turnTimings = turnTimings;
  next.items = applyTurnTimingsToItems(historyItemsFromMessages(next, msgs), turnTimings);

  const restoredTitle = titleFromMessages(msgs);
  if (restoredTitle) next.title = restoredTitle;

  const seenMessages = new Set(msgs.map((m) => messageKey(m.role || 'system', m.content || '')));
  let pendingStreamEvents = [];
  const flushPendingStreamEvents = () => {
    for (const ev of pendingStreamEvents) {
      const reduced = reduceTranscriptEvent(next, ev);
      next = reduced.state;
      effects.push(...reduced.effects);
    }
    pendingStreamEvents = [];
  };

  for (const ev of (Array.isArray(data.events) ? data.events : [])) {
    if (isStaleSequencedEvent(next, ev)) continue;
    if (ev?.type === 'token' || ev?.type === 'reasoning') {
      pendingStreamEvents.push(ev);
      continue;
    }
    if (ev?.type === 'message') {
      const p = ev.payload || {};
      const key = messageKey(p.role || 'system', p.content || '');
      if (seenMessages.has(key)) {
        pendingStreamEvents = [];
        markEventSeqApplied(next, ev);
        continue;
      }
      seenMessages.add(key);
    }
    flushPendingStreamEvents();
    const reduced = reduceTranscriptEvent(next, ev);
    next = reduced.state;
    effects.push(...reduced.effects);
  }
  flushPendingStreamEvents();

  if (Object.prototype.hasOwnProperty.call(data, 'goal')) {
    next.goal = cloneGoal(data.goal);
  }
  if (Object.prototype.hasOwnProperty.call(data, 'todos')) {
    const todos = normalizeTodos(data.todos);
    next.todos = todos;
    next.todoSummary = normalizeTodoSummary(data.todo_summary ?? data.todoSummary, todos);
  }
  const restoredTurnCount = readRuntimeTurnCount(data);
  if (restoredTurnCount !== null) {
    next.turns = restoredTurnCount;
  }
  const restoredUsage = data.token_usage ?? data.tokenUsage ?? data.latest_token_usage ?? data.latestTokenUsage;
  if (restoredUsage && typeof restoredUsage === 'object') {
    next.tokenUsage = normalizeUsagePayload(restoredUsage, Date.now());
  }
  if (data.busy === true) {
    next.busy = true;
    next.status = 'running';
  } else if (data.busy === false && next.status !== 'error') {
    next.busy = false;
    next.status = 'idle';
  }

  return { state: next, effects };
}

export function canLiveMonitorSession(sessionRef, live = 'auto') {
  if (live === true) return true;
  if (live === false) return false;
  const ref = normalizeSessionRef(sessionRef);
  if (!ref) return false;
  const status = ref.status || ref.attention_state || ref.read_state || '';
  return !!(
    ref.active ||
    ref.busy ||
    status === 'running' ||
    status === 'waiting' ||
    status === 'in_progress'
  );
}

export function projectCompactTranscriptItems(items, limit = 6) {
  const source = Array.isArray(items) ? items : [];
  const boundedLimit = Math.max(1, Number(limit) || 1);
  return source.slice(-boundedLimit);
}

function dispatchEffects(effects, sid, options) {
  if (!effects || effects.length === 0) return;
  for (const effect of effects) {
    const payload = { ...(effect.payload || {}) };
    if (sid && !payload.session_id) payload.session_id = sid;
    if (effect.type === 'error') options.onError?.(payload.reason || '');
    if (effect.type === 'permission_request') options.onPermissionRequest?.(payload);
    if (effect.type === 'question_request') options.onQuestionRequest?.(payload);
    if (effect.type === 'turn_completed') options.onTurnCompleted?.(payload);
  }
}

export function useSessionTranscript(sessionRef, options = {}) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef), [sessionRef]);
  const sid = ref?.sessionId || '';
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  const liveMode = options.live ?? 'auto';
  const isLive = !!sid && canLiveMonitorSession(ref, liveMode);
  const initialTitle = sid ? sessionDisplayTitle(ref) : '';
  const [state, setState] = useState(() => createTranscriptState({ title: initialTitle, isLive, loadState: sid ? 'loading' : 'idle' }));
  const stateRef = useRef(state);
  const optionsRef = useRef(options);

  useEffect(() => { optionsRef.current = options; }, [options]);
  useEffect(() => { stateRef.current = state; }, [state]);

  const applyEvent = useCallback((msg, { emitEffects = true } = {}) => {
    const prevState = stateRef.current;
    const reduced = reduceTranscriptEvent(prevState, msg);
    // transcript_replace 是重试/compact 的有意重置,合法变短,不报警。
    if (msg?.type !== 'transcript_replace') {
      detectAssistantTailRegression(`event:${msg?.type || '?'}`, prevState, reduced.state);
    }
    stateRef.current = reduced.state;
    setState(reduced.state);
    if (emitEffects) dispatchEffects(reduced.effects, sid, optionsRef.current);
    return reduced.state;
  }, [sid]);

  const setTitle = useCallback((title) => {
    const nextState = { ...stateRef.current, title: title || '' };
    stateRef.current = nextState;
    setState(nextState);
  }, []);

  const getState = useCallback(() => stateRef.current, []);

  const updateState = useCallback((producer) => {
    const currentState = stateRef.current;
    const nextState = typeof producer === 'function' ? producer(currentState) : producer;
    if (!nextState || nextState === currentState) return currentState;
    stateRef.current = nextState;
    setState(nextState);
    return nextState;
  }, []);

  useEffect(() => {
    const baseTitle = sid ? sessionDisplayTitle(ref) : '';
    const reset = createTranscriptState({
      title: baseTitle,
      isLive,
      loadState: sid ? 'loading' : 'idle',
    });
    stateRef.current = reset;
    setState(reset);
    if (!sid) return undefined;

    let off = false;
    api.getMessages(sid, 0).then((data) => {
      if (off) return;
      const loaded = loadTranscriptHistory(stateRef.current, data || {});
      // 防回退:实时 WS 可能在 getMessages(0) 解析期间已累积了更完整的当前
      // 回合内容,而这份 REST 快照更旧(messages 尚未含进行中的 assistant)。
      // 直接覆盖会把界面截断,这里保留更完整的实时尾巴。
      const guarded = preserveLiveAssistantTailOnLoad(loaded.state, stateRef.current);
      const nextState = {
        ...guarded,
        isLive,
        loadState: 'loaded',
      };
      detectAssistantTailRegression('load', stateRef.current, nextState);
      stateRef.current = nextState;
      setState(nextState);
      dispatchEffects(loaded.effects, sid, optionsRef.current);

      const loadedSeq = nextState.lastSeq || 0;
      const replaySince = replaySinceForLiveCatchup({ isLive, loadedSeq });
      if (replaySince !== null) {
        api.getMessages(sid, replaySince).then((replayData) => {
          if (off) return;
          const replayEvents = Array.isArray(replayData)
            ? replayData
            : (Array.isArray(replayData?.events) ? replayData.events : []);
          if (replayEvents.length > 0) {
            const replayed = applyTranscriptReplayEvents(stateRef.current, replayEvents);
            const replayState = replayed.state;
            detectAssistantTailRegression('catchup', stateRef.current, replayState);
            stateRef.current = replayState;
            setState(replayState);
            dispatchEffects(replayed.effects, sid, optionsRef.current);
            return;
          }
          if (loadedSeq === 0 && Array.isArray(replayData?.messages)) {
            const refreshed = loadTranscriptHistory(stateRef.current, replayData);
            const refreshedState = {
              ...refreshed.state,
              isLive,
              loadState: 'loaded',
            };
            detectAssistantTailRegression('refresh', stateRef.current, refreshedState);
            stateRef.current = refreshedState;
            setState(refreshedState);
            dispatchEffects(refreshed.effects, sid, optionsRef.current);
          }
        }).catch(() => {});
      }
    }).catch((error) => {
      if (off) return;
      const nextState = {
        ...stateRef.current,
        loadState: 'error',
        error: error?.message || 'load failed',
      };
      stateRef.current = nextState;
      setState(nextState);
      optionsRef.current.onError?.('加载会话失败:' + (error?.message || ''));
    });

    return () => { off = true; };
  }, [api, isLive, ref, sid]);

  useEffect(() => {
    if (!sid || !isLive) return undefined;
    connection.reconfigure({ port: ref?.port || '', token: ref?.token || '' });
    const handler = (event) => {
      const msg = event.detail || {};
      const msgSid = msg.session_id || msg.payload?.session_id || '';
      if (msgSid && msgSid !== sid) return;
      applyEvent(msg);
    };
    connection.addEventListener('message', handler);
    connection.retainSession(sid);
    return () => {
      connection.removeEventListener('message', handler);
      connection.releaseSession(sid);
    };
  }, [applyEvent, isLive, ref?.port, ref?.token, sid]);

  return {
    ...state,
    title: state.title || initialTitle,
    isLive,
    loadState: state.loadState,
    applyEvent,
    setTitle,
    getState,
    updateState,
  };
}
