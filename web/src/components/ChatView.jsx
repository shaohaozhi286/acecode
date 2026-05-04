// 主聊天视图:头部(会话名 + 状态 badge)+ 消息流 +
// InputBar + StatusBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示欢迎屏(空态:logo + 新建按钮 + slash 命令提示)。

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createApi } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { InputBar } from './InputBar.jsx';
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
  for (const key of ['workspaceHash', 'contextId', 'port', 'token', 'cwd']) {
    if (ref[key] != null) next[key] = ref[key];
  }
  return next;
}

export function ChatView({ sessionRef, sessionId, onSessionPromoted, health, onPermissionRequest, onQuestionRequest, questionRequest, onQuestionResolve, showSidePanel = false, sidePanelWidth = 280, onSidePanelResize, sidePanelCollapsed = false, onToggleSidePanel }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const sidRef = useRef(sid);
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  const transcript = useSessionTranscript(ref, {
    live: true,
    onPermissionRequest,
    onQuestionRequest,
    onError: (reason) => toast({
      kind: 'err',
      text: String(reason || '').startsWith('加载会话失败:')
        ? String(reason || '')
        : '错误:' + (reason || ''),
    }),
  });
  const { items, busy, turns, title, status: transcriptStatus, streamingId, applyEvent, setTitle: setTranscriptTitle } = transcript;
  const [history,  setHistory]  = useState([]);
  const scrollRef = useRef(null);
  const inputRef = useRef(null);
  const layoutRef = useRef(null);
  const sidePanelResizeActiveRef = useRef(false);
  const [queueState, setQueueState] = useState(() => createChatInputQueueState());
  const queueStateRef = useRef(queueState);
  const drainRef = useRef(false);
  const visibleQueuedItems = useMemo(() => buildQueuedMessageItems(queueState, sid), [queueState, sid]);
  const renderedItems = useMemo(() => [...items, ...visibleQueuedItems], [items, visibleQueuedItems]);
  const itemsRef = useRef(renderedItems);
  const stickyRafRef = useRef(0);
  const [stickyUserContext, setStickyUserContext] = useState(null);

  useEffect(() => { sidRef.current = sid; }, [sid]);
  useEffect(() => { queueStateRef.current = queueState; }, [queueState]);

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

  // 拉 history(per-cwd)
  useEffect(() => {
    const cwd = ref?.cwd || health?.cwd || '';
    if (!cwd) return;
    api.getHistory(cwd, 200)
      .then((r) => setHistory(Array.isArray(r) ? r : []))
      .catch(() => {});
  }, [health, api, ref?.cwd]);

  // 监听 desktop "新对话" 事件
  useEffect(() => {
    const handler = async () => {
      try {
        const r = ref?.workspaceHash
          ? await api.createWorkspaceSession(ref.workspaceHash, {})
          : await api.createSession({});
        const id = r && (r.session_id || r.id);
        if (id) onSessionPromoted?.({
          ...newSessionRefFrom(ref, id),
          workspaceHash: r.workspace_hash || ref?.workspaceHash,
          cwd: r.cwd || ref?.cwd,
        });
      } catch (e) {
        toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
      }
    };
    window.addEventListener('ace:new-session', handler);
    return () => window.removeEventListener('ace:new-session', handler);
  }, [api, onSessionPromoted, ref]);

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
      const create = ref?.workspaceHash
        ? api.createWorkspaceSession(ref.workspaceHash, { initial_user_message: text, auto_start: true })
        : api.createSession({ initial_user_message: text, auto_start: true });
      create.then((r) => {
        const id = r && (r.session_id || r.id);
        if (id) {
          const next = newSessionRefFrom(ref, id);
          next.workspaceHash = r.workspace_hash || ref?.workspaceHash;
          next.cwd = r.cwd || ref?.cwd;
          next.title = text;
          onSessionPromoted?.(next);
          recordInputHistory(text);
        }
      }).catch((e) => toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') }));
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
  }, [sid, busy, api, ref, onSessionPromoted, recordInputHistory, enqueueInput, applyEvent, setTranscriptTitle]);

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
    return (
      <div className="flex-1 flex flex-col">
        <div className="h-9 px-3 flex items-center bg-surface border-b border-border shrink-0">
          <span className="text-fg-mute text-[12px]">未选择会话</span>
        </div>
        <div className="flex-1 flex flex-col items-center justify-center text-center p-8">
          <img src="/acecode-logo.png" alt="ACECode" width="64" height="64" className="mb-4 select-none" draggable="false" />
          <h2 className="text-lg font-semibold mb-2">开始一个新对话</h2>
          <p className="text-fg-2 text-sm max-w-md leading-relaxed mb-6">
            ACECode 是终端 AI 编码代理 — 让 Agent 帮你读写文件、执行命令、调用工具。
            从下方输入开始,或点 / 查看可用命令。
          </p>
          <button
            type="button"
            onClick={() => window.dispatchEvent(new CustomEvent('ace:new-session'))}
            className="px-4 h-9 rounded-md bg-accent text-white text-sm font-medium hover:opacity-90 transition inline-flex items-center gap-1.5"
          >
            <VsIcon name="add" size={14} mono={false} className="ace-icon-on-accent" />
            <span>新建会话</span>
          </button>
          <div className="mt-8 grid grid-cols-2 gap-3 max-w-lg w-full">
            {[
              { icon: 'edit', title: '编辑代码', desc: '让 Agent 帮你重构、修 bug、加测试' },
              { icon: 'searchSparkle', title: '探索代码库', desc: '问"这个函数在哪里被调用"' },
              { icon: 'run', title: '运行命令', desc: 'bash / npm / git 等,Agent 会逐步确认' },
              { icon: 'lightbulb', title: '使用 Skills', desc: '预定义工作流,从侧边栏开启' },
            ].map((c, i) => (
              <div key={i} className="text-left bg-surface border border-border-soft rounded-lg p-3">
                <VsIcon name={c.icon} size={22} className="mb-1" />
                <div className="text-[13px] font-semibold mb-0.5">{c.title}</div>
                <div className="text-[11px] text-fg-mute leading-relaxed">{c.desc}</div>
              </div>
            ))}
          </div>
        </div>
        {questionForView && (
          <QuestionPicker request={questionForView} onResolve={resolveQuestion} />
        )}
        <InputBar
          ref={inputRef}
          history={history}
          onSubmit={submit}
          disabled={!!questionForView}
          placeholder="输入消息开始新会话…"
        />
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
              data-chat-queued-state={it.queued?.state || undefined}
              data-chat-user-message={it.kind === 'msg' && it.role === 'user' ? 'true' : undefined}
            >
              {it.kind === 'tool' ? (
                <ToolBlock entry={it.tool} />
              ) : (
                <Message
                  role={it.role} content={it.content} ts={it.ts}
                  streaming={it.streaming}
                  messageId={it.messageId}
                  queued={it.queued}
                  onCancelQueued={cancelQueued}
                  onRetryQueued={retryQueued}
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

      <InputBar
        ref={inputRef}
        busy={busy}
        history={history}
        onSubmit={submit}
        onAbort={abort}
        disabled={!!questionForView}
        placeholder={questionForView ? '请先回答上方问题…' : undefined}
      />
      <StatusBar model="—" turns={turns} branch={health?.branch || ''} />
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
