import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createApi } from './api.js';
import { connection } from './connection.js';
import { sessionDisplayTitle, titleFromMessages } from './sessionTitle.js';

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

function cloneTokenUsage(tokenUsage) {
  if (!tokenUsage || typeof tokenUsage !== 'object') return null;
  return { ...tokenUsage };
}

function cloneGoal(goal) {
  if (!goal || typeof goal !== 'object') return null;
  return { ...goal };
}

function cloneState(state) {
  return {
    ...state,
    items: Array.isArray(state.items) ? state.items : [],
    toolMap: cloneToolMap(state.toolMap),
    tokenUsage: cloneTokenUsage(state.tokenUsage),
    goal: cloneGoal(state.goal),
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
  return msg?.timestamp_ms || msg?.ts || Date.now();
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

function readRuntimeTurnCount(data) {
  const raw = data?.turn_count ?? data?.turnCount;
  const value = Number(raw);
  if (!Number.isFinite(value)) return null;
  return Math.max(0, Math.trunc(value));
}

function normalizePersistedToolHunks(metadata) {
  const raw = metadata?.tool_hunks;
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((hunk) => hunk && typeof hunk === 'object' && !Array.isArray(hunk))
    .map((hunk) => ({ ...hunk }));
}

function historyItemFromMessage(next, m) {
  const metadata = m?.metadata && typeof m.metadata === 'object' ? m.metadata : null;
  if ((m?.role || '') === 'tool' && metadata) {
    const summary = normalizePersistedToolSummary(metadata);
    const hunks = normalizePersistedToolHunks(metadata);
    if (summary || hunks.length > 0) {
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
          startedAtMs: m.ts || m.timestamp_ms || Date.now(),
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
        },
        ts: m.ts || m.timestamp_ms || Date.now(),
      };
    }
  }

  return {
    kind: 'msg',
    id: allocateItemId(next),
    messageId: m.id || '',
    role: m.role || 'system',
    content: m.content || '',
    metadata: m.metadata,
    ts: m.ts || m.timestamp_ms || Date.now(),
  };
}

function visibleTranscriptMessages(messages) {
  if (!Array.isArray(messages)) return [];
  return messages.filter((m) => !m?.is_meta && !m?.metadata?.hidden_goal_context);
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
    nextItemId: 1,
    error: '',
    tokenUsage: null,
    goal: null,
    activity: null,
    // turnHadAssistantText / lastAssistantText 用于桌面通知:在 busy=true→false
    // 转换且本回合产生过 assistant 文本时,emit turn_completed effect。reducer 之外
    // 的代码不应直接读 / 写它们。见 openspec/changes/add-desktop-attention-notifications。
    turnHadAssistantText: false,
    lastAssistantText: '',
    ...overrides,
    toolMap: cloneToolMap(overrides.toolMap),
    tokenUsage: cloneTokenUsage(overrides.tokenUsage),
    goal: cloneGoal(overrides.goal),
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
  const next = cloneState(state || createTranscriptState());
  const effects = [];
  const t = msg?.type || '';
  const p = msg?.payload || {};

  if (typeof msg?.seq === 'number' && msg.seq > (next.lastSeq || 0)) {
    next.lastSeq = msg.seq;
  }

  switch (t) {
    case 'transcript_replace': {
      finalizeStreaming(next);
      next.toolMap = new Map();
      const messages = visibleTranscriptMessages(p.messages);
      next.items = messages.map((m) => historyItemFromMessage(next, m));
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
    case 'message': {
      const role = p.role || 'system';
      if (role === 'assistant' && next.streamingId != null) {
        const currentStreamingId = next.streamingId;
        next.streamingId = null;
        const finalContent = p.content || '';
        if (finalContent && finalContent.trim()) {
          next.turnHadAssistantText = true;
          next.lastAssistantText = finalContent;
        }
        next.items = next.items.map((item) => item.id === currentStreamingId
          ? {
              ...item,
              role: 'assistant',
              content: finalContent || item.content || '',
              messageId: p.id || item.messageId || '',
              ts: eventTs(msg),
              streaming: false,
            }
          : item);
        break;
      }
      finalizeStreaming(next);
      const incomingContent = p.content || '';
      if (role === 'assistant' && incomingContent && incomingContent.trim()) {
        next.turnHadAssistantText = true;
        next.lastAssistantText = incomingContent;
      }
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: p.id || '',
          role,
          content: incomingContent,
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
          { kind: 'msg', id, role: 'assistant', content: text, ts: eventTs(msg), streaming: true },
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
      next.streamingId = null;
      const id = allocateItemId(next);
      next.toolMap.set(toolKey(p), id);
      const tool = {
        isTaskComplete: !!p.is_task_complete,
        isDone: false,
        success: null,
        tool: p.tool || '',
        toolCallId: p.tool_call_id || p.call_id || p.id || '',
        toolIndex: p.tool_index ?? null,
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
    case 'busy_changed': {
      const wasBusy = !!state?.busy;
      next.busy = !!p.busy;
      next.status = next.busy ? 'running' : 'idle';
      if (next.busy && !wasBusy) {
        // 回合开始 → 重置桌面通知用的回合标记
        next.turnHadAssistantText = false;
        next.lastAssistantText = '';
      }
      if (!p.busy) {
        next.activity = null;
        next.turns = (next.turns || 0) + 1;
        finalizeStreaming(next);
        if (next.turnHadAssistantText) {
          effects.push({
            type: 'turn_completed',
            payload: { final_assistant_text: next.lastAssistantText || '' },
          });
        }
      }
      break;
    }
    case 'done': {
      next.busy = false;
      next.status = 'idle';
      next.activity = null;
      finalizeStreaming(next);
      if (next.turnHadAssistantText) {
        effects.push({
          type: 'turn_completed',
          payload: { final_assistant_text: next.lastAssistantText || '' },
        });
      }
      break;
    }
    case 'error':
      next.busy = false;
      next.status = 'error';
      next.error = p.reason || '';
      next.activity = null;
      finalizeStreaming(next);
      appendTerminationNotice(next, msg, { ...p, source: p.source || 'server' });
      effects.push({ type: 'error', payload: p });
      break;
    case 'turn_aborted':
      next.busy = false;
      next.status = 'idle';
      next.activity = null;
      finalizeStreaming(next);
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
    lastSeq: current.lastSeq || 0,
    loadState: 'loaded',
  });
  const effects = [];
  const msgs = visibleTranscriptMessages(data.messages);

  next.items = msgs.map((m) => historyItemFromMessage(next, m));

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
    if (typeof ev?.seq === 'number' && ev.seq > (next.lastSeq || 0)) next.lastSeq = ev.seq;
    if (ev?.type === 'token' || ev?.type === 'reasoning') {
      pendingStreamEvents.push(ev);
      continue;
    }
    if (ev?.type === 'message') {
      const p = ev.payload || {};
      const key = messageKey(p.role || 'system', p.content || '');
      if (seenMessages.has(key)) {
        pendingStreamEvents = [];
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
    const reduced = reduceTranscriptEvent(stateRef.current, msg);
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
      const nextState = {
        ...loaded.state,
        isLive,
        loadState: 'loaded',
      };
      stateRef.current = nextState;
      setState(nextState);
      dispatchEffects(loaded.effects, sid, optionsRef.current);
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
    connection.retainSession(sid);
    const handler = (event) => {
      const msg = event.detail || {};
      const msgSid = msg.session_id || msg.payload?.session_id || '';
      if (msgSid && msgSid !== sid) return;
      applyEvent(msg);
    };
    connection.addEventListener('message', handler);
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
  };
}
