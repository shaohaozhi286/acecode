// 主聊天视图:头部(会话名 + 状态 badge)+ 消息流 +
// InputBar + StatusBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示 Codex 风格新任务主页(首条消息提交时才创建 session)。

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createApi } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { InputBar } from './InputBar.jsx';
import { QueueCardList } from './QueueCardList.jsx';
import { QuestionPicker } from './QuestionPicker.jsx';
import { StickyUserContext } from './StickyUserContext.jsx';
import { SidePanel } from './SidePanel.jsx';
import { StatusBar } from './StatusBar.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import {
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  hasSendingQueuedInput,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  retryQueuedInput,
} from '../lib/chatInputQueue.js';
import { findStickyUserContext, sameStickyUserContext } from '../lib/stickyUserContext.js';
import { useSessionTranscript } from '../lib/sessionTranscript.js';
import { maybeNotify } from '../lib/desktopNotify.js';
import {
  modelDisplayLabel,
  modelSelectValue,
  normalizeModelOptions,
  normalizeModelState,
  selectedModelName,
} from '../lib/sessionModel.js';
import { VsIcon } from './Icon.jsx';

function isEditableElement(el) {
  if (!el || el === document.body || el === document.documentElement) return false;
  const tag = String(el.tagName || '').toLowerCase();
  return tag === 'input' || tag === 'textarea' || tag === 'select' || !!el.isContentEditable;
}

function collectRowMetrics(container) {
  if (!container) return [];
  const containerRect = container.getBoundingClientRect();
  return Array.from(container.querySelectorAll('[data-chat-row="true"]')).map((node) => {
    const rect = node.getBoundingClientRect();
    return {
      id: node.getAttribute('data-chat-item-id') || '',
      top: rect.top - containerRect.top + container.scrollTop,
      bottom: rect.bottom - containerRect.top + container.scrollTop,
    };
  });
}

function normalizeSessionRef(sessionRef, sessionId) {
  if (sessionRef && typeof sessionRef === 'object') return sessionRef;
  if (typeof sessionRef === 'string' && sessionRef) return { sessionId: sessionRef };
  if (sessionId) return { sessionId };
  return null;
}

function newSessionRefFrom(ref, sessionId) {
  const next = { sessionId };
  if (!ref || typeof ref !== 'object') return next;
  for (const key of ['workspaceHash', 'workspaceName', 'contextId', 'port', 'token', 'cwd']) {
    if (ref[key] != null) next[key] = ref[key];
  }
  return next;
}

function hasDesktopBridge() {
  return typeof window.aceDesktop_listWorkspaces === 'function';
}

function parseDesktopResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function pathBaseName(path = '') {
  const normalized = String(path || '').replace(/\\/g, '/').replace(/\/+$/, '');
  if (!normalized) return '';
  return normalized.split('/').filter(Boolean).pop() || normalized;
}

function normalizeWorkspaceOption(workspace, fallbackIndex = 0) {
  if (!workspace || typeof workspace !== 'object') return null;
  const hash = workspace.hash || workspace.workspaceHash || workspace.workspace_hash || '';
  const cwd = workspace.cwd || '';
  const name = workspace.name || workspace.workspaceName || pathBaseName(cwd) || hash || `项目 ${fallbackIndex + 1}`;
  return {
    hash,
    cwd,
    name,
    active: !!workspace.active,
    contextId: workspace.contextId || workspace.context_id || 'default',
    port: workspace.port,
    token: workspace.token,
  };
}

function fallbackWorkspaceOption(ref, health) {
  const hash = ref?.workspaceHash || ref?.workspace_hash || '';
  const cwd = ref?.cwd || health?.cwd || '';
  return {
    hash: hash || '__local__',
    cwd,
    name: ref?.workspaceName || ref?.name || pathBaseName(cwd) || '当前项目',
    active: true,
    contextId: ref?.contextId || 'default',
    port: ref?.port,
    token: ref?.token,
  };
}

function isRealWorkspaceHash(hash) {
  return !!hash && hash !== '__local__';
}

export function ChatView({ sessionRef, sessionId, onSessionPromoted, health, onPermissionRequest, onQuestionRequest, questionRequest, onQuestionResolve, showSidePanel = false, sidePanelWidth = 280, onSidePanelResize, sidePanelCollapsed = false, onToggleSidePanel }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const sidRef = useRef(sid);
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  // 桌面通知 — 见 openspec/changes/add-desktop-attention-notifications。
  // transcriptTitleRef 由后面的 effect 与 transcript.title 同步;在 callback 里
  // 通过 ref.current 读最新 title,避免 fireDesktopNotification 进 useCallback deps。
  const transcriptTitleRef = useRef('');
  const fireDesktopNotification = useCallback((type, payload) => {
    if (typeof document === 'undefined') return;
    const cfg = health?.notifications;
    const sessionId = ref?.sessionId || sid || '';
    const workspaceHash = ref?.workspaceHash || '';
    const sessionTitle = transcriptTitleRef.current || '';
    let bodyText = '';
    if (type === 'question') {
      bodyText = String(payload?.question || payload?.prompt || '');
    } else if (type === 'completion') {
      bodyText = String(payload?.final_assistant_text || '');
    }
    maybeNotify({
      type,
      sessionId,
      workspaceHash,
      sessionTitle,
      bodyText,
      activeRef: { sessionId, workspaceHash },
      hasFocus: typeof document.hasFocus === 'function' ? document.hasFocus() : true,
      cfg,
    });
  }, [health?.notifications, ref?.sessionId, ref?.workspaceHash, sid]);

  const transcript = useSessionTranscript(ref, {
    live: true,
    onPermissionRequest,
    onQuestionRequest: (payload) => {
      onQuestionRequest?.(payload);
      fireDesktopNotification('question', payload);
    },
    onTurnCompleted: (payload) => {
      fireDesktopNotification('completion', payload);
    },
    onError: (reason) => toast({
      kind: 'err',
      text: String(reason || '').startsWith('加载会话失败:')
        ? String(reason || '')
        : '错误:' + (reason || ''),
    }),
  });
  const { items, busy, turns, title, status: transcriptStatus, streamingId, applyEvent, setTitle: setTranscriptTitle } = transcript;
  // 让 fireDesktopNotification 拿到最新 title,无需进入它的 useCallback deps。
  useEffect(() => { transcriptTitleRef.current = title || ''; }, [title]);
  const [history,  setHistory]  = useState([]);
  const [homeWorkspaces, setHomeWorkspaces] = useState([]);
  const [homeWorkspaceHash, setHomeWorkspaceHash] = useState('');
  const [homeSubmitting, setHomeSubmitting] = useState(false);
  const [projectDropdownOpen, setProjectDropdownOpen] = useState(false);
  const [modelOptions, setModelOptions] = useState([]);
  const [modelState, setModelState] = useState(null);
  const [pendingModelName, setPendingModelName] = useState('');
  const [modelSwitching, setModelSwitching] = useState(false);
  const scrollRef = useRef(null);
  const inputRef = useRef(null);
  const layoutRef = useRef(null);
  const sidePanelResizeActiveRef = useRef(false);
  const [queueState, setQueueState] = useState(() => createChatInputQueueState());
  const queueStateRef = useRef(queueState);
  const drainRef = useRef(false);
  // 排队消息从 transcript 中分离出来,只喂给 InputBar 上方的 QueueCardList。
  // transcript 只渲染后端真实落库的消息,避免把"草稿/未发送"和"已发送"混在一起。
  const visibleQueuedItems = useMemo(() => buildQueuedMessageItems(queueState, sid), [queueState, sid]);
  const renderedItems = useMemo(() => items, [items]);
  const itemsRef = useRef(renderedItems);
  const stickyRafRef = useRef(0);
  const [stickyUserContext, setStickyUserContext] = useState(null);

  const selectedHomeWorkspace = useMemo(() => {
    return homeWorkspaces.find((w) => w.hash === homeWorkspaceHash)
      || homeWorkspaces[0]
      || fallbackWorkspaceOption(ref, health);
  }, [health, homeWorkspaceHash, homeWorkspaces, ref]);

  useEffect(() => { sidRef.current = sid; }, [sid]);
  useEffect(() => { queueStateRef.current = queueState; }, [queueState]);

  useEffect(() => {
    if (!sid) {
      setModelState(null);
      setPendingModelName('');
      setModelSwitching(false);
      return undefined;
    }
    let cancelled = false;
    setPendingModelName('');
    setModelSwitching(false);

    api.listModels()
      .then((list) => {
        if (!cancelled) setModelOptions(normalizeModelOptions(list));
      })
      .catch(() => {
        if (!cancelled) setModelOptions([]);
      });

    api.getSessionModel(sid, ref?.workspaceHash || '')
      .then((state) => {
        if (!cancelled) setModelState(normalizeModelState(state));
      })
      .catch(() => {
        if (!cancelled) {
          setModelState(normalizeModelState({
            name: ref?.model_name || ref?.model_preset || '',
            provider: ref?.provider || '',
            model: ref?.model || '',
            context_window: ref?.context_window || 0,
            is_legacy: ref?.model_is_legacy,
          }));
        }
      });

    return () => { cancelled = true; };
  }, [api, ref?.context_window, ref?.model, ref?.model_is_legacy, ref?.model_name, ref?.model_preset, ref?.provider, ref?.workspaceHash, sid]);

  const updateQueueState = useCallback((updater) => {
    const base = queueStateRef.current;
    const next = typeof updater === 'function' ? updater(base) : updater;
    queueStateRef.current = next;
    setQueueState(next);
  }, []);

  const measureStickyContext = useCallback(() => {
    const el = scrollRef.current;
    if (!el) {
      setStickyUserContext(null);
      return;
    }
    const nextContext = findStickyUserContext({
      items: itemsRef.current,
      rowMetrics: collectRowMetrics(el),
      scrollTop: el.scrollTop,
      clientHeight: el.clientHeight,
      scrollHeight: el.scrollHeight,
    });
    setStickyUserContext((prev) => (
      sameStickyUserContext(prev, nextContext) ? prev : nextContext
    ));
  }, []);

  const scheduleStickyMeasure = useCallback(() => {
    if (stickyRafRef.current) cancelAnimationFrame(stickyRafRef.current);
    stickyRafRef.current = requestAnimationFrame(() => {
      stickyRafRef.current = 0;
      measureStickyContext();
    });
  }, [measureStickyContext]);

  const focusChatInput = useCallback((force = false) => {
    if (questionRequest) return;
    if (!force && isEditableElement(document.activeElement)) return;
    inputRef.current?.focus();
  }, [questionRequest]);

  // 自动滚到底
  useEffect(() => {
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [renderedItems, busy]);

  useEffect(() => {
    let timer = 0;
    const id = requestAnimationFrame(() => {
      focusChatInput(true);
      timer = window.setTimeout(() => focusChatInput(true), 80);
    });
    return () => {
      cancelAnimationFrame(id);
      if (timer) window.clearTimeout(timer);
    };
  }, [sid, focusChatInput]);

  useEffect(() => {
    const focusIfCurrentWindow = () => {
      requestAnimationFrame(() => focusChatInput(false));
    };
    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') focusIfCurrentWindow();
    };
    window.addEventListener('focus', focusIfCurrentWindow);
    window.addEventListener('pageshow', focusIfCurrentWindow);
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      window.removeEventListener('focus', focusIfCurrentWindow);
      window.removeEventListener('pageshow', focusIfCurrentWindow);
      document.removeEventListener('visibilitychange', onVisibilityChange);
    };
  }, [focusChatInput]);

  useLayoutEffect(() => {
    itemsRef.current = renderedItems;
    scheduleStickyMeasure();
  }, [renderedItems, scheduleStickyMeasure]);

  useEffect(() => {
    setStickyUserContext(null);
    scheduleStickyMeasure();
  }, [sid, scheduleStickyMeasure]);

  useEffect(() => () => {
    if (stickyRafRef.current) {
      cancelAnimationFrame(stickyRafRef.current);
      stickyRafRef.current = 0;
    }
  }, []);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;

    const onResize = () => scheduleStickyMeasure();
    window.addEventListener('resize', onResize);

    let mutationObserver = null;
    if (typeof MutationObserver !== 'undefined') {
      mutationObserver = new MutationObserver(scheduleStickyMeasure);
      mutationObserver.observe(el, { childList: true, subtree: true, characterData: true });
    }

    let resizeObserver = null;
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(scheduleStickyMeasure);
      resizeObserver.observe(el);
    }

    scheduleStickyMeasure();
    return () => {
      window.removeEventListener('resize', onResize);
      mutationObserver?.disconnect();
      resizeObserver?.disconnect();
    };
  }, [sid, scheduleStickyMeasure]);

  useEffect(() => {
    if (sid) return undefined;
    let cancelled = false;

    const load = async () => {
      let options = [];
      try {
        const list = await api.listWorkspaces();
        options = Array.isArray(list)
          ? list.map((w, i) => normalizeWorkspaceOption(w, i)).filter(Boolean)
          : [];
      } catch {
        options = [];
      }

      if (options.length === 0 && hasDesktopBridge()) {
        try {
          const list = parseDesktopResult(await window.aceDesktop_listWorkspaces());
          options = Array.isArray(list)
            ? list.map((w, i) => normalizeWorkspaceOption(w, i)).filter(Boolean)
            : [];
        } catch {
          options = [];
        }
      }

      const fallback = fallbackWorkspaceOption(ref, health);
      if (options.length === 0 || (isRealWorkspaceHash(fallback.hash) && !options.some((w) => w.hash === fallback.hash))) {
        options = [fallback, ...options];
      }
      if (options.length === 0) options = [fallback];

      if (cancelled) return;
      setHomeWorkspaces(options);
      setHomeWorkspaceHash((prev) => {
        if (ref?.workspaceHash && options.some((w) => w.hash === ref.workspaceHash)) return ref.workspaceHash;
        if (prev && options.some((w) => w.hash === prev)) return prev;
        return options.find((w) => w.active)?.hash || options[0]?.hash || '';
      });
    };

    load();
    return () => { cancelled = true; };
  }, [api, health, ref, sid]);

  // 拉 history(per-cwd)
  useEffect(() => {
    const cwd = sid
      ? (ref?.cwd || health?.cwd || '')
      : (selectedHomeWorkspace?.cwd || ref?.cwd || health?.cwd || '');
    if (!cwd) return;
    api.getHistory(cwd, 200)
      .then((r) => setHistory(Array.isArray(r) ? r : []))
      .catch(() => {});
  }, [api, health?.cwd, ref?.cwd, selectedHomeWorkspace?.cwd, sid]);

  const recordInputHistory = useCallback((text) => {
    api.appendHistory(text).catch(() => {});
    setHistory((h) => [...h, text]);
  }, [api]);

  const enqueueInput = useCallback((text) => {
    if (!sid) return;
    updateQueueState((prev) => enqueueQueuedInput(prev, { sessionId: sid, text }));
    recordInputHistory(text);
    if (!ref?.title) setTranscriptTitle(text);
  }, [recordInputHistory, ref?.title, setTranscriptTitle, sid, updateQueueState]);

  const cancelQueued = useCallback((queuedId) => {
    updateQueueState((prev) => cancelQueuedInput(prev, queuedId));
  }, [updateQueueState]);

  const retryQueued = useCallback((queuedId) => {
    updateQueueState((prev) => retryQueuedInput(prev, queuedId));
  }, [updateQueueState]);

  const submit = useCallback((text) => {
    if (!sid) {
      // 自动新建会话并让 daemon 直接接管首条消息。
      const trimmed = String(text || '').trim();
      if (!trimmed || homeSubmitting) return;
      const target = selectedHomeWorkspace || fallbackWorkspaceOption(ref, health);
      const targetHash = target?.hash || '';
      const create = isRealWorkspaceHash(targetHash)
        ? api.createWorkspaceSession(targetHash, { initial_user_message: text, auto_start: true })
        : api.createSession({ initial_user_message: text, auto_start: true });
      setHomeSubmitting(true);
      create.then((r) => {
        const id = r && (r.session_id || r.id);
        if (id) {
          const next = newSessionRefFrom(ref, id);
          if (r.workspace_hash || isRealWorkspaceHash(targetHash)) {
            next.workspaceHash = r.workspace_hash || targetHash;
          }
          next.workspaceName = target?.name || ref?.workspaceName;
          next.cwd = r.cwd || target?.cwd || ref?.cwd;
          next.title = text;
          onSessionPromoted?.(next);
          recordInputHistory(text);
        }
      }).catch((e) => toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') }))
        .finally(() => setHomeSubmitting(false));
      return;
    }
    if (busy) {
      enqueueInput(text);
      return;
    }
    api.sendInput(sid, text).catch((e) => {
      toast({ kind: 'err', text: '发送失败:' + (e.message || '') });
      applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
    });
    recordInputHistory(text);
    if (!ref?.title) setTranscriptTitle(text);
    applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
  }, [sid, busy, api, ref, health, homeSubmitting, selectedHomeWorkspace, onSessionPromoted, recordInputHistory, enqueueInput, applyEvent, setTranscriptTitle]);

  const drainQueuedInput = useCallback(() => {
    const targetSid = sidRef.current;
    if (!targetSid || busy || drainRef.current) return;
    const queuedItem = nextQueuedInput(queueStateRef.current, targetSid);
    if (!queuedItem) return;

    drainRef.current = true;
    updateQueueState((prev) => markQueuedInputSending(prev, queuedItem.queued.id));
    api.sendInput(targetSid, queuedItem.content)
      .then(() => {
        if (sidRef.current === targetSid) {
          applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
        }
      })
      .catch((e) => {
        const message = e?.message || '发送失败';
        updateQueueState((prev) => markQueuedInputFailed(prev, queuedItem.queued.id, message));
        toast({ kind: 'err', text: '排队发送失败:' + message });
      })
      .finally(() => {
        drainRef.current = false;
      });
  }, [api, applyEvent, busy, updateQueueState]);

  const prevBusyRef = useRef(busy);
  useEffect(() => {
    const wasBusy = prevBusyRef.current;
    prevBusyRef.current = busy;
    if (!sid || busy) return;
    if (wasBusy || !hasSendingQueuedInput(queueState, sid)) {
      drainQueuedInput();
    }
  }, [busy, drainQueuedInput, queueState, sid]);

  useEffect(() => {
    if (!sid || items.length === 0) return;
    let nextState = queueStateRef.current;
    let changed = false;
    for (const item of items) {
      if (item.kind !== 'msg' || item.role !== 'user') continue;
      const candidate = completeQueuedInputForMessage(nextState, {
        sessionId: sid,
        content: item.content || '',
        ts: item.ts,
      });
      if (candidate !== nextState) {
        nextState = candidate;
        changed = true;
      }
    }
    if (changed) updateQueueState(nextState);
  }, [items, sid, updateQueueState]);

  const abort = useCallback(() => connection.sendAbort(sid), [sid]);

  const switchSessionModel = useCallback(async (name) => {
    const nextName = String(name || '');
    const currentName = selectedModelName(modelState);
    if (!sid || !nextName || nextName === currentName || modelSwitching) return;
    setPendingModelName(nextName);
    setModelSwitching(true);
    try {
      const nextState = normalizeModelState(await api.switchModel(sid, nextName));
      setModelState(nextState);
      toast({ kind: 'ok', text: '已切换到 ' + modelDisplayLabel(nextState, nextName) });
    } catch (e) {
      toast({ kind: 'err', text: '模型切换失败:' + (e?.message || '') });
    } finally {
      setPendingModelName('');
      setModelSwitching(false);
    }
  }, [api, modelState, modelSwitching, sid]);

  const startSidePanelResize = useCallback((event) => {
    if (!showSidePanel || !sid || !onSidePanelResize) return;
    if (event.button != null && event.button !== 0) return;
    if (sidePanelResizeActiveRef.current) return;
    sidePanelResizeActiveRef.current = true;
    event.preventDefault();
    const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = sidePanelWidth;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      onSidePanelResize(startWidth + startX - moveEvent.clientX, contentWidth);
    };
    const onStop = () => {
      sidePanelResizeActiveRef.current = false;
      document.body.classList.remove('ace-resizing');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onStop);
      window.removeEventListener('pointercancel', onStop);
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onStop);
    };

    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onStop, { once: true });
    window.addEventListener('pointercancel', onStop, { once: true });
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onStop, { once: true });
  }, [onSidePanelResize, showSidePanel, sid, sidePanelWidth]);

  const onSidePanelHandleKeyDown = useCallback((event) => {
    if (!onSidePanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
      onSidePanelResize(sidePanelWidth + delta, contentWidth);
    }
  }, [onSidePanelResize, sidePanelWidth]);

  // fork: 调后端 POST /api/sessions/:id/fork,成功后切到新 session(同 ref)。
  // 失败弹 toast 不打断当前 session。新 session 不会自动启 turn,
  // 用户在新 session 自己输入消息才开始。
  const forkAndSwitch = useCallback(async (messageId) => {
    if (!sid || !messageId) return;
    try {
      const r = await api.forkSession(sid, messageId, '');
      if (!r || !r.session_id) {
        toast({ kind: 'err', text: '分叉失败:无 session_id' });
        return;
      }
      onSessionPromoted?.({
        ...newSessionRefFrom(ref, r.session_id),
        title: r.title,
        workspaceHash: r.workspace_hash || ref?.workspaceHash,
        cwd: r.cwd || ref?.cwd,
      });
      toast({ kind: 'ok', text: '已分叉到 ' + (r.title || r.session_id) });
    } catch (e) {
      toast({ kind: 'err', text: '分叉失败:' + (e?.message || '') });
    }
  }, [sid, api, ref, onSessionPromoted]);

  const status = useMemo(() => {
    if (!sid) return null;
    return busy || transcriptStatus === 'running' ? 'running' : 'idle';
  }, [sid, busy, transcriptStatus]);

  const currentModelLabel = modelDisplayLabel(modelState, ref?.model_name || ref?.model_preset || ref?.model || '加载中');
  const currentModelName = modelSelectValue(modelState, pendingModelName);
  const displayedModelOptions = useMemo(() => {
    const currentName = selectedModelName(modelState);
    if (!currentName || modelOptions.some((m) => m.name === currentName)) return modelOptions;
    const normalized = normalizeModelState(modelState);
    return normalized ? [normalized, ...modelOptions] : modelOptions;
  }, [modelOptions, modelState]);

  // SidePanel 「变更」tab 的数据源:把 items 里 tool 项的 hunks 抽成消息格式。
  // 必须放在 early return 之前,否则空态/有 session 之间 hooks 数量不一致 → React #310。
  const sidePanelMessages = useMemo(() => {
    if (!showSidePanel) return [];
    return items
      .filter((it) => it.kind === 'tool' && Array.isArray(it.tool?.hunks) && it.tool.hunks.length > 0)
      .map((it) => ({ hunks: it.tool.hunks }));
  }, [items, showSidePanel]);

  const questionForView = useMemo(() => {
    if (!questionRequest) return null;
    const reqSid = questionRequest.session_id || '';
    if (reqSid && (!sid || reqSid !== sid)) return null;
    return questionRequest;
  }, [questionRequest, sid]);

  const resolveQuestion = useCallback(() => {
    onQuestionResolve?.();
    requestAnimationFrame(() => inputRef.current?.focus());
  }, [onQuestionResolve]);

  // 空态:没选会话
  if (!sid) {
    const homeProjectName = selectedHomeWorkspace?.name || '当前项目';
    const homeHints = [
      { icon: 'edit', title: '编辑代码', desc: '让 Agent 帮你重构、修 bug、加测试' },
      { icon: 'searchSparkle', title: '探索代码库', desc: '问“这个函数在哪里被调用”' },
      { icon: 'run', title: '运行命令', desc: 'bash / npm / git 等 Agent 会逐步确认' },
      { icon: 'lightbulb', title: '使用 Skills', desc: '预定义工作流，从侧边栏开启' },
    ];
    return (
      <div className="flex-1 flex flex-col bg-bg">
        <div className="h-9 px-3 flex items-center bg-surface border-b border-border shrink-0">
          <span className="text-fg-mute text-[12px]">未选择会话</span>
        </div>
        <div className="ace-home-panel flex-1">
          <div className="ace-home-content">
            <img src="/acecode-logo.png" alt="ACECode" width="64" height="64" className="ace-home-logo select-none" draggable="false" />
            <h1 className="ace-home-title">我们该在 {homeProjectName} 中做什么？</h1>
            <InputBar
              ref={inputRef}
              variant="hero"
              history={history}
              onSubmit={submit}
              disabled={!!questionForView || homeSubmitting}
              placeholder="向 ACECode 描述任务，或输入 / 命令..."
            />
            <div className="relative mr-auto ml-0">
              <button
                type="button"
                className="ace-home-project-row group"
                onClick={() => setProjectDropdownOpen(!projectDropdownOpen)}
                title={selectedHomeWorkspace?.cwd || homeProjectName}
              >
                <VsIcon name="folder" size={15} />
                <span className="text-fg-mute">项目</span>
                <span className="ace-home-project-select truncate">
                  {homeWorkspaces.find(w => w.hash === homeWorkspaceHash)?.name || selectedHomeWorkspace?.name || '当前项目'}
                </span>
                <VsIcon name="expandDown" size={14} className="opacity-50 group-hover:opacity-100 transition-opacity -ml-[2px]" />
                {homeSubmitting && <span className="ace-spinner w-3 h-3 ml-1" />}
              </button>

              {projectDropdownOpen && (
                <>
                  <div
                    className="fixed inset-0 z-40"
                    onClick={() => setProjectDropdownOpen(false)}
                  />
                  <div className="absolute top-full left-0 mt-1.5 w-[280px] max-h-[40vh] overflow-y-auto bg-surface border border-border ace-shadow rounded-xl z-50 py-1.5 ace-scrollbar">
                    <div className="px-3 pb-1 mb-1 text-[11px] font-semibold text-fg-mute border-b border-border/50 uppercase tracking-wider">
                      工作区
                    </div>
                    {homeWorkspaces.map((w) => (
                      <button
                        key={w.hash || w.cwd || w.name}
                        type="button"
                        className={clsx(
                          "w-full text-left px-3 py-1.5 text-[13px] flex flex-col gap-[2px] transition-colors",
                          w.hash === homeWorkspaceHash ? "bg-accent/10 text-accent font-medium" : "text-fg hover:bg-surface-hi"
                        )}
                        onClick={() => {
                          setHomeWorkspaceHash(w.hash);
                          setProjectDropdownOpen(false);
                        }}
                      >
                        <div className="truncate leading-tight">{w.name}</div>
                        <div className={clsx("text-[10.5px] truncate leading-tight", w.hash === homeWorkspaceHash ? "text-accent/60" : "text-fg-mute/70")} title={w.cwd}>
                          {w.cwd.replace(/\\/g, '/')}
                        </div>
                      </button>
                    ))}
                  </div>
                </>
              )}
            </div>
            <div className="ace-home-hints">
              {homeHints.map((c) => (
                <div key={c.title} className="ace-home-hint-card">
                  <VsIcon name={c.icon} size={22} className="mb-1" />
                  <div className="text-[13px] font-semibold mb-0.5">{c.title}</div>
                  <div className="text-[11px] text-fg-mute leading-relaxed">{c.desc}</div>
                </div>
              ))}
            </div>
          </div>
        </div>
        {questionForView && (
          <QuestionPicker request={questionForView} onResolve={resolveQuestion} />
        )}
        <StatusBar model="—" turns={0} branch={health?.branch || ''} />
      </div>
    );
  }

  const sidePanelCwd = ref?.cwd || health?.cwd || '';

  const sidePanelMounted = showSidePanel && !!sid;

  return (
    <div ref={layoutRef} className="flex-1 flex min-w-0 ace-chat-layout">
      <div className="flex-1 flex flex-col min-w-0 relative">
      <div className="h-9 px-3 flex items-center justify-between bg-surface border-b border-border shrink-0 gap-2">
        <div className="flex items-center gap-2 min-w-0">
          <span className="text-[13px] font-semibold text-fg truncate">{title}</span>
          <span
            className={clsx(
              'px-2.5 py-0.5 rounded-full text-[10px] font-medium border whitespace-nowrap',
              status === 'running' && 'bg-ok-bg text-ok border-ok-border',
              status === 'idle'    && 'bg-surface-hi text-fg-mute border-transparent',
            )}
          >
            {status === 'running' ? '运行中' : '空闲'}
          </span>
        </div>
        {sidePanelMounted && sidePanelCollapsed && onToggleSidePanel && (
          <button
            type="button"
            onClick={onToggleSidePanel}
            className="ace-side-panel-expand-fab"
            title="展开右侧面板"
            aria-label="展开右侧面板"
          >
            <VsIcon name="expandRight" size={14} />
          </button>
        )}
      </div>

      <div className="relative flex-1 min-h-0">
        <div
          ref={scrollRef}
          onScroll={scheduleStickyMeasure}
          className="h-full overflow-y-auto px-3.5 py-3 flex flex-col gap-3"
        >
          {renderedItems.map((it) => (
            <div
              key={it.id}
              className="ace-chat-row flex flex-col"
              data-chat-row="true"
              data-chat-item-id={String(it.id)}
              data-chat-kind={it.kind || ''}
              data-chat-role={it.kind === 'msg' ? (it.role || '') : (it.kind || '')}
              data-chat-user-message={it.kind === 'msg' && it.role === 'user' ? 'true' : undefined}
            >
              {it.kind === 'tool' ? (
                <ToolBlock entry={it.tool} />
              ) : (
                <Message
                  role={it.role} content={it.content} ts={it.ts}
                  streaming={it.streaming}
                  messageId={it.messageId}
                  metadata={it.metadata}
                  onFork={forkAndSwitch}
                />
              )}
            </div>
          ))}
          {busy && streamingId == null && (
            <div className="flex gap-2 max-w-[85%]">
              <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center mt-[2px]">A</div>
              <div className="flex gap-1 py-2 px-3">
                {[0,1,2].map((i) => (
                  <span
                    key={i}
                    className="w-1.5 h-1.5 rounded-full bg-fg-mute"
                    style={{ animation: `ace-pulse 1.2s ease-in-out ${i * 0.2}s infinite` }}
                  />
                ))}
              </div>
            </div>
          )}
        </div>
        <StickyUserContext context={stickyUserContext} />
      </div>

      {questionForView && (
        <QuestionPicker request={questionForView} onResolve={resolveQuestion} />
      )}

      <QueueCardList
        items={visibleQueuedItems}
        onCancel={cancelQueued}
        onRetry={retryQueued}
      />

      <InputBar
        ref={inputRef}
        busy={busy}
        history={history}
        onSubmit={submit}
        onAbort={abort}
        disabled={!!questionForView}
        placeholder={questionForView ? '请先回答上方问题…' : undefined}
      />
      <StatusBar
        model={currentModelLabel}
        turns={turns}
        branch={health?.branch || ''}
        modelOptions={displayedModelOptions}
        selectedModelName={currentModelName}
        modelSwitching={modelSwitching}
        onModelChange={switchSessionModel}
      />
      </div>
      {sidePanelMounted && (
        <>
          {!sidePanelCollapsed && (
            <div
              role="separator"
              aria-label="调整右侧栏宽度"
              aria-orientation="vertical"
              tabIndex={0}
              className="ace-resize-handle ace-resize-handle-right"
              onPointerDown={startSidePanelResize}
              onMouseDown={startSidePanelResize}
              onKeyDown={onSidePanelHandleKeyDown}
              title="拖动调整右侧栏宽度"
            />
          )}
          <div
            className="ace-side-panel-shell"
            data-collapsed={sidePanelCollapsed ? 'true' : 'false'}
            style={{ width: sidePanelCollapsed ? 0 : sidePanelWidth }}
          >
            <SidePanel
              sessionRef={ref}
              sessionId={sid}
              cwd={sidePanelCwd}
              messages={sidePanelMessages}
              width={sidePanelWidth}
              collapsed={sidePanelCollapsed}
              onToggleCollapse={onToggleSidePanel}
            />
          </div>
        </>
      )}
    </div>
  );
}
