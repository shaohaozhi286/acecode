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

function cloneState(state) {
  return {
    ...state,
    items: Array.isArray(state.items) ? state.items : [],
    toolMap: cloneToolMap(state.toolMap),
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

function toolKey(payload = {}) {
  return payload.tool_call_id || payload.call_id || payload.id || payload.tool || '_anon';
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
    ...overrides,
    toolMap: cloneToolMap(overrides.toolMap),
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
    case 'message': {
      const role = p.role || 'system';
      if (role === 'assistant' && next.streamingId != null) {
        const currentStreamingId = next.streamingId;
        next.streamingId = null;
        next.items = next.items.map((item) => item.id === currentStreamingId
          ? {
              ...item,
              role: 'assistant',
              content: p.content || item.content || '',
              messageId: p.id || item.messageId || '',
              ts: eventTs(msg),
              streaming: false,
            }
          : item);
        break;
      }
      next.streamingId = null;
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: p.id || '',
          role,
          content: p.content || '',
          ts: eventTs(msg),
        },
      ];
      break;
    }
    case 'token': {
      const text = p.text || '';
      if (next.streamingId == null) {
        const id = allocateItemId(next);
        next.streamingId = id;
        next.items = [
          ...next.items,
          { kind: 'msg', id, role: 'assistant', content: text, ts: eventTs(msg), streaming: true },
        ];
      } else {
        const currentStreamingId = next.streamingId;
        next.items = next.items.map((item) => item.id === currentStreamingId
          ? { ...item, content: (item.content || '') + text, ts: eventTs(msg) }
          : item);
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
          },
        };
      });
      break;
    }
    case 'busy_changed':
      next.busy = !!p.busy;
      next.status = next.busy ? 'running' : 'idle';
      if (!p.busy) {
        next.turns = (next.turns || 0) + 1;
        finalizeStreaming(next);
      }
      break;
    case 'done':
      next.busy = false;
      next.status = 'idle';
      finalizeStreaming(next);
      break;
    case 'error':
      next.busy = false;
      next.status = 'error';
      next.error = p.reason || '';
      effects.push({ type: 'error', payload: p });
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
  const msgs = Array.isArray(data.messages) ? data.messages : [];

  next.items = msgs.map((m) => ({
    kind: 'msg',
    id: allocateItemId(next),
    messageId: m.id || '',
    role: m.role || 'system',
    content: m.content || '',
    ts: m.ts || m.timestamp_ms || Date.now(),
  }));

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
  }
}

export function useSessionTranscript(sessionRef, options = {}) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef), [sessionRef]);
  const sid = ref?.sessionId || '';
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  const liveMode = options.live ?? 'auto';
  const isLive = !!sid && canLiveMonitorSession(ref, liveMode);
  const initialTitle = sid ? sessionDisplayTitle(ref, sid) : '';
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
    const baseTitle = sid ? sessionDisplayTitle(ref, sid) : '';
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
