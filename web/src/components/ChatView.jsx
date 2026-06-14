// 主聊天视图:头部(会话名 + 状态 badge)+ 消息流 +
// InputBar + StatusBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示 Codex 风格新任务主页(首条消息提交时才创建 session)。

import { Fragment, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createApi } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { InputBar } from './InputBar.jsx';
import { QueueCardList } from './QueueCardList.jsx';
import { QuestionPicker } from './QuestionPicker.jsx';
import { StickyUserContext } from './StickyUserContext.jsx';
import { SidePanel } from './SidePanel.jsx';
import { PreviewDetailsPanel } from './PreviewDetailsPanel.jsx';
import { StatusBar } from './StatusBar.jsx';
import { ChangeGlassDock } from './ChangeReview.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import {
  aggregateHunksFromMessages,
  changeGroupsSignature,
  collectHunkMessagesFromItems,
  summarizeChangeGroups,
} from '../lib/sessionChanges.js';
import { stableBySignature } from '../lib/changeReviewStability.js';
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
import { findStickyUserContext, sameStickyUserContext, scrollTopForStickySourceRow } from '../lib/stickyUserContext.js';
import { useSessionTranscript } from '../lib/sessionTranscript.js';
import { projectCollapsedTranscriptItems } from '../lib/transcriptProjection.js';
import { usePreference } from '../lib/usePreference.js';
import { maybeNotify } from '../lib/desktopNotify.js';
import { normalizeTokenBudget } from '../lib/tokenBudget.js';
import { pickModelLoad } from '../lib/modelLoad.js';
import {
  modelDisplayLabel,
  isEmptyModelState,
  modelSelectValue,
  normalizeModelOptions,
  normalizeModelState,
  resolveHomeModelName,
  selectedModelName,
  withCreateSessionModel,
} from '../lib/sessionModel.js';
import { normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { ATTACHMENT_HARD_LIMIT_BYTES, normalizeImageFile } from '../lib/imageNormalize.js';
import { PanelToggleIcon, VsIcon } from './Icon.jsx';
import { commandWorkspaceHashForInput } from '../lib/slashCommandWorkspace.js';
import { inputRouteForText, sessionCreateOptionsForText } from '../lib/builtinCommandRouting.js';
import { fileTreeRefreshKeyFromItems } from '../lib/fileTreeRefresh.js';
import { buildAssistantRunDirectives } from '../lib/assistantRunDirectives.js';
import { activityChromeState } from '../lib/assistantAvatarDisplay.js';
import { notifySessionListChanged } from '../lib/sessionListEvents.js';
import { MIN_CHAT_WIDTH, solveSingleContentLayout } from '../lib/singleLayout.js';
import {
  PREVIEW_TAB_TYPES,
  activePreviewTab,
  activatePreviewTab,
  closeOtherPreviewTabs,
  closePreviewTab,
  closePreviewTabsToRight,
  closeVisiblePreviewTabs,
  closeVisiblePreviewTabsConfirmationMessage,
  openFileTab,
  openSessionChangesTab,
  previewScopeKey,
  reorderPreviewTab,
  updateSessionChangesTab,
  visiblePreviewTabs,
} from '../lib/previewTabs.js';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  nextChatTailFollowState,
  shouldAutoFollowChatTail,
} from '../lib/chatScrollFollow.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import {
  createFileContext,
  normalizeComposerContext,
  selectionContextFingerprint,
  selectionContextFromWindowSelection,
  selectionContextLocationKey,
} from '../lib/selectionChatContext.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { todoChecklistPresentation } from '../lib/todoChecklist.js';
import {
  CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
  dismissChangeDockSignature,
  dismissedDockSignatureFor,
  dockDismissalKey,
  validateDockDismissals,
} from '../lib/changeDockDismissal.js';

function isEditableElement(el) {
  if (!el || el === document.body || el === document.documentElement) return false;
  const tag = String(el.tagName || '').toLowerCase();
  return tag === 'input' || tag === 'textarea' || tag === 'select' || !!el.isContentEditable;
}

function fileToBase64(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(reader.error || new Error('read failed'));
    reader.onload = () => {
      const text = String(reader.result || '');
      const comma = text.indexOf(',');
      resolve(comma >= 0 ? text.slice(comma + 1) : text);
    };
    reader.readAsDataURL(file);
  });
}

function normalizeComposerPayload(text, attachments = [], contexts = []) {
  return {
    text: String(text || ''),
    attachments: attachments
      .filter((item) => item && !item.uploading && item.id)
      .map((item) => ({ id: item.id })),
    contexts: contexts.map(normalizeComposerContext).filter(Boolean),
  };
}

function payloadHasExtras(payload) {
  return (Array.isArray(payload?.attachments) && payload.attachments.length > 0) ||
    (Array.isArray(payload?.contexts) && payload.contexts.length > 0);
}

function payloadText(payload) {
  return typeof payload === 'string' ? payload : String(payload?.text || '');
}

function nextSelectionContextId() {
  return `selection-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function messageTextForContext(item) {
  if (item?.kind !== 'msg') return '';
  if (item.role === 'user' && typeof item.metadata?.display_text === 'string' && item.metadata.display_text) {
    return item.metadata.display_text;
  }
  return String(item.content || '');
}

function messageContextAttrs(item) {
  if (item?.kind !== 'msg') return {};
  const messageId = item.messageId || '';
  return {
    'data-desktop-message-id': messageId || undefined,
    'data-desktop-message-role': item.role || undefined,
    'data-desktop-message-text': messageTextForContext(item) || undefined,
    'data-desktop-message-can-fork': messageId ? 'true' : undefined,
  };
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

function formatElapsedSeconds(startedAtMs, nowMs) {
  const start = Number(startedAtMs) || 0;
  if (!start) return '';
  const seconds = Math.max(0, Math.floor((nowMs - start) / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const rest = seconds % 60;
  return `${minutes}m ${rest}s`;
}

function ActivityIndicator({ activity, showAceCodeAvatar = false }) {
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    setNowMs(Date.now());
    const id = window.setInterval(() => setNowMs(Date.now()), 1000);
    return () => window.clearInterval(id);
  }, [activity?.startedAtMs, activity?.phase, activity?.toolCallId, activity?.toolIndex]);

  const isPermWaiting = activity?.phase === 'permission_waiting';
  const label = activity?.label || '正在处理请求';
  const detail = activity?.detail || '';
  const elapsed = formatElapsedSeconds(activity?.startedAtMs, nowMs);
  const chrome = activityChromeState(showAceCodeAvatar);
  return (
    <div className={`flex ${chrome.gapClass} max-w-[85%]`}>
      {chrome.showAvatar ? (
        <div className={clsx(
          'w-6 h-6 rounded-full text-white text-[11px] font-bold flex items-center justify-center mt-[2px]',
          isPermWaiting ? 'bg-warn' : 'bg-ok',
        )}>A</div>
      ) : chrome.showAvatarPlaceholder ? (
        <div className="w-6 shrink-0" aria-hidden="true" />
      ) : (
        null
      )}
      <div className={clsx(
        'rounded-2xl border px-3 py-2 text-[12px] text-fg shadow-sm min-w-[180px]',
        isPermWaiting ? 'border-warn/50 bg-warn/10' : 'border-border bg-surface-hi',
      )}>
        <div className="flex items-center gap-2">
          <span className="font-medium">{label}</span>
          {elapsed && <span className="text-fg-mute tabular-nums">{elapsed}</span>}
        </div>
        {detail && <div className="text-fg-mute mt-0.5 truncate max-w-[420px]">{detail}</div>}
        <div className="flex gap-1 mt-2" aria-hidden="true">
          {[0, 1, 2].map((i) => (
            <span
              key={i}
              className={clsx('w-1.5 h-1.5 rounded-full', isPermWaiting ? 'bg-warn' : 'bg-fg-mute')}
              style={{ animation: `ace-pulse 1.2s ease-in-out ${i * 0.2}s infinite` }}
            />
          ))}
        </div>
      </div>
    </div>
  );
}

const EMPTY_TODO_SUMMARY = Object.freeze({
  total: 0,
  pending: 0,
  in_progress: 0,
  completed: 0,
  cancelled: 0,
});

function TodoChecklist({ todos = [], summary = null, onClear, clearing = false }) {
  const checklist = todoChecklistPresentation(todos, summary);
  if (!checklist.visible) return null;

  return (
    <div className="ace-todo-glass-wrap shrink-0">
      <div className="ace-todo-glass-dock" role="group" aria-label={`待办事项 (${checklist.done}/${checklist.total})`}>
        <div className="ace-todo-glass-content">
          <div className="ace-todo-glass-title">
            待办事项 ({checklist.done}/{checklist.total})
          </div>
          <div className="ace-todo-glass-list">
            {checklist.items.map((item) => {
              return (
                <div
                  key={item.key}
                  className="ace-todo-glass-row"
                  data-todo-status={item.status}
                >
                  <span
                    className={clsx('ace-todo-glass-marker', item.markerClassName)}
                    title={item.markerLabel}
                    aria-label={item.markerLabel}
                  >
                    {item.icon === 'check' && <VsIcon name="ok" size={10} mono={false} />}
                    {item.icon === 'dot' && <span className="h-1.5 w-1.5 rounded-full bg-warn" />}
                    {item.icon === 'dash' && <span className="h-px w-2 bg-fg-mute" />}
                  </span>
                  <span className={clsx('ace-todo-glass-text', item.textClassName)}>
                    {item.content}
                  </span>
                </div>
              );
            })}
          </div>
        </div>
        <button
          type="button"
          className="ace-todo-glass-clear"
          onClick={onClear}
          disabled={!onClear || clearing}
          title="清空待办事项"
          aria-label="清空待办事项"
        >
          <VsIcon name="clearAll" size={16} alt="清空待办事项" />
        </button>
      </div>
    </div>
  );
}

function ActivitySummaryBlock({ item, expanded, onToggle }) {
  return (
    <div className="ml-8 my-1 max-w-[88%]">
      <button
        type="button"
        className="group inline-flex max-w-full items-center gap-2 px-0 py-0.5 text-left text-fg-mute/80 transition-colors"
        onClick={onToggle}
        title={expanded ? '收起详情' : '展开详情'}
        aria-label={expanded ? '收起详情' : '展开详情'}
      >
        <VsIcon name="edit" size={13} className="shrink-0 opacity-80" />
        <span className="text-[12px] font-medium min-w-0 truncate group-hover:text-fg transition-colors">
          {item?.title || '已处理'}
        </span>
        <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={11} className="shrink-0 opacity-80" />
      </button>
      <div className="mt-1 h-px w-full origin-top scale-y-50 bg-fg-mute/20" aria-hidden="true" />
    </div>
  );
}

function CompletionSummaryBlock({ item }) {
  return (
    <div className="max-w-[88%] ml-8 px-1 py-0.5 text-[12px] leading-5 italic text-fg whitespace-pre-wrap break-words">
      {item?.title || '总结：已完成'}
    </div>
  );
}

function TerminationNoticeBlock({ item }) {
  return (
    <div className="max-w-[88%] ml-8 px-1 py-0.5 text-[12px] leading-5 text-danger whitespace-pre-wrap break-words">
      {item?.content || '任务已终止'}
    </div>
  );
}

function normalizeSessionRef(sessionRef, sessionId) {
  if (sessionRef && typeof sessionRef === 'object') return sessionRef;
  if (typeof sessionRef === 'string' && sessionRef) return { sessionId: sessionRef };
  if (sessionId) return { sessionId };
  return null;
}

function sidebarSessionContextTarget(sessionId = '', workspaceHash = '', fallbackTarget = null) {
  if (!sessionId || typeof document === 'undefined') return fallbackTarget;
  const rows = Array.from(document.querySelectorAll('.ace-sidebar-session-row[data-desktop-session-id]'));
  const exact = rows.find((row) => (
    row.getAttribute('data-desktop-session-id') === sessionId
    && (!workspaceHash || row.getAttribute('data-desktop-session-workspace') === workspaceHash)
  ));
  if (exact) return exact;
  return rows.find((row) => row.getAttribute('data-desktop-session-id') === sessionId) || fallbackTarget;
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

export function ChatView({ sessionRef, sessionId, onSessionPromoted, onCommandWorkspaceChange, health, onPermissionRequest, onQuestionRequest, questionRequest, onQuestionResolve, onPermissionModeChanged, showSidePanel = false, sidePanelWidth = 280, onSidePanelResize, previewPanelWidth = 640, onPreviewPanelResize, onPreviewPanelVisibleChange, sidePanelCollapsed = false, onToggleSidePanel, sidePanelMaximized = false, onToggleSidePanelMaximized, showAceCodeAvatar = false }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const sidRef = useRef(sid);
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  // PUB 模型池负载:每 30s 轮询一次缓存快照,失败静默(监控不可用不影响主流程)。
  useEffect(() => {
    let alive = true;
    const fetchPool = () => {
      api.modelPoolStatus()
        .then((r) => { if (alive) setPoolModels(Array.isArray(r?.models) ? r.models : []); })
        .catch(() => {});
    };
    fetchPool();
    const id = window.setInterval(fetchPool, 30000);
    return () => { alive = false; window.clearInterval(id); };
  }, [api]);
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
  const { items, busy, turns, title, status: transcriptStatus, streamingId, tokenUsage, goal, todos, todoSummary, activity, applyEvent, setTitle: setTranscriptTitle } = transcript;
  // 让 fireDesktopNotification 拿到最新 title,无需进入它的 useCallback deps。
  useEffect(() => { transcriptTitleRef.current = title || ''; }, [title]);
  const [history,  setHistory]  = useState([]);
  const [homeWorkspaces, setHomeWorkspaces] = useState([]);
  const [homeWorkspaceHash, setHomeWorkspaceHash] = useState('');
  const [homeSubmitting, setHomeSubmitting] = useState(false);
  const [projectDropdownOpen, setProjectDropdownOpen] = useState(false);
  const [modelOptions, setModelOptions] = useState([]);
  const [modelListLoaded, setModelListLoaded] = useState(false);
  const [homeModelName, setHomeModelName] = useState('');
  const [modelState, setModelState] = useState(null);
  // PUB 模型池负载快照(每 30s 轮询 /api/model-pool-status)。
  const [poolModels, setPoolModels] = useState([]);
  const [pendingModelName, setPendingModelName] = useState('');
  const [modelSwitching, setModelSwitching] = useState(false);
  const [modelRefreshing, setModelRefreshing] = useState(false);
  const [permissionMode, setPermissionMode] = useState('default');
  const [permissionSwitching, setPermissionSwitching] = useState(false);
  const [goalStopping, setGoalStopping] = useState(false);
  const [todoClearing, setTodoClearing] = useState(false);
  const [reviewRequest, setReviewRequest] = useState(0);
  const [previewTabState, setPreviewTabState] = useState({});
  const [dismissedDockSignatures, setDismissedDockSignatures] = usePreference(
    CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
    {},
    validateDockDismissals,
  );
  const changeDockRef = useRef(null);
  const [changeDockBottomPadding, setChangeDockBottomPadding] = useState(0);
  const [expandedActivityKeys, setExpandedActivityKeys] = useState(() => new Set());
  const scrollRef = useRef(null);
  const tailFollowStateRef = useRef(CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  const lastUserTurnKeyRef = useRef('');
  const inputRef = useRef(null);
  const layoutRef = useRef(null);
  const [layoutWidth, setLayoutWidth] = useState(0);
  const sidePanelResizeActiveRef = useRef(false);
  const previewPanelResizeActiveRef = useRef(false);
  const [composerValue, setComposerValue] = useState('');
  const [composerAttachments, setComposerAttachments] = useState([]);
  const [composerContexts, setComposerContexts] = useState([]);
  const [selectionPreview, setSelectionPreview] = useState(null);
  const [composerSubmitting, setComposerSubmitting] = useState(false);
  const [draftReadyKey, setDraftReadyKey] = useState('');
  const draftEditVersionRef = useRef(0);
  const draftSessionKeyRef = useRef('');
  const draftLastSavedRef = useRef({ key: '', text: '' });
  const composerValueRef = useRef('');
  const composerDirtyRef = useRef(false);
  const preserveComposerExtrasOnSessionChangeRef = useRef(false);
  const restoreComposerFocusAfterSubmitRef = useRef(false);
  const selectionPreviewFingerprintRef = useRef('');
  const [queueState, setQueueState] = useState(() => createChatInputQueueState());
  const queueStateRef = useRef(queueState);
  const drainRef = useRef(false);
  // 排队消息从 transcript 中分离出来,只喂给 InputBar 上方的 QueueCardList。
  // transcript 只渲染后端真实落库的消息,避免把"草稿/未发送"和"已发送"混在一起。
  const visibleQueuedItems = useMemo(() => buildQueuedMessageItems(queueState, sid), [queueState, sid]);
  const draftWorkspaceHash = isRealWorkspaceHash(ref?.workspaceHash) ? ref.workspaceHash : '';
  const draftSessionKey = sid ? `${draftWorkspaceHash}:${sid}` : '';
  composerValueRef.current = composerValue;
  const rawItems = items;
  const renderedItems = useMemo(
    () => projectCollapsedTranscriptItems(rawItems, { deferTrailingToolSummary: busy }),
    [rawItems, busy],
  );
  const lastUserTurnKey = useMemo(() => {
    for (let index = rawItems.length - 1; index >= 0; index -= 1) {
      const item = rawItems[index];
      if (item?.kind === 'msg' && item.role === 'user') {
        return String(item.messageId || item.id || index);
      }
    }
    return '';
  }, [rawItems]);
  // 决定每条 assistant 消息的 run 边界;ACECode 头像永久隐藏,空内容(且非
  // streaming)直接隐藏整行。详见 lib/assistantRunDirectives.js。
  const assistantRunDirectives = useMemo(
    () => buildAssistantRunDirectives(renderedItems),
    [renderedItems],
  );
  const hasActiveTool = useMemo(() => renderedItems.some((item) => item.kind === 'tool' && !item.tool?.isDone), [renderedItems]);
  const hasVisibleStreamingAssistant = useMemo(() => (
    streamingId != null
    && renderedItems.some((item) => (
      item.kind === 'msg'
      && item.role === 'assistant'
      && item.id === streamingId
      && typeof item.content === 'string'
      && item.content.trim()
    ))
  ), [renderedItems, streamingId]);
  const itemsRef = useRef(renderedItems);
  const stickyRafRef = useRef(0);
  const [stickyUserContext, setStickyUserContext] = useState(null);

  const selectedHomeWorkspace = useMemo(() => {
    return homeWorkspaces.find((w) => w.hash === homeWorkspaceHash)
      || homeWorkspaces[0]
      || fallbackWorkspaceOption(ref, health);
  }, [health, homeWorkspaceHash, homeWorkspaces, ref]);

  const commandWorkspaceHash = useMemo(() => commandWorkspaceHashForInput({
    activeRef: ref,
    selectedHomeWorkspace,
    hasSession: !!sid,
  }), [ref, selectedHomeWorkspace, sid]);

  useEffect(() => {
    onCommandWorkspaceChange?.(commandWorkspaceHash);
  }, [commandWorkspaceHash, onCommandWorkspaceChange]);

  useEffect(() => { sidRef.current = sid; }, [sid]);
  useEffect(() => { draftSessionKeyRef.current = draftSessionKey; }, [draftSessionKey]);
  useEffect(() => { queueStateRef.current = queueState; }, [queueState]);

  const handleComposerChange = useCallback((next) => {
    draftEditVersionRef.current += 1;
    composerDirtyRef.current = true;
    setComposerValue(next);
  }, []);

  const clearComposerExtras = useCallback(() => {
    setComposerAttachments((items) => {
      for (const item of items) {
        if (item?.preview_url && item.preview_url.startsWith('blob:')) {
          URL.revokeObjectURL(item.preview_url);
        }
      }
      return [];
    });
    setComposerContexts([]);
  }, []);

  const createHomeComposerSession = useCallback(async (text, {
    createOptions = null,
    preserveExtras = false,
    title = '',
  } = {}) => {
    if (homeSubmitting) return null;
    const target = selectedHomeWorkspace || fallbackWorkspaceOption(ref, health);
    const targetHash = target?.hash || '';
    const options = withCreateSessionModel(createOptions || sessionCreateOptionsForText(text), homeModelName);
    const create = isRealWorkspaceHash(targetHash)
      ? api.createWorkspaceSession(targetHash, options)
      : api.createSession(options);
    setHomeSubmitting(true);
    try {
      const r = await create;
      const id = r && (r.session_id || r.id);
      if (!id) throw new Error('missing session id');
      const next = newSessionRefFrom(ref, id);
      if (r.workspace_hash || isRealWorkspaceHash(targetHash)) {
        next.workspaceHash = r.workspace_hash || targetHash;
      }
      next.workspaceName = target?.name || ref?.workspaceName;
      next.cwd = r.cwd || target?.cwd || ref?.cwd;
      next.title = title || text;
      if (preserveExtras) preserveComposerExtrasOnSessionChangeRef.current = true;
      onSessionPromoted?.(next);
      return { id, response: r, target };
    } finally {
      setHomeSubmitting(false);
    }
  }, [api, health, homeModelName, homeSubmitting, onSessionPromoted, ref, selectedHomeWorkspace]);

  const uploadMediaFilesToSession = useCallback((targetSid, files) => {
    for (const [index, file] of Array.from(files || []).entries()) {
      const localId = `local-${Date.now()}-${index}-${Math.random().toString(16).slice(2)}`;
      const kind = String(file.type || '').startsWith('image/') ? 'image' : 'file';
      const previewUrl = kind === 'image' ? URL.createObjectURL(file) : '';
      const localItem = {
        local_id: localId,
        name: file.name || 'attachment',
        kind,
        mime_type: file.type || '',
        size_bytes: file.size || 0,
        preview_url: previewUrl,
        uploading: true,
      };
      setComposerAttachments((items) => [...items, localItem]);
      normalizeImageFile(file)
        .then((normalized) => {
          if (normalized.file.size > ATTACHMENT_HARD_LIMIT_BYTES) {
            throw new Error('附件超过 25MiB，且无法压缩到限制内');
          }
          return normalized;
        })
        .then(({ file: uploadFile }) => fileToBase64(uploadFile)
          .then((dataBase64) => ({ uploadFile, dataBase64 })))
        .then(({ uploadFile, dataBase64 }) => {
          const uploadName = uploadFile.name || file.name || 'attachment';
          const uploadMime = uploadFile.type || file.type || '';
          return api.uploadSessionAttachment(targetSid, {
            name: uploadName,
            mime_type: uploadMime,
            data_base64: dataBase64,
          });
        })
        .then((result) => {
          const attachment = result?.attachment || {};
          setComposerAttachments((items) => items.map((item) => (
            item.local_id === localId
              ? { ...attachment, local_id: localId, preview_url: previewUrl, uploading: false }
              : item
          )));
        })
        .catch((e) => {
          if (previewUrl) URL.revokeObjectURL(previewUrl);
          setComposerAttachments((items) => items.filter((item) => item.local_id !== localId));
          toast({ kind: 'err', text: '附件上传失败:' + (e.message || '') });
        });
    }
  }, [api]);

  const handleMediaFiles = useCallback((files) => {
    const fileList = Array.from(files || []);
    if (fileList.length === 0) return;
    if (sid) {
      uploadMediaFilesToSession(sid, fileList);
      return;
    }
    createHomeComposerSession('', {
      createOptions: { auto_start: false },
      preserveExtras: true,
      title: '附件消息',
    })
      .then((created) => {
        if (created?.id) uploadMediaFilesToSession(created.id, fileList);
      })
      .catch((e) => toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') }));
  }, [createHomeComposerSession, sid, uploadMediaFilesToSession]);

  const removeComposerAttachment = useCallback((key) => {
    setComposerAttachments((items) => {
      const removed = items.find((item) => (item.local_id || item.id || item.name) === key);
      if (removed?.preview_url && removed.preview_url.startsWith('blob:')) {
        URL.revokeObjectURL(removed.preview_url);
      }
      return items.filter((item) => (item.local_id || item.id || item.name) !== key);
    });
  }, []);

  const addBrowserContext = useCallback(() => {
    const item = {
      local_id: `browser-${Date.now()}`,
      type: 'browser',
      label: 'Browser',
      note: 'Context',
    };
    setComposerContexts((items) => items.some((ctx) => ctx.type === 'browser') ? items : [...items, item]);
  }, []);

  const removeComposerContext = useCallback((key) => {
    setComposerContexts((items) => items.filter((item) => (item.local_id || item.id || item.type) !== key));
  }, []);

  const pinSelectionContext = useCallback((context) => {
    const localId = context?.local_id || context?.id || nextSelectionContextId();
    const normalized = normalizeComposerContext({
      ...context,
      local_id: localId,
      id: context?.id || localId,
    });
    if (!normalized) return false;
    const pinned = {
      ...normalized,
      local_id: localId,
      id: normalized.id || localId,
    };
    const locationKey = selectionContextLocationKey(pinned);
    setComposerContexts((items) => {
      if (
        locationKey
        && items.some((item) => selectionContextLocationKey(item) === locationKey)
      ) {
        return items;
      }
      return [...items, pinned];
    });
    selectionPreviewFingerprintRef.current = '';
    setSelectionPreview(null);
    requestAnimationFrame(() => inputRef.current?.focus());
    return true;
  }, []);

  useEffect(() => {
    let raf = 0;
    const updatePreview = () => {
      raf = 0;
      const next = selectionContextFromWindowSelection();
      if (!next) return;
      const fingerprint = selectionContextFingerprint(next);
      if (fingerprint === selectionPreviewFingerprintRef.current) return;
      selectionPreviewFingerprintRef.current = fingerprint;
      setSelectionPreview(next);
    };
    const schedulePreviewUpdate = () => {
      if (raf) cancelAnimationFrame(raf);
      raf = requestAnimationFrame(updatePreview);
    };

    document.addEventListener('selectionchange', schedulePreviewUpdate);
    document.addEventListener('mouseup', schedulePreviewUpdate, true);
    document.addEventListener('keyup', schedulePreviewUpdate, true);
    return () => {
      if (raf) cancelAnimationFrame(raf);
      document.removeEventListener('selectionchange', schedulePreviewUpdate);
      document.removeEventListener('mouseup', schedulePreviewUpdate, true);
      document.removeEventListener('keyup', schedulePreviewUpdate, true);
    };
  }, []);

  const pinnedSelectionLocationKeys = useMemo(() => {
    const keys = new Set();
    for (const context of composerContexts) {
      const key = selectionContextLocationKey(context);
      if (key) keys.add(key);
    }
    return keys;
  }, [composerContexts]);

  const visibleSelectionPreview = useMemo(() => {
    if (!selectionPreview) return null;
    const key = selectionContextLocationKey(selectionPreview);
    return key && pinnedSelectionLocationKeys.has(key) ? null : selectionPreview;
  }, [pinnedSelectionLocationKeys, selectionPreview]);

  const composerInputProps = useMemo(() => ({
    attachments: composerAttachments,
    contexts: composerContexts,
    selectionPreview: visibleSelectionPreview,
    onMediaFiles: handleMediaFiles,
    onRemoveAttachment: removeComposerAttachment,
    onAddBrowserContext: addBrowserContext,
    onRemoveContext: removeComposerContext,
    onPinSelectionPreview: pinSelectionContext,
  }), [
    addBrowserContext,
    composerAttachments,
    composerContexts,
    handleMediaFiles,
    pinSelectionContext,
    removeComposerAttachment,
    removeComposerContext,
    visibleSelectionPreview,
  ]);

  useEffect(() => {
    if (preserveComposerExtrasOnSessionChangeRef.current) {
      preserveComposerExtrasOnSessionChangeRef.current = false;
      return;
    }
    clearComposerExtras();
  }, [clearComposerExtras, draftSessionKey]);

  const persistDraftValue = useCallback((targetSid, targetWorkspaceHash, targetKey, text) => {
    if (!targetSid || !targetKey) return Promise.resolve(null);
    return api.setSessionDraft(targetSid, text, targetWorkspaceHash)
      .then((result) => {
        if (draftSessionKeyRef.current === targetKey) {
          draftLastSavedRef.current = { key: targetKey, text };
          if (composerValueRef.current === text) {
            composerDirtyRef.current = false;
          }
        }
        return result;
      })
      .catch(() => null);
  }, [api]);

  const clearCurrentSessionDraft = useCallback(() => {
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    if (!targetSid || !targetKey) return;
    if (draftSessionKeyRef.current === targetKey) {
      draftEditVersionRef.current += 1;
      setComposerValue('');
    }
    void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, '');
  }, [draftSessionKey, draftWorkspaceHash, persistDraftValue, sid]);

  useEffect(() => {
    let cancelled = false;
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    const editVersionAtLoad = draftEditVersionRef.current;
    setDraftReadyKey('');
    setComposerSubmitting(false);

    if (!targetSid || !targetKey) {
      composerDirtyRef.current = false;
      setComposerValue('');
      draftLastSavedRef.current = { key: '', text: '' };
      return () => { cancelled = true; };
    }

    composerDirtyRef.current = false;
    setComposerValue('');
    api.getSessionDraft(targetSid, targetWorkspaceHash)
      .then((result) => {
        if (cancelled || draftSessionKeyRef.current !== targetKey) return;
        const text = typeof result?.text === 'string' ? result.text : '';
        draftLastSavedRef.current = { key: targetKey, text };
        if (draftEditVersionRef.current === editVersionAtLoad) {
          setComposerValue(text);
        }
        setDraftReadyKey(targetKey);
      })
      .catch(() => {
        if (cancelled || draftSessionKeyRef.current !== targetKey) return;
        draftLastSavedRef.current = { key: targetKey, text: '' };
        setDraftReadyKey(targetKey);
      });

    return () => { cancelled = true; };
  }, [api, draftSessionKey, draftWorkspaceHash, sid]);

  useEffect(() => {
    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    return () => {
      if (!targetSid || !targetKey || !composerDirtyRef.current) return;
      void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, composerValueRef.current);
    };
  }, [draftSessionKey, draftWorkspaceHash, persistDraftValue, sid]);

  useEffect(() => {
    if (!sid || !draftSessionKey || draftReadyKey !== draftSessionKey) return undefined;
    if (draftLastSavedRef.current.key === draftSessionKey &&
        draftLastSavedRef.current.text === composerValue) {
      return undefined;
    }

    const targetSid = sid;
    const targetWorkspaceHash = draftWorkspaceHash;
    const targetKey = draftSessionKey;
    const text = composerValue;
    const timer = setTimeout(() => {
      void persistDraftValue(targetSid, targetWorkspaceHash, targetKey, text);
    }, 350);
    return () => clearTimeout(timer);
  }, [composerValue, draftReadyKey, draftSessionKey, draftWorkspaceHash, persistDraftValue, sid]);

  useEffect(() => {
    let cancelled = false;
    setPendingModelName('');
    setModelSwitching(false);
    setModelRefreshing(false);
    setModelListLoaded(false);

    if (!sid) {
      setModelState(null);
      Promise.allSettled([
        api.listModels(),
        api.getDefaultModel(),
      ]).then(([modelsResult, defaultResult]) => {
        if (cancelled) return;
        const options = modelsResult.status === 'fulfilled'
          ? normalizeModelOptions(modelsResult.value)
          : [];
        setModelOptions(options);
        setModelListLoaded(true);
        const defaultName = defaultResult.status === 'fulfilled'
          ? (defaultResult.value?.name || defaultResult.value?.default_model_name || '')
          : '';
        setHomeModelName((prev) => resolveHomeModelName(options, defaultName, prev));
      });
      return () => { cancelled = true; };
    }

    api.listModels()
      .then((list) => {
        if (!cancelled) {
          setModelOptions(normalizeModelOptions(list));
          setModelListLoaded(true);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setModelOptions([]);
          setModelListLoaded(true);
        }
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
            deleted: ref?.deleted || ref?.model_deleted || ref?.modelDeleted || false,
          }));
        }
      });

    return () => { cancelled = true; };
  }, [api, ref?.context_window, ref?.deleted, ref?.model, ref?.modelDeleted, ref?.model_deleted, ref?.model_name, ref?.model_preset, ref?.provider, ref?.workspaceHash, sid]);

  const refreshSessionModels = useCallback(async () => {
    if (modelRefreshing) return;
    const targetSid = sid;
    const workspaceHash = ref?.workspaceHash || '';
    setModelRefreshing(true);
    try {
      const requests = targetSid
        ? [api.listModels(), api.getSessionModel(targetSid, workspaceHash)]
        : [api.listModels(), api.getDefaultModel()];
      const [modelsResult, stateResult] = await Promise.allSettled(requests);
      if (targetSid && sidRef.current !== targetSid) return;
      const nextOptions = modelsResult.status === 'fulfilled'
        ? normalizeModelOptions(modelsResult.value)
        : modelOptions;
      if (modelsResult.status === 'fulfilled') {
        setModelOptions(nextOptions);
        setModelListLoaded(true);
      }
      if (targetSid && stateResult.status === 'fulfilled') {
        setModelState(normalizeModelState(stateResult.value));
      } else if (!targetSid) {
        const defaultName = stateResult.status === 'fulfilled'
          ? (stateResult.value?.name || stateResult.value?.default_model_name || '')
          : '';
        setHomeModelName((prev) => resolveHomeModelName(nextOptions, defaultName, prev));
      }
      if (modelsResult.status === 'fulfilled') {
        toast({ kind: 'ok', text: '模型列表已刷新' });
      } else {
        toast({ kind: 'err', text: '模型列表刷新失败:' + (modelsResult.reason?.message || '') });
      }
    } finally {
      setModelRefreshing(false);
    }
  }, [api, modelOptions, modelRefreshing, ref?.workspaceHash, sid]);

  useEffect(() => {
    if (!sid) {
      setPermissionMode('default');
      setPermissionSwitching(false);
      return undefined;
    }
    let cancelled = false;
    setPermissionSwitching(false);
    api.getSessionPermissionMode(sid)
      .then((state) => {
        if (!cancelled) setPermissionMode(normalizePermissionMode(state?.mode));
      })
      .catch(() => {
        if (!cancelled) setPermissionMode(normalizePermissionMode(ref?.permission_mode));
      });
    return () => { cancelled = true; };
  }, [api, ref?.permission_mode, sid]);

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

  const setTailFollowFromAction = useCallback((action) => {
    tailFollowStateRef.current = nextChatTailFollowState(tailFollowStateRef.current, action);
  }, []);

  const pauseTailFollowForReview = useCallback(() => {
    if (!busy && transcriptStatus !== 'running') return;
    setTailFollowFromAction({ type: 'review_pause' });
  }, [busy, setTailFollowFromAction, transcriptStatus]);

  const handleMessagesScroll = useCallback(() => {
    const el = scrollRef.current;
    if (el) {
      setTailFollowFromAction({
        type: 'scroll',
        metrics: chatScrollMetrics(el),
      });
    }
    scheduleStickyMeasure();
  }, [scheduleStickyMeasure, setTailFollowFromAction]);

  const jumpToStickyUserSource = useCallback((context) => {
    const el = scrollRef.current;
    const targetId = String(context?.itemId || '');
    if (!el || !targetId) return;

    const targetRow = Array.from(el.querySelectorAll('[data-chat-row="true"]'))
      .find((row) => row.getAttribute('data-chat-item-id') === targetId);
    if (!targetRow) return;

    const containerRect = el.getBoundingClientRect();
    const rowRect = targetRow.getBoundingClientRect();
    el.scrollTo({
      top: scrollTopForStickySourceRow({
        scrollTop: el.scrollTop,
        containerTop: containerRect.top,
        rowTop: rowRect.top,
      }),
      behavior: 'smooth',
    });
    requestAnimationFrame(scheduleStickyMeasure);
    window.setTimeout(scheduleStickyMeasure, 220);
  }, [scheduleStickyMeasure]);

  const focusChatInput = useCallback((force = false) => {
    if (questionRequest) return;
    if (!force && isEditableElement(document.activeElement)) return;
    inputRef.current?.focus();
  }, [questionRequest]);

  const restoreChatInputFocusSoon = useCallback((force = false) => {
    requestAnimationFrame(() => {
      focusChatInput(force);
      requestAnimationFrame(() => focusChatInput(force));
      window.setTimeout(() => focusChatInput(force), 80);
    });
  }, [focusChatInput]);

  useEffect(() => {
    if (composerSubmitting || !restoreComposerFocusAfterSubmitRef.current) return;
    restoreComposerFocusAfterSubmitRef.current = false;
    restoreChatInputFocusSoon(false);
  }, [composerSubmitting, restoreChatInputFocusSoon]);

  useLayoutEffect(() => {
    setTailFollowFromAction({ type: 'session_reset' });
    lastUserTurnKeyRef.current = '';
  }, [sid, setTailFollowFromAction]);

  useEffect(() => {
    if (!sid || !lastUserTurnKey) return;
    const prev = lastUserTurnKeyRef.current;
    if (!prev || prev !== lastUserTurnKey) {
      setTailFollowFromAction({ type: 'new_turn' });
    }
    lastUserTurnKeyRef.current = lastUserTurnKey;
  }, [lastUserTurnKey, setTailFollowFromAction, sid]);

  // 只在用户仍跟随底部时自动滚到底。审查栏会异步测量高度并给消息区补
  // bottom padding,因此跟随模式下仍需在 padding 生效后补几帧滚动。
  useLayoutEffect(() => {
    if (!shouldAutoFollowChatTail(tailFollowStateRef.current)) return undefined;

    let raf1 = 0;
    let raf2 = 0;
    const scrollToBottom = () => {
      const el = scrollRef.current;
      if (el) el.scrollTop = el.scrollHeight;
    };

    scrollToBottom();
    raf1 = requestAnimationFrame(() => {
      scrollToBottom();
      raf2 = requestAnimationFrame(scrollToBottom);
    });

    return () => {
      if (raf1) cancelAnimationFrame(raf1);
      if (raf2) cancelAnimationFrame(raf2);
    };
  }, [renderedItems, busy, changeDockBottomPadding, sid]);

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

  const enqueueInput = useCallback((payload) => {
    if (!sid) return;
    const text = payloadText(payload);
    updateQueueState((prev) => enqueueQueuedInput(prev, { sessionId: sid, payload }));
    if (text.trim()) recordInputHistory(text);
    if (!ref?.title) setTranscriptTitle(text || '附件消息');
  }, [recordInputHistory, ref?.title, setTranscriptTitle, sid, updateQueueState]);

  const cancelQueued = useCallback((queuedId) => {
    updateQueueState((prev) => cancelQueuedInput(prev, queuedId));
  }, [updateQueueState]);

  const retryQueued = useCallback((queuedId) => {
    updateQueueState((prev) => retryQueuedInput(prev, queuedId));
  }, [updateQueueState]);

  const sendInputOrBuiltin = useCallback((targetSid, payload) => {
    const text = payloadText(payload);
    const hasExtras = payloadHasExtras(payload);
    const route = inputRouteForText(text);
    if (!hasExtras && route.kind === 'builtin') {
      return api.executeCommand(targetSid, route.command);
    }
    return api.sendInput(targetSid, payload);
  }, [api]);

  const submit = useCallback((text) => {
    if (composerAttachments.some((item) => item.uploading)) {
      toast({ kind: 'err', text: '附件仍在上传，请稍后发送' });
      return;
    }
    const payload = normalizeComposerPayload(text, composerAttachments, composerContexts);
    const hasExtras = payloadHasExtras(payload);
    if (!payload.text.trim() && !hasExtras) return;
    const route = inputRouteForText(payload.text);
    const isBuiltin = !hasExtras && route.kind === 'builtin';
    if (!isBuiltin || hasExtras) {
      setTailFollowFromAction({ type: 'new_turn' });
    }
    if (!sid) {
      // 自动新建会话。普通消息由 daemon auto_start 接管;builtin 先创建
      // 空会话,再走专门 command endpoint。
      const trimmed = String(payload.text || '').trim();
      if ((!trimmed && !hasExtras) || homeSubmitting) return;
      const createOptions = hasExtras
        ? { auto_start: false }
        : sessionCreateOptionsForText(payload.text);
      createHomeComposerSession(payload.text, { createOptions })
        .then(async (created) => {
          const id = created?.id;
          if (!id) return;
          if (isBuiltin) {
            await api.executeCommand(id, route.command);
          } else if (hasExtras) {
            await sendInputOrBuiltin(id, payload);
          }
          if (payload.text.trim()) recordInputHistory(payload.text);
          if (hasExtras) {
            clearComposerExtras();
            applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
          }
        })
        .catch((e) => toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') }))
        .finally(() => restoreChatInputFocusSoon(false));
      return;
    }
    if (composerSubmitting) return;
    if (busy && !isBuiltin) {
      enqueueInput(payload);
      clearCurrentSessionDraft();
      clearComposerExtras();
      restoreChatInputFocusSoon(false);
      return;
    }
    const targetSid = sid;
    restoreComposerFocusAfterSubmitRef.current = true;
    setComposerSubmitting(true);
    sendInputOrBuiltin(targetSid, payload)
      .then(() => {
        if (payload.text.trim()) recordInputHistory(payload.text);
        if (!ref?.title) setTranscriptTitle(payload.text || composerAttachments[0]?.name || '附件消息');
        clearCurrentSessionDraft();
        clearComposerExtras();
        if (!isBuiltin) {
          applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
        }
      })
      .catch((e) => {
        toast({ kind: 'err', text: '发送失败:' + (e.message || '') });
        applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
      })
      .finally(() => setComposerSubmitting(false));
  }, [sid, busy, api, homeSubmitting, recordInputHistory, enqueueInput, applyEvent, setTranscriptTitle, sendInputOrBuiltin, composerSubmitting, clearCurrentSessionDraft, composerAttachments, composerContexts, clearComposerExtras, createHomeComposerSession, restoreChatInputFocusSoon, setTailFollowFromAction]);

  const drainQueuedInput = useCallback(() => {
    const targetSid = sidRef.current;
    if (!targetSid || busy || drainRef.current) return;
    const queuedItem = nextQueuedInput(queueStateRef.current, targetSid);
    if (!queuedItem) return;

    drainRef.current = true;
    setTailFollowFromAction({ type: 'new_turn' });
    updateQueueState((prev) => markQueuedInputSending(prev, queuedItem.queued.id));
    const queuedPayload = queuedItem.queued?.payload || queuedItem.content;
    sendInputOrBuiltin(targetSid, queuedPayload)
      .then(() => {
        if (sidRef.current === targetSid) {
          if (inputRouteForText(payloadText(queuedPayload)).kind !== 'builtin' ||
              payloadHasExtras(queuedPayload)) {
            applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
          }
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
  }, [applyEvent, busy, sendInputOrBuiltin, setTailFollowFromAction, updateQueueState]);

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

  const abort = useCallback(() => {
    if (!sid) return;
    applyEvent({
      type: 'turn_aborted',
      payload: { reason: '用户已终止本轮任务' },
      timestamp_ms: Date.now(),
    }, { emitEffects: false });
    connection.sendAbort(sid);
  }, [applyEvent, sid]);

  const clearSessionTodos = useCallback(async () => {
    if (!sid || todoClearing) return;
    const previousTodos = todos;
    const previousSummary = todoSummary;
    setTodoClearing(true);
    applyEvent({
      type: 'todo_updated',
      payload: {
        session_id: sid,
        todos: [],
        summary: EMPTY_TODO_SUMMARY,
      },
    }, { emitEffects: false });

    try {
      await api.clearSessionTodos(sid, ref?.workspaceHash || '');
    } catch (e) {
      applyEvent({
        type: 'todo_updated',
        payload: {
          session_id: sid,
          todos: previousTodos,
          summary: previousSummary,
        },
      }, { emitEffects: false });
      toast({ kind: 'err', text: '清空待办事项失败:' + (e?.message || '') });
    } finally {
      setTodoClearing(false);
    }
  }, [api, applyEvent, ref?.workspaceHash, sid, todoClearing, todos, todoSummary]);

  const goalActive = goal?.status === 'active';

  useEffect(() => {
    if (!goalActive) setGoalStopping(false);
  }, [goalActive, sid]);

  const stopCurrentWork = useCallback(() => {
    if (!sid) return;
    const stopControl = getGoalStopControlState({ goal, busy, stopping: goalStopping });
    if (stopControl.action === 'abort') {
      abort();
      return;
    }
    if (stopControl.action !== 'pause_goal' || goalStopping) return;
    setGoalStopping(true);
    api.executeCommand(sid, { name: 'goal', args: 'pause', display_text: '/goal pause' })
      .catch((e) => toast({ kind: 'err', text: '停止 Goal 失败:' + (e?.message || '') }))
      .finally(() => setGoalStopping(false));
  }, [abort, api, busy, goal, goalStopping, sid]);

  const selectHomeModel = useCallback((name) => {
    const nextName = String(name || '');
    if (!nextName || modelRefreshing) return;
    setHomeModelName(nextName);
  }, [modelRefreshing]);

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

  const switchPermissionMode = useCallback(async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permissionMode);
    if (!sid || nextMode === previousMode || permissionSwitching) return;
    setPermissionMode(nextMode);
    setPermissionSwitching(true);
    try {
      const state = await api.setSessionPermissionMode(sid, nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermissionMode(confirmedMode);
      onPermissionModeChanged?.({ sessionId: sid, mode: confirmedMode });
      toast({ kind: 'ok', text: '权限模式已切换为 ' + permissionModeOption(confirmedMode).label });
    } catch (e) {
      setPermissionMode(previousMode);
      toast({ kind: 'err', text: '权限模式切换失败:' + (e?.message || '') });
    } finally {
      setPermissionSwitching(false);
    }
  }, [api, onPermissionModeChanged, permissionMode, permissionSwitching, sid]);

  useLayoutEffect(() => {
    const element = layoutRef.current;
    if (!element) return undefined;
    const measure = () => {
      setLayoutWidth(Math.ceil(element.getBoundingClientRect().width || 0));
    };
    measure();
    if (typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [sid]);

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

  const startPreviewPanelResize = useCallback((event) => {
    if (!sid || !onPreviewPanelResize) return;
    if (event.button != null && event.button !== 0) return;
    if (previewPanelResizeActiveRef.current) return;
    previewPanelResizeActiveRef.current = true;
    event.preventDefault();
    const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = previewPanelWidth;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      onPreviewPanelResize(startWidth + startX - moveEvent.clientX, contentWidth);
    };
    const onStop = () => {
      previewPanelResizeActiveRef.current = false;
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
  }, [onPreviewPanelResize, previewPanelWidth, sid]);

  const onPreviewPanelHandleKeyDown = useCallback((event) => {
    if (!onPreviewPanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
      onPreviewPanelResize(previewPanelWidth + delta, contentWidth);
    }
  }, [onPreviewPanelResize, previewPanelWidth]);

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
      const workspaceHash = r.workspace_hash || ref?.workspaceHash || '';
      const cwd = r.cwd || ref?.cwd || '';
      const now = new Date().toISOString();
      const forkedSession = {
        ...r,
        id: r.session_id,
        active: true,
        status: 'idle',
        attention_state: 'read',
        read_state: 'read',
        workspace_hash: workspaceHash,
        cwd,
        title: r.title,
        created_at: r.created_at || now,
        updated_at: r.updated_at || now,
      };
      onSessionPromoted?.({
        ...newSessionRefFrom(ref, r.session_id),
        title: r.title,
        workspaceHash,
        cwd,
      });
      notifySessionListChanged({
        reason: 'fork',
        sessionId: r.session_id,
        workspaceHash,
        session: forkedSession,
      });
      toast({ kind: 'ok', text: '已分叉到 ' + (r.title || r.session_id) });
    } catch (e) {
      toast({ kind: 'err', text: '分叉失败:' + (e?.message || '') });
    }
  }, [sid, api, ref, onSessionPromoted]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE) return;
      if (target?.type !== 'message' || !target.messageId) return;
      detail.handled = true;
      forkAndSwitch(target.messageId);
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [forkAndSwitch]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT) return;
      const context = detail.selectionContext
        || selectionContextFromWindowSelection({
          target,
          selectedText: detail.selectedText || '',
        });
      detail.handled = true;
      if (!pinSelectionContext(context)) {
        toast({ kind: 'err', text: '没有可引用的选中文本' });
        return;
      }
      toast({ kind: 'ok', text: '已引用到聊天' });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [pinSelectionContext]);

  useEffect(() => {
    const handler = async (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT) return;
      detail.handled = true;
      const filePath = target?.relativePath || target?.absolutePath || '';
      if (!filePath) {
        toast({ kind: 'err', text: '无法获取文件路径' });
        return;
      }
      const cwd = ref?.cwd || health?.cwd || '';
      try {
        const text = await api.readFile(cwd, filePath);
        const context = createFileContext({
          path: target?.absolutePath || filePath,
          kind: 'text',
          text,
        });
        if (!pinSelectionContext(context)) {
          toast({ kind: 'err', text: '文件内容为空' });
          return;
        }
        toast({ kind: 'ok', text: '已引用到聊天' });
      } catch (err) {
        const status = err?.status || 0;
        const body = err?.body || err?.message || '';
        const errorStr = typeof body === 'object' ? (body.error || '') : String(body);
        if (status === 415 && errorStr.includes('binary')) {
          toast({ kind: 'err', text: '无法引用二进制文件' });
        } else if (status === 415 && errorStr.includes('too large')) {
          toast({ kind: 'err', text: '文件过大，无法引用' });
        } else if (status === 404) {
          toast({ kind: 'err', text: '文件不存在' });
        } else {
          toast({ kind: 'err', text: '读取文件失败' });
        }
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [api, ref?.cwd, health?.cwd, pinSelectionContext]);

  const status = useMemo(() => {
    if (!sid) return null;
    return busy || transcriptStatus === 'running' ? 'running' : 'idle';
  }, [sid, busy, transcriptStatus]);
  const sessionWorkspaceHash = ref?.workspaceHash || ref?.workspace_hash || '';
  const sessionPinned = !!(ref?.pinned || ref?.isPinned || ref?.is_pinned);
  const openSessionContextMenu = useCallback((event) => {
    if (!sid || typeof window === 'undefined') return;
    event.preventDefault();
    event.stopPropagation();
    const button = event.currentTarget;
    const rect = button.getBoundingClientRect();
    const target = sidebarSessionContextTarget(sid, sessionWorkspaceHash, button);
    target.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true,
      cancelable: true,
      view: window,
      clientX: Math.min(window.innerWidth - 8, rect.right - 4),
      clientY: Math.min(window.innerHeight - 8, rect.bottom + 4),
    }));
  }, [sessionWorkspaceHash, sid]);

  const modelListEmptyLoaded = modelListLoaded && modelOptions.length === 0;
  const noModelLabel = '未配置模型';
  const currentModelFallback = isEmptyModelState(modelState)
    ? noModelLabel
    : (ref?.model_name || ref?.model_preset || ref?.model || (modelListEmptyLoaded ? noModelLabel : '加载中'));
  const currentModelLabel = modelDisplayLabel(modelState, currentModelFallback);
  const currentModelName = modelSelectValue(modelState, pendingModelName);
  const homeModelFallback = !homeModelName && modelListEmptyLoaded ? noModelLabel : (homeModelName || '加载中');
  const homeModelLabel = modelDisplayLabel(
    modelOptions.find((option) => option.name === homeModelName) || (homeModelName ? { name: homeModelName } : null),
    homeModelFallback,
  );
  const currentContextWindow = Number(modelState?.contextWindow || ref?.context_window || ref?.contextWindow || 0) || 0;
  const tokenBudget = useMemo(() => normalizeTokenBudget({
    usage: tokenUsage,
    contextWindow: currentContextWindow,
  }), [currentContextWindow, tokenUsage]);
  const displayedModelOptions = useMemo(() => {
    const currentName = selectedModelName(modelState);
    if (!currentName || modelOptions.some((m) => m.name === currentName)) return modelOptions;
    const normalized = normalizeModelState(modelState);
    return normalized ? [normalized, ...modelOptions] : modelOptions;
  }, [modelOptions, modelState]);
  // 当前/首屏模型的 PUB 池负载(按 model id 精确匹配 modelPoolName;非 PUB → null)。
  const currentModelLoad = useMemo(
    () => pickModelLoad(poolModels, modelState?.model),
    [poolModels, modelState],
  );
  const homeModelLoad = useMemo(() => {
    const opt = modelOptions.find((option) => option.name === homeModelName);
    return pickModelLoad(poolModels, opt?.model);
  }, [poolModels, modelOptions, homeModelName]);

  // 三处 diff UI 共用同一份数据源:把 items 里 tool 项的 hunks 抽成消息格式。
  // 必须放在 early return 之前,否则空态/有 session 之间 hooks 数量不一致 → React #310。
  const changeMessages = useMemo(() => collectHunkMessagesFromItems(items), [items]);
  const rawChangeGroups = useMemo(() => aggregateHunksFromMessages(changeMessages), [changeMessages]);
  const changeSignature = useMemo(() => changeGroupsSignature(rawChangeGroups), [rawChangeGroups]);
  // 引用稳定化:流式期间每个 WS 帧 items 都换新引用,rawChangeGroups 即使内容
  // 没变也是新数组,会让 DiffPreview 等下游 memo 每帧失效、整棵重建 diff DOM,
  // 用户在变更视图里的滚动位置因此丢失(fix-preview-scroll-during-stream)。
  // 签名一致时复用旧数组引用;ref 在渲染期写入是纯缓存,并发渲染丢弃也无害
  // (签名相同意味着内容相同,最多多算一次)。
  const changeGroupsStableRef = useRef(null);
  changeGroupsStableRef.current = stableBySignature(
    changeGroupsStableRef.current,
    { signature: changeSignature, value: rawChangeGroups },
  );
  const changeGroups = changeGroupsStableRef.current.value;
  const changeSummary = useMemo(() => summarizeChangeGroups(changeGroups), [changeGroups]);
  // changeMessages 同理:groups 由 messages 确定性推导,签名相同即内容相同,
  // 复用同一签名让 SidePanel 的 fallback 聚合 memo 在流式期间不再每帧失效。
  const changeMessagesStableRef = useRef(null);
  changeMessagesStableRef.current = stableBySignature(
    changeMessagesStableRef.current,
    { signature: changeSignature, value: changeMessages },
  );
  const stableChangeMessages = changeMessagesStableRef.current.value;
  const changeDockDismissalKey = useMemo(
    () => dockDismissalKey(ref, sid),
    [ref?.workspaceHash, sid],
  );
  const dismissedDockSignature = useMemo(
    () => dismissedDockSignatureFor(dismissedDockSignatures, changeDockDismissalKey),
    [changeDockDismissalKey, dismissedDockSignatures],
  );
  const fileTreeRefreshKey = useMemo(() => fileTreeRefreshKeyFromItems(items), [items]);
  const showChangeDock = changeSummary.hasChanges
    && !!changeSignature
    && dismissedDockSignature !== changeSignature;

  useLayoutEffect(() => {
    if (!showChangeDock) {
      setChangeDockBottomPadding(0);
      return undefined;
    }

    const measure = () => {
      const height = changeDockRef.current?.getBoundingClientRect().height || 0;
      setChangeDockBottomPadding(height ? Math.ceil(height) + 3 : 0);
    };

    measure();
    const element = changeDockRef.current;
    if (!element || typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    return () => observer.disconnect();
  }, [showChangeDock, changeSummary.fileCount, changeSummary.totalAdditions, changeSummary.totalDeletions]);

  useEffect(() => {
    setExpandedActivityKeys(new Set());
  }, [sid]);

  const toggleActivitySummary = useCallback((key) => {
    pauseTailFollowForReview();
    setExpandedActivityKeys((prev) => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  }, [pauseTailFollowForReview]);

  const openReviewPanel = useCallback(() => {
    if (!showSidePanel || !sid) return;
    if (sidePanelCollapsed) onToggleSidePanel?.();
    setReviewRequest((n) => n + 1);
  }, [onToggleSidePanel, showSidePanel, sid, sidePanelCollapsed]);

  const dismissChangeDock = useCallback(() => {
    if (!changeDockDismissalKey || !changeSignature) return;
    setDismissedDockSignatures((prev) => dismissChangeDockSignature(
      prev,
      changeDockDismissalKey,
      changeSignature,
    ));
  }, [changeDockDismissalKey, changeSignature, setDismissedDockSignatures]);

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

  const sidePanelCwd = ref?.cwd || health?.cwd || '';
  const sidePanelMounted = showSidePanel && !!sid;
  const previewScope = useMemo(
    () => previewScopeKey({ cwd: sidePanelCwd, workspaceHash: ref?.workspaceHash || '' }),
    [ref?.workspaceHash, sidePanelCwd],
  );
  const previewContext = useMemo(
    () => ({ scopeKey: previewScope, sessionId: sid }),
    [previewScope, sid],
  );
  const previewTabs = useMemo(
    () => visiblePreviewTabs(previewTabState, previewContext),
    [previewContext, previewTabState],
  );
  const activePreview = useMemo(
    () => activePreviewTab(previewTabState, previewContext),
    [previewContext, previewTabState],
  );
  const previewTabsOpen = previewTabs.length > 0;
  const previewPanelVisible = previewTabsOpen && !(sidePanelCollapsed && !sidePanelMaximized);
  const previewPanelMaximized = sidePanelMaximized && previewTabsOpen;
  const selectedChangeFile = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFile || ''
    : '';
  const selectedChangeFileRevision = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFileRevision || 0
    : 0;
  const contentLayout = useMemo(() => solveSingleContentLayout({
    contentWidth: layoutWidth,
    sidePanelWidth,
    previewPanelWidth,
    sidePanelVisible: sidePanelMounted,
    sidePanelCollapsed,
    previewPanelVisible,
    previewPanelMaximized,
  }), [
    layoutWidth,
    previewPanelMaximized,
    previewPanelVisible,
    previewPanelWidth,
    sidePanelCollapsed,
    sidePanelMounted,
    sidePanelWidth,
  ]);
  const effectiveChatWidth = layoutWidth > 0 ? contentLayout.chatWidth : 0;
  const effectivePreviewPanelWidth = layoutWidth > 0 ? contentLayout.previewPanelWidth : previewPanelWidth;
  const effectiveSidePanelWidth = layoutWidth > 0 ? contentLayout.sidePanelWidth : sidePanelWidth;

  useEffect(() => {
    if (!sid) return;
    setPreviewTabState((prev) => updateSessionChangesTab(prev, {
      sessionId: sid,
      fileCount: changeSummary.fileCount,
    }));
  }, [changeSummary.fileCount, sid]);

  useEffect(() => {
    onPreviewPanelVisibleChange?.(previewPanelVisible);
  }, [onPreviewPanelVisibleChange, previewPanelVisible]);

  const openFilePreview = useCallback((path) => {
    if (!sid || !previewScope || !path) return;
    setPreviewTabState((prev) => openFileTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      cwd: sidePanelCwd,
      path,
    }));
  }, [previewScope, sid, sidePanelCwd]);

  const openSessionChangePreview = useCallback((filePath) => {
    if (!sid || !filePath) return;
    setPreviewTabState((prev) => openSessionChangesTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      expandedFile: filePath,
      fileCount: changeSummary.fileCount,
    }));
  }, [changeSummary.fileCount, previewScope, sid]);

  const activatePreview = useCallback((tabKey) => {
    setPreviewTabState((prev) => activatePreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
  }, [previewScope, sid]);

  const closePreview = useCallback((tabKey) => {
    const closingLastVisibleTab = previewTabs.length <= 1;
    setPreviewTabState((prev) => closePreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
    if (closingLastVisibleTab && sidePanelMaximized) onToggleSidePanelMaximized?.();
  }, [onToggleSidePanelMaximized, previewScope, previewTabs.length, sid, sidePanelMaximized]);

  const closePreviewPanel = useCallback(() => {
    const confirmMessage = closeVisiblePreviewTabsConfirmationMessage(previewTabs.length);
    if (confirmMessage && !window.confirm(confirmMessage)) return;
    setPreviewTabState((prev) => closeVisiblePreviewTabs(prev, {
      scopeKey: previewScope,
      sessionId: sid,
    }));
    if (sidePanelMaximized) onToggleSidePanelMaximized?.();
  }, [onToggleSidePanelMaximized, previewScope, previewTabs.length, sid, sidePanelMaximized]);

  const closeOtherPreviews = useCallback((tabKey) => {
    setPreviewTabState((prev) => closeOtherPreviewTabs(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
  }, [previewScope, sid]);

  const closePreviewsToRight = useCallback((tabKey) => {
    setPreviewTabState((prev) => closePreviewTabsToRight(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
  }, [previewScope, sid]);

  const reorderPreview = useCallback((sourceKey, targetKey, placement) => {
    setPreviewTabState((prev) => reorderPreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      sourceKey,
      targetKey,
      placement,
    }));
  }, [previewScope, sid]);

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
              {...composerInputProps}
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
        <StatusBar
          model={homeModelLabel}
          turns={0}
          branch={health?.branch || ''}
          modelOptions={modelOptions}
          selectedModelName={homeModelName}
          modelLoad={homeModelLoad}
          modelRefreshing={modelRefreshing}
          onModelChange={selectHomeModel}
          onRefreshModels={refreshSessionModels}
        />
      </div>
    );
  }

  function renderExpandedActivityItems(children, keyPrefix) {
    const list = Array.isArray(children) ? children : [];
    const directives = buildAssistantRunDirectives(list);

    return list.map((child, index) => {
      const key = `${keyPrefix}-${child.id ?? index}`;

      if (child.kind === 'activity_summary') {
        const nestedExpanded = expandedActivityKeys.has(child.id);
        const nestedItems = child.detailItems || child.collapsedItems || [];
        return (
          <div
            key={key}
            className="flex flex-col"
            data-chat-kind={child.kind || ''}
            data-chat-role="activity_summary"
          >
            <ActivitySummaryBlock
              item={child}
              expanded={nestedExpanded}
              onToggle={() => toggleActivitySummary(child.id)}
            />
            {nestedExpanded && (
              <div className="mt-1 ml-4 pl-3 border-l border-border/70 flex flex-col gap-2">
                {renderExpandedActivityItems(nestedItems, `${key}-nested`)}
              </div>
            )}
          </div>
        );
      }

      if (child.kind === 'completion_summary') {
        return (
          <div
            key={key}
            className="flex flex-col"
            data-chat-kind={child.kind || ''}
            data-chat-role="completion_summary"
          >
            <CompletionSummaryBlock item={child} />
          </div>
        );
      }

      if (child.kind === 'termination_notice') {
        return (
          <div
            key={key}
            className="flex flex-col"
            data-chat-kind={child.kind || ''}
            data-chat-role="termination_notice"
          >
            <TerminationNoticeBlock item={child} />
          </div>
        );
      }

      const childDirective = child.kind === 'msg' && child.role === 'assistant'
        ? directives.get(child.id)
        : undefined;
      if (childDirective?.hide) return null;
      const childContinuation = childDirective ? childDirective.showHeader === false : false;
      const childShowFooter = childDirective ? childDirective.showFooter !== false : true;
      return (
        <div
          key={key}
          className="flex flex-col"
          data-chat-kind={child.kind || ''}
          data-chat-role={child.kind === 'msg' ? (child.role || '') : (child.kind || '')}
          {...messageContextAttrs(child)}
        >
          {child.kind === 'tool' ? (
            <ToolBlock entry={child.tool} onReviewToggle={pauseTailFollowForReview} />
          ) : (
            <Message
              role={child.role}
              content={child.content}
              contentParts={child.contentParts}
              ts={child.ts}
              streaming={child.streaming}
              messageId={child.messageId}
              metadata={child.metadata}
              onFork={forkAndSwitch}
              continuation={childContinuation}
              showFooter={childShowFooter}
              showAceCodeAvatar={showAceCodeAvatar}
            />
          )}
        </div>
      );
    });
  }

  const chatColumnStyle = previewPanelMaximized
    ? undefined
    : (layoutWidth > 0
      ? {
          flex: `0 0 ${Math.max(0, effectiveChatWidth)}px`,
          width: Math.max(0, effectiveChatWidth),
          minWidth: Math.min(MIN_CHAT_WIDTH, Math.max(0, effectiveChatWidth)),
        }
      : { minWidth: MIN_CHAT_WIDTH });
  const previewShellStyle = previewPanelMaximized
    ? {
        flex: '1 1 auto',
        width: 'auto',
      }
    : {
        width: Math.max(0, effectivePreviewPanelWidth),
      };
  const sidePanelShellStyle = {
    width: sidePanelCollapsed ? 0 : Math.max(0, effectiveSidePanelWidth),
  };

  return (
    <div ref={layoutRef} className="flex-1 flex min-w-0 ace-chat-layout">
      <div
        className={clsx(
          'flex-1 flex flex-col min-w-0 relative',
          previewPanelMaximized && 'hidden',
        )}
        style={chatColumnStyle}
      >
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
        <div className="flex items-center gap-1 shrink-0">
          {sid && (
            <button
              type="button"
              data-desktop-session-id={sid || undefined}
              data-desktop-session-workspace={sessionWorkspaceHash || undefined}
              data-desktop-session-pinned={sessionPinned ? 'true' : 'false'}
              data-desktop-session-title={title || undefined}
              data-desktop-session-archive="true"
              onClick={openSessionContextMenu}
              className="w-7 h-7 rounded-md bg-surface-hi/0 text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
              title="会话菜单"
              aria-label="会话菜单"
            >
              <VsIcon name="ellipsis" size={15} />
            </button>
          )}
          {sidePanelMounted && sidePanelCollapsed && onToggleSidePanel && (
            <button
              type="button"
              onClick={onToggleSidePanel}
              className="ace-side-panel-expand-fab"
              title="展开右侧面板"
              aria-label="展开右侧面板"
            >
              <PanelToggleIcon side="right" size={15} />
            </button>
          )}
        </div>
      </div>

      <div className="relative flex-1 min-h-0">
        <div
          ref={scrollRef}
          onScroll={handleMessagesScroll}
          className="h-full overflow-y-auto px-3.5 py-3 flex flex-col gap-3"
          style={changeDockBottomPadding > 0 ? { paddingBottom: changeDockBottomPadding } : undefined}
        >
          {renderedItems.map((it) => {
            if (it.kind === 'termination_notice') {
              return (
                <div
                  key={it.id}
                  className="ace-chat-row flex flex-col"
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role="termination_notice"
                >
                  <TerminationNoticeBlock item={it} />
                </div>
              );
            }

            if (it.kind === 'completion_summary') {
              return (
                <div
                  key={it.id}
                  className="ace-chat-row flex flex-col"
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role="completion_summary"
                >
                  <CompletionSummaryBlock item={it} />
                </div>
              );
            }

            if (it.kind === 'activity_summary') {
              const expanded = expandedActivityKeys.has(it.id);
              const detailItems = it.detailItems || it.collapsedItems || [];
              return (
                <Fragment key={it.id}>
                  <div
                    className="ace-chat-row flex flex-col"
                    data-chat-row="true"
                    data-chat-item-id={String(it.id)}
                    data-chat-kind={it.kind || ''}
                    data-chat-role="activity_summary"
                  >
                    <ActivitySummaryBlock
                      item={it}
                      expanded={expanded}
                      onToggle={() => toggleActivitySummary(it.id)}
                    />
                    {expanded && (
                      <div className="mt-1 ml-4 pl-3 border-l border-border/70 flex flex-col gap-2">
                        {renderExpandedActivityItems(detailItems, `activity-hidden-${it.id}`)}
                      </div>
                    )}
                  </div>
                </Fragment>
              );
            }
            const directive = it.kind === 'msg' && it.role === 'assistant'
              ? assistantRunDirectives.get(it.id)
              : undefined;
            // 空内容 + 非 streaming 的 assistant 行整体隐藏。
            if (directive?.hide) {
              return null;
            }
            const continuation = directive ? directive.showHeader === false : false;
            const showFooter = directive ? directive.showFooter !== false : true;
            return (
              <Fragment key={it.id}>
                <div
                  className="ace-chat-row flex flex-col"
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role={it.kind === 'msg' ? (it.role || '') : (it.kind || '')}
                  data-chat-user-message={it.kind === 'msg' && it.role === 'user' ? 'true' : undefined}
                  data-chat-assistant-continuation={continuation ? 'true' : undefined}
                  {...messageContextAttrs(it)}
                >
                  {it.kind === 'tool' ? (
                    <ToolBlock entry={it.tool} onReviewToggle={pauseTailFollowForReview} />
                  ) : (
                    <Message
                      role={it.role} content={it.content} ts={it.ts}
                      contentParts={it.contentParts}
                      streaming={it.streaming}
                      messageId={it.messageId}
                      metadata={it.metadata}
                      onFork={forkAndSwitch}
                      continuation={continuation}
                      showFooter={showFooter}
                      showAceCodeAvatar={showAceCodeAvatar}
                    />
                  )}
                </div>
              </Fragment>
            );
          })}
          {busy && !hasVisibleStreamingAssistant && (!hasActiveTool || activity?.phase === 'permission_waiting') && (
            <ActivityIndicator activity={activity} showAceCodeAvatar={showAceCodeAvatar} />
          )}
        </div>
        <StickyUserContext context={stickyUserContext} onJumpToSource={jumpToStickyUserSource} />
        {showChangeDock && (
          <ChangeGlassDock
            dockRef={changeDockRef}
            scrollRef={scrollRef}
            summary={changeSummary}
            onReview={openReviewPanel}
            onDismiss={dismissChangeDock}
          />
        )}
      </div>

      {questionForView && (
        <QuestionPicker request={questionForView} onResolve={resolveQuestion} />
      )}

      <QueueCardList
        items={visibleQueuedItems}
        onCancel={cancelQueued}
        onRetry={retryQueued}
      />
      <TodoChecklist
        todos={todos}
        summary={todoSummary}
        onClear={clearSessionTodos}
        clearing={todoClearing}
      />
      <InputBar
        ref={inputRef}
        busy={busy}
        goal={goal}
        goalStopping={goalStopping}
        history={history}
        value={composerValue}
        onChange={handleComposerChange}
        onSubmit={submit}
        onAbort={stopCurrentWork}
        {...composerInputProps}
        disabled={!!questionForView || composerSubmitting}
        placeholder={questionForView ? '请先回答上方问题…' : undefined}
      />
      <StatusBar
        model={currentModelLabel}
        turns={turns}
        branch={health?.branch || ''}
        modelOptions={displayedModelOptions}
        selectedModelName={currentModelName}
        modelLoad={currentModelLoad}
        modelSwitching={modelSwitching}
        modelRefreshing={modelRefreshing}
        onModelChange={switchSessionModel}
        onRefreshModels={refreshSessionModels}
        tokenBudget={tokenBudget}
        goal={goal}
        permissionMode={permissionMode}
        permissionSwitching={permissionSwitching}
        onPermissionModeChange={switchPermissionMode}
      />
      </div>
      {previewPanelVisible && !previewPanelMaximized && (
        <div
          role="separator"
          aria-label="调整预览面板宽度"
          aria-orientation="vertical"
          tabIndex={0}
          className="ace-resize-handle ace-resize-handle-preview"
          onPointerDown={startPreviewPanelResize}
          onMouseDown={startPreviewPanelResize}
          onKeyDown={onPreviewPanelHandleKeyDown}
          title="拖动调整预览面板宽度"
        />
      )}
      {previewPanelVisible && (
        <div
          className="ace-preview-details-shell"
          data-maximized={previewPanelMaximized ? 'true' : 'false'}
          style={previewShellStyle}
        >
          <PreviewDetailsPanel
            api={api}
            cwd={sidePanelCwd}
            tabs={previewTabs}
            activeTab={activePreview}
            changeGroups={changeGroups}
            changeSummary={changeSummary}
            maximized={previewPanelMaximized}
            onActivateTab={activatePreview}
            onCloseTab={closePreview}
            onCloseOthers={closeOtherPreviews}
            onCloseToRight={closePreviewsToRight}
            onCloseAll={closePreviewPanel}
            onReorderTab={reorderPreview}
            onToggleMaximize={onToggleSidePanelMaximized}
            onSelectChangeFile={openSessionChangePreview}
          />
        </div>
      )}
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
            data-maximized="false"
            style={sidePanelShellStyle}
          >
            <SidePanel
              sessionRef={ref}
              sessionId={sid}
              cwd={sidePanelCwd}
              messages={stableChangeMessages}
              changeGroups={changeGroups}
              changeSummary={changeSummary}
              fileRefreshKey={fileTreeRefreshKey}
              reviewRequest={reviewRequest}
              width={sidePanelWidth}
              collapsed={sidePanelCollapsed}
              onToggleCollapse={onToggleSidePanel}
              onOpenFilePreview={openFilePreview}
              onOpenSessionChangePreview={openSessionChangePreview}
              selectedChangeFile={selectedChangeFile}
              selectedChangeFileRevision={selectedChangeFileRevision}
            />
          </div>
        </>
      )}
    </div>
  );
}
