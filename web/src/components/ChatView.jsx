// 主聊天视图:头部(会话名 + 状态 badge)+ 消息流 +
// 内嵌会话控制的 InputBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示 Codex 风格新任务主页(首条消息提交时才创建 session)。

import {
  Component,
  Fragment,
  Suspense,
  lazy,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { flushSync } from 'react-dom';
import { createApi } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { tr } from '../i18n/index.js';
import { renderMarkdown } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { InputBar } from './InputBar.jsx';
import { QueueCardList } from './QueueCardList.jsx';
import { SideQuestionCard } from './SideQuestionCard.jsx';
import { GitSessionPill } from './GitSessionPill.jsx';
import { LspIndicator } from './LspIndicator.jsx';
import { QuestionPicker } from './QuestionPicker.jsx';
import { PermissionCard } from './PermissionCard.jsx';
import { StickyUserContext } from './StickyUserContext.jsx';
import { SidePanel } from './SidePanel.jsx';
import { SubagentPanel } from './SubagentPanel.jsx';
import { SubagentGroupBlock } from './SubagentGroupBlock.jsx';
import { PreviewDetailsPanel } from './PreviewDetailsPanel.jsx';
import { Modal } from './Modal.jsx';
import { CreateProjectModal } from './CreateProjectModal.jsx';
import { ChangeGlassDock } from './ChangeReview.jsx';
import { TurnFileList } from './TurnFileList.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import {
  aggregateHunksFromMessages,
  changeGroupsSignature,
  collectHunkMessagesFromItems,
  collectTurnChangeSetsFromItems,
  latestTurnSuccessfulChangedFiles,
  summarizeChangeGroups,
} from '../lib/sessionChanges.js';
import { stableBySignature } from '../lib/changeReviewStability.js';
import {
  acceptedQueuedInputEvent,
  beginQueuedGuidance,
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  finishQueuedGuidance,
  hasSendingQueuedInput,
  markQueuedGuidanceAccepted,
  markQueuedInputCompleted,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  queuedInputRequestPayload,
  restoreUncommittedGuidanceForSession,
  retryQueuedInput,
} from '../lib/chatInputQueue.js';
import { findStickyUserContext, sameStickyUserContext, scrollTopForStickySourceRow } from '../lib/stickyUserContext.js';
import { loadTranscriptHistory, useSessionTranscript } from '../lib/sessionTranscript.js';
import { projectCollapsedTranscriptItems } from '../lib/transcriptProjection.js';
import { buildComposerHistory } from '../lib/inputHistoryNavigation.js';
import {
  completedTurnSelfHealEnabled,
  createCompletedTurnSelfHealScheduler,
  reconcileLatestCompletedTurn,
} from '../lib/transcriptSelfHeal.js';
import { usePreference } from '../lib/usePreference.js';
import { pickExistingWorkspace } from '../lib/workspacePicker.js';
import {
  DEFAULT_HOME_WORKSPACE_SELECTION,
  HOME_WORKSPACE_SELECTION_STORAGE_KEY,
  homeWorkspaceOptionForHash,
  noHomeWorkspaceOption,
  readDesktopHomeWorkspaceHash,
  resolveHomeWorkspaceHash,
  validateHomeWorkspaceSelection,
  writeDesktopHomeWorkspaceHash,
} from '../lib/homeWorkspaceSelection.js';
import { bindDesktopComposerAutoFocus } from '../lib/composerCaretRestore.js';
import { useSubagentTasks } from '../lib/useSubagentTasks.js';
import { taskDisplayTitle } from '../lib/subagentTasks.js';
import {
  CONVERSATION_ACTIVITY_KIND,
  selectConversationActivity,
} from '../lib/conversationActivity.js';
import { normalizeTokenBudget } from '../lib/tokenBudget.js';
import { pickModelLoad } from '../lib/modelLoad.js';
import { normalizeExperts } from '../lib/expertComponents.js';
import {
  modelDisplayLabel,
  isEmptyModelState,
  modelSelectValue,
  normalizeModelOptions,
  normalizeModelState,
  resolveHomeModelName,
  selectedModelName,
  withCreateSessionPreferences,
} from '../lib/sessionModel.js';
import { normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { ATTACHMENT_HARD_LIMIT_BYTES, normalizeImageFile } from '../lib/imageNormalize.js';
import { PanelToggleIcon, VsIcon } from './Icon.jsx';
import { commandWorkspaceHashForInput } from '../lib/slashCommandWorkspace.js';
import { consoleCwdForContext } from '../lib/consoleDock.js';
import { inputRouteForText, sessionCreateOptionsForText } from '../lib/builtinCommandRouting.js';
import { buildCurrentSessionDesktopFeedbackPayload } from '../lib/desktopFeedback.js';
import { buildWorktreeIntent } from '../lib/gitSessionPill.js';
import { fileTreeRefreshKeyFromItems } from '../lib/fileTreeRefresh.js';
import { buildAssistantRunDirectives } from '../lib/assistantRunDirectives.js';
import { activityChromeState } from '../lib/assistantAvatarDisplay.js';
import { notifySessionListChanged } from '../lib/sessionListEvents.js';
import { MIN_CHAT_WIDTH, solveSingleContentLayout } from '../lib/singleLayout.js';
import { completionSummaryMarkdown } from '../lib/taskCompleteSummary.js';
import {
  activeConversationTurnIndex as resolveActiveConversationTurnIndex,
  activatedConversationTurnIndex as resolveActivatedConversationTurnIndex,
  buildConversationTurnPreviews,
  shouldShowConversationTurnScrubber,
} from '../lib/conversationTurnScrubber.js';
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
  openGitChangesTab,
  openSessionChangesTab,
  previewFileLocation,
  previewScopeKey,
  refreshPreviewTab,
  reorderPreviewTab,
  updateGitChangesTab,
  updateSessionChangesTab,
  visiblePreviewTabs,
} from '../lib/previewTabs.js';
import { nextAutoPreviewRefresh } from '../lib/previewRefresh.js';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  nextChatTailFollowState,
  observeChatTailContent,
  shouldAutoFollowChatTail,
} from '../lib/chatScrollFollow.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { normalizeReferencePath } from '../lib/pathReference.js';
import {
  createFileContext,
  normalizeComposerContext,
  selectionContextFingerprint,
  selectionContextFromWindowSelection,
  selectionContextLocationKey,
} from '../lib/selectionChatContext.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import {
  CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
  dismissChangeDockSignature,
  dismissedDockSignatureFor,
  dockDismissalKey,
  isTodoDockSuppressed,
  todoDockSignature,
  validateDockDismissals,
} from '../lib/changeDockDismissal.js';

const LazyConversationTurnScrubber = lazy(
  () => import('./ConversationTurnScrubber.jsx'),
);

class ConversationTurnScrubberBoundary extends Component {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  render() {
    return this.state.failed ? null : this.props.children;
  }
}

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

function finiteMessageOrdinal(value) {
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : null;
}

function searchJumpOrdinalFromRef(ref) {
  const match = ref?.searchMatch || ref?.search_match || null;
  return finiteMessageOrdinal(
    match?.messageOrdinal ??
    match?.message_ordinal ??
    ref?.messageOrdinal ??
    ref?.message_ordinal,
  );
}

function scrollTopForCenteredRow(container, row) {
  if (!container || !row) return 0;
  const containerRect = container.getBoundingClientRect();
  const rowRect = row.getBoundingClientRect();
  const target = container.scrollTop +
    rowRect.top - containerRect.top -
    Math.max(0, (container.clientHeight - rowRect.height) / 2);
  return Math.max(0, target);
}

function searchJumpTargetRow(container, ordinal) {
  if (!container || ordinal === null) return null;
  return Array.from(container.querySelectorAll('[data-chat-row="true"][data-chat-user-message="true"]'))
    .find((row) => row.getAttribute('data-chat-message-ordinal') === String(ordinal)) || null;
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

function chatRowClassName(item, extra = '') {
  const role = item?.kind === 'msg' ? (item.role || '') : (item?.kind || '');
  return clsx(
    'ace-chat-row flex flex-col',
    (item?.kind === 'tool' || role === 'system') && 'ace-chat-row-assistant-gutter',
    extra,
  );
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

  const kind = activity?.kind || CONVERSATION_ACTIVITY_KIND.FOREGROUND;
  const isWaiting = activity?.needsAction === true
    || kind === CONVERSATION_ACTIVITY_KIND.RECOVERY;
  const label = activity?.label || '正在处理请求';
  const detail = [
    activity?.detail || '',
    kind !== CONVERSATION_ACTIVITY_KIND.BACKGROUND
      && activity?.backgroundCount > 0
      ? activity.backgroundLabel
      : '',
  ].filter(Boolean).join(' · ');
  const elapsed = formatElapsedSeconds(activity?.startedAtMs, nowMs);
  const chrome = activityChromeState(showAceCodeAvatar);
  return (
    <div
      className={`flex ${chrome.gapClass} max-w-[85%]`}
      data-conversation-activity-bubble="true"
      data-activity-kind={kind}
    >
      {chrome.showAvatar ? (
        <div className={clsx(
          'w-6 h-6 rounded-full text-white text-[11px] font-bold flex items-center justify-center mt-[2px]',
          isWaiting ? 'bg-warn' : 'bg-ok',
        )}>A</div>
      ) : chrome.showAvatarPlaceholder ? (
        <div className="w-6 shrink-0" aria-hidden="true" />
      ) : (
        null
      )}
      <div className={clsx(
        'rounded-2xl border px-3 py-2 text-[12px] text-fg shadow-sm min-w-[180px]',
        isWaiting ? 'border-warn/50 bg-warn/10' : 'border-border bg-surface-hi',
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
              className={clsx('w-1.5 h-1.5 rounded-full', isWaiting ? 'bg-warn' : 'bg-fg-mute')}
              style={{ animation: `ace-pulse 1.2s ease-in-out ${i * 0.2}s infinite` }}
            />
          ))}
        </div>
      </div>
    </div>
  );
}

function ActivitySummaryBlock({ item, expanded, onToggle }) {
  return (
    <div className="my-1 max-w-[88%]">
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

function completionSummaryText(item) {
  return completionSummaryMarkdown(item, '已完成');
}

function CompletionSummaryBlock({ item }) {
  const summaryText = completionSummaryText(item);
  const html = useMemo(() => ({ __html: renderMarkdown(summaryText) }), [summaryText]);
  const handleMarkdownClick = useCallback(async (event) => {
    const text = codeTextFromCopyButtonTarget(event.target);
    if (text == null) return;
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  }, []);

  return (
    <div
      className="max-w-[88%] px-1 py-0.5 text-fg break-words"
      title={item?.title || `总结：${summaryText}`}
    >
      <div className="text-[12px] font-semibold text-fg-mute mb-0.5">总结</div>
      <div
        className="ace-md ace-completion-summary-md text-[13px] leading-[1.6]"
        onClick={handleMarkdownClick}
        dangerouslySetInnerHTML={html}
      />
    </div>
  );
}

function TerminationNoticeBlock({ item }) {
  return (
    <div className="max-w-[88%] px-1 py-0.5 text-[12px] leading-5 text-danger whitespace-pre-wrap break-words">
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

export function ChatView({ sessionRef, sessionId, modelProfileRevision = 0, onSessionPromoted, onHomeWorkspaceChange, onCommandWorkspaceChange, onConsoleCwdChange, onFindInConversation, onOpenModelSettings, health, autoFocusOnDesktopWindowFocus = false, onPermissionRequest, onQuestionRequest, permissionRequests = [], onPermissionDecision, questionRequest, onQuestionResolve, onPermissionModeChanged, onSubagentTasksChange, showSidePanel = false, sidePanelWidth = 280, onSidePanelResize, previewPanelWidth = 640, previewPanelAutoFit = false, onPreviewPanelResize, onPreviewPanelVisibleChange, sidePanelCollapsed = false, sidePanelListCollapsed = false, onToggleSidePanel, onToggleSidePanelList, onRevealSidePanelList, sidePanelMaximized = false, onToggleSidePanelMaximized, showAceCodeAvatar = false }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const readOnlyExternalSession = !!(
    ref?.readOnly || ref?.read_only
  );
  const sidRef = useRef(sid);
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  // 模型池负载:每 30s 轮询一次缓存快照,失败静默(监控不可用不影响主流程)。
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
  const completedTurnSelfHealScheduleRef = useRef(null);

  const transcript = useSessionTranscript(ref, {
    live: !readOnlyExternalSession,
    refreshIntervalMs: readOnlyExternalSession ? 1500 : 0,
    onPermissionRequest,
    onQuestionRequest: (payload) => {
      onQuestionRequest?.(payload);
    },
    onTurnCompleted: () => {
      completedTurnSelfHealScheduleRef.current?.schedule();
    },
    onError: (reason) => toast({
      kind: 'err',
      text: String(reason || '').startsWith('加载会话失败:')
        ? String(reason || '')
        : '错误:' + (reason || ''),
    }),
  });
  const {
    items,
    busy,
    activeTurnId,
    turns,
    title,
    status: transcriptStatus,
    loadState: transcriptLoadState,
    streamingId,
    tokenUsage,
    goal,
    todos,
    todoSummary,
    activity,
    applyEvent,
    setTitle: setTranscriptTitle,
  } = transcript;

  // 后台任务(spawn_subagent 子会话):数据 hook 常驻(运行中任务保持 WS
  // 订阅,权限/问题请求才能冒泡到主会话 UI),面板本身按需打开。
  const subagentTasks = useSubagentTasks(sid);
  const [subagentPanelOpen, setSubagentPanelOpen] = useState(false);
  // 聊天流「调用了 N 个智能体」分组点某个智能体 → 打开面板并定位其 transcript。
  // focus.n 单调递增,让同一 sessionId 的重复点击也能触发 SubagentPanel 内 effect。
  const [subagentFocus, setSubagentFocus] = useState(null);
  useEffect(() => { setSubagentPanelOpen(false); setSubagentFocus(null); }, [sid]);
  const openSubagentTranscript = useCallback((sessionId) => {
    if (!sessionId) return;
    setSubagentPanelOpen(true);
    setSubagentFocus((prev) => ({ id: sessionId, n: (prev?.n || 0) + 1 }));
  }, []);
  // sessionId → 任务,给分组卡片解析实时标题/运行态。
  const subagentTasksById = useMemo(
    () => new Map(subagentTasks.tasks.map((t) => [t.id, t])),
    [subagentTasks.tasks]);
  // 上报给 App:子任务 id → 标题映射,用于 question 请求的可见性放宽与
  // 权限/问题弹窗的「来自后台任务」来源标记。
  const onSubagentTasksChangeRef = useRef(onSubagentTasksChange);
  useEffect(() => { onSubagentTasksChangeRef.current = onSubagentTasksChange; }, [onSubagentTasksChange]);
  useEffect(() => {
    onSubagentTasksChangeRef.current?.({
      parentId: sid,
      titles: Object.fromEntries(
        subagentTasks.tasks.map((t) => [t.id, taskDisplayTitle(t)])),
    });
  }, [sid, subagentTasks.tasks]);
  const selfHealEnabled = completedTurnSelfHealEnabled(health);
  const selfHealRuntimeRef = useRef({
    sid: '',
    api: null,
    enabled: false,
    isLive: false,
  });
  const selfHealTranscriptRef = useRef({
    getState: null,
    updateState: null,
  });
  const selfHealSchedulerRef = useRef(null);
  if (!selfHealSchedulerRef.current) {
    selfHealSchedulerRef.current = createCompletedTurnSelfHealScheduler({
      getEnabled: () => selfHealRuntimeRef.current.enabled === true,
      getSessionId: () => selfHealRuntimeRef.current.sid,
      getIsLive: () => selfHealRuntimeRef.current.isLive === true,
      getState: () => selfHealTranscriptRef.current.getState?.() || null,
      isVisible: () => (
        typeof document === 'undefined' || document.visibilityState !== 'hidden'
      ),
      fetchCanonicalHistory: (sessionId) => (
        selfHealRuntimeRef.current.api?.getMessages(sessionId, 0)
      ),
      applyCanonicalHistory: (data, snapshot) => {
        const current = selfHealTranscriptRef.current.getState?.();
        if (!current) return { replaced: false, reason: 'missing_state', state: current };
        const canonical = loadTranscriptHistory(current, data || {}).state;
        const result = reconcileLatestCompletedTurn(current, canonical, snapshot);
        if (result.replaced) {
          selfHealTranscriptRef.current.updateState?.(result.state);
        }
        return result;
      },
    });
  }
  completedTurnSelfHealScheduleRef.current = selfHealSchedulerRef.current;
  useEffect(() => {
    selfHealRuntimeRef.current = {
      sid,
      api,
      enabled: selfHealEnabled,
      isLive: transcript.isLive === true,
    };
  }, [api, selfHealEnabled, sid, transcript.isLive]);
  useEffect(() => {
    selfHealTranscriptRef.current = {
      getState: transcript.getState,
      updateState: transcript.updateState,
    };
  }, [transcript.getState, transcript.updateState]);
  useEffect(() => () => {
    selfHealSchedulerRef.current?.cancel();
  }, [sid]);
  const [history,  setHistory]  = useState([]);
  const [homeWorkspaces, setHomeWorkspaces] = useState([]);
  const [homeWorkspaceHash, setHomeWorkspaceHash] = useState('');
  const [homeWorkspaceSelection, setHomeWorkspaceSelection] = usePreference(
    HOME_WORKSPACE_SELECTION_STORAGE_KEY,
    DEFAULT_HOME_WORKSPACE_SELECTION,
    validateHomeWorkspaceSelection,
  );
  const [homeSubmitting, setHomeSubmitting] = useState(false);
  const [projectDropdownOpen, setProjectDropdownOpen] = useState(false);
  const [createProjectOpen, setCreateProjectOpen] = useState(false);
  const [modelOptions, setModelOptions] = useState([]);
  const [modelListLoaded, setModelListLoaded] = useState(false);
  const [homeModelName, setHomeModelName] = useState('');
  const [experts, setExperts] = useState([]);
  const [homeExpertId, setHomeExpertId] = useState(() => String(
    ref?.expertId || ref?.expert_id || ref?.expert?.id || '',
  ));
  const [modelState, setModelState] = useState(null);
  // 模型池负载快照(每 30s 轮询 /api/model-pool-status)。
  const [poolModels, setPoolModels] = useState([]);
  const [pendingModelName, setPendingModelName] = useState('');
  const [modelSwitching, setModelSwitching] = useState(false);
  const [modelRefreshing, setModelRefreshing] = useState(false);
  const [permissionMode, setPermissionMode] = useState('default');
  const [permissionSwitching, setPermissionSwitching] = useState(false);
  const [goalStopping, setGoalStopping] = useState(false);
  const [reviewRequest, setReviewRequest] = useState(0);
  const [previewTabState, setPreviewTabState] = useState({});
  const [previewCloseConfirm, setPreviewCloseConfirm] = useState(null);
  const [dismissedDockSignatures, setDismissedDockSignatures] = usePreference(
    CHANGE_DOCK_DISMISSALS_STORAGE_KEY,
    {},
    validateDockDismissals,
  );
  // 下一轮对话提交时整体收起玻璃 dock:变更走 dismissChangeDock(持久化
  // 签名),todo 记会话内存级快照抑制 {sessionKey, signature}。真正的收起
  // 动作经 ref 中转 —— submit 的 useCallback 定义在 changeSignature /
  // todoSignature 之前(TDZ 不能进 deps),渲染期写 ref 是纯缓存,与
  // changeGroupsStableRef 同一模式。
  const [todoDockSuppression, setTodoDockSuppression] = useState(null);
  const dockAutoDismissRef = useRef(() => {});
  const changeDockRef = useRef(null);
  const [changeDockBottomPadding, setChangeDockBottomPadding] = useState(0);
  const [expandedActivityKeys, setExpandedActivityKeys] = useState(() => new Set());
  const scrollRef = useRef(null);
  const transcriptContentRef = useRef(null);
  const tailFollowStateRef = useRef(CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  const tailFollowScrollRafRef = useRef({ first: 0, second: 0 });
  // 区分"用户滚动"与"流式渲染引起的 scrollTop 位移"用的上下文:上一次
  // scroll 事件的指标 + 指针是否按住(拖动滚动条/拖选期间的滚动算用户意图)。
  const scrollActivityRef = useRef({ prev: null, pointerActive: false });
  const lastUserTurnKeyRef = useRef('');
  const previewAutoRefreshRef = useRef({ sid: '', busy: false, completedTurnKey: '' });
  const inputRef = useRef(null);
  const layoutRef = useRef(null);
  const [layoutWidth, setLayoutWidth] = useState(0);
  const sidePanelResizeActiveRef = useRef(false);
  const previewPanelResizeActiveRef = useRef(false);
  const renderedPreviewPanelWidthRef = useRef(previewPanelWidth);
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
  const [sideQuestion, setSideQuestion] = useState(null);
  const sideQuestionInFlightRef = useRef(false);
  const sideQuestionEpochRef = useRef(0);
  // GitSessionPill 的待生效意图(worktree 勾选 + 基线分支)。ref 不入 dep:
  // 只在发送首条消息那一刻读取,不驱动渲染。
  const gitPillIntentRef = useRef({ worktreeChecked: false, selectedBase: '' });
  const handleGitPillIntentChange = useCallback((intent) => {
    gitPillIntentRef.current = intent || { worktreeChecked: false, selectedBase: '' };
  }, []);
  // 本会话经首条消息创建的 worktree(客户端态,刷新后回落为分支展示)。
  const [localWorktree, setLocalWorktree] = useState(null); // {sid, name}
  const drainRef = useRef(false);
  // 排队消息从 transcript 中分离出来,只喂给 InputBar 上方的 QueueCardList。
  // transcript 只渲染后端真实落库的消息,避免把"草稿/未发送"和"已发送"混在一起。
  const visibleQueuedItems = useMemo(() => buildQueuedMessageItems(queueState, sid), [queueState, sid]);
  const draftWorkspaceHash = isRealWorkspaceHash(ref?.workspaceHash) ? ref.workspaceHash : '';
  const draftSessionKey = sid ? `${draftWorkspaceHash}:${sid}` : '';
  composerValueRef.current = composerValue;
  const rawItems = items;
  // GitSessionPill 的 sessionStarted 判定在 submit 回调里读(ref 免 dep churn)。
  const rawItemsLengthRef = useRef(0);
  rawItemsLengthRef.current = rawItems.length;
  // 上下键翻的历史:per-cwd 输入历史 + 当前 transcript 会话中用户发过的消息
  const composerHistory = useMemo(
    () => buildComposerHistory({ cwdHistory: history, transcriptItems: rawItems }),
    [history, rawItems],
  );
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
  const itemsRef = useRef(renderedItems);
  const stickyRafRef = useRef(0);
  const conversationTurnRafRef = useRef(0);
  const conversationTurnsRef = useRef([]);
  const conversationTurnActivationRef = useRef(null);
  const searchJumpRetryRef = useRef({ frame: 0, timer: 0 });
  const [stickyUserContext, setStickyUserContext] = useState(null);
  const [conversationTurnState, setConversationTurnState] = useState({
    sid: '',
    turns: [],
  });
  const [activeConversationTurn, setActiveConversationTurn] = useState(-1);
  const preparedConversationTurns = useMemo(
    () => (
      conversationTurnState.sid === sid
        ? conversationTurnState.turns
        : []
    ),
    [conversationTurnState, sid],
  );
  const showConversationTurnScrubber = shouldShowConversationTurnScrubber(
    preparedConversationTurns,
  );
  const searchJumpOrdinal = useMemo(() => searchJumpOrdinalFromRef(ref), [ref]);

  useEffect(() => {
    if (!sid || transcriptLoadState !== 'loaded') return undefined;

    let cancelled = false;
    let frame = 0;
    let idle = 0;
    let timer = 0;
    const prepare = () => {
      if (cancelled) return;
      const nextTurns = buildConversationTurnPreviews(itemsRef.current, { busy });
      if (cancelled) return;
      setConversationTurnState({ sid, turns: nextTurns });
    };

    frame = window.requestAnimationFrame(() => {
      frame = 0;
      if (cancelled) return;
      if (typeof window.requestIdleCallback === 'function') {
        idle = window.requestIdleCallback(prepare, { timeout: 450 });
        return;
      }
      timer = window.setTimeout(prepare, 0);
    });

    return () => {
      cancelled = true;
      if (frame) window.cancelAnimationFrame(frame);
      if (idle && typeof window.cancelIdleCallback === 'function') {
        window.cancelIdleCallback(idle);
      }
      if (timer) window.clearTimeout(timer);
    };
  }, [busy, lastUserTurnKey, sid, transcriptLoadState]);

  const homeWorkspacePreferenceHash = homeWorkspaceSelection?.workspaceHash || '';
  const persistHomeWorkspaceHash = useCallback((hash = '') => {
    const workspaceHash = String(hash || '');
    setHomeWorkspaceSelection({ workspaceHash });
    writeDesktopHomeWorkspaceHash(workspaceHash).catch(() => {});
  }, [setHomeWorkspaceSelection]);

  const selectHomeWorkspace = useCallback((workspace) => {
    const selected = workspace?.noWorkspace ? noHomeWorkspaceOption() : workspace;
    const workspaceHash = String(selected?.hash || '');
    setHomeWorkspaceHash(workspaceHash);
    persistHomeWorkspaceHash(workspaceHash);
    onHomeWorkspaceChange?.(selected);
    setProjectDropdownOpen(false);
  }, [onHomeWorkspaceChange, persistHomeWorkspaceHash]);

  const handleProjectCreated = useCallback(async (workspace) => {
    const option = normalizeWorkspaceOption(workspace, homeWorkspaces.length);
    if (!option?.hash || !option?.cwd) {
      throw new Error('项目已创建，但返回的工作区信息不完整');
    }
    setHomeWorkspaces((previous) => [
      option,
      ...previous.filter((item) => item.hash !== option.hash),
    ]);
    selectHomeWorkspace(option);
    notifySessionListChanged({
      reason: 'project-created',
      workspaceHash: option.hash,
    });
    const directoryName = workspace?.directory_name || option.name;
    toast({
      kind: 'ok',
      text: workspace?.sanitized
        ? `已创建项目：${directoryName}（目录名已自动转换）`
        : `已创建项目：${directoryName}`,
    });
  }, [homeWorkspaces.length, selectHomeWorkspace]);

  const handleOpenExistingDirectory = useCallback(async () => {
    setProjectDropdownOpen(false);
    try {
      const workspace = await pickExistingWorkspace({ api });
      if (workspace == null) return;
      const option = normalizeWorkspaceOption(workspace, homeWorkspaces.length);
      if (!option?.hash || !option?.cwd) {
        throw new Error('打开的目录缺少工作区信息');
      }
      setHomeWorkspaces((previous) => [
        option,
        ...previous.filter((item) => item.hash !== option.hash),
      ]);
      selectHomeWorkspace(option);
      notifySessionListChanged({
        reason: 'workspace-opened',
        workspaceHash: option.hash,
      });
    } catch (error) {
      if (!hasDesktopBridge() && (error?.status === 404 || error?.status === 501)) {
        toast({ kind: 'info', text: '需在 desktop webapp 中使用' });
      } else {
        toast({ kind: 'err', text: `打开现有目录失败：${error?.message || ''}` });
      }
    }
  }, [api, homeWorkspaces.length, selectHomeWorkspace]);

  const selectedHomeWorkspace = useMemo(() => {
    return homeWorkspaceOptionForHash(homeWorkspaces, homeWorkspaceHash);
  }, [homeWorkspaceHash, homeWorkspaces]);

  const commandWorkspaceHash = useMemo(() => commandWorkspaceHashForInput({
    activeRef: ref,
    selectedHomeWorkspace,
    hasSession: !!sid,
  }), [ref, selectedHomeWorkspace, sid]);

  useEffect(() => {
    if (sid) return;
    setHomeExpertId(String(ref?.expertId || ref?.expert_id || ref?.expert?.id || ''));
  }, [ref?.expert?.id, ref?.expertId, ref?.expert_id, sid]);

  useEffect(() => {
    let alive = true;
    api.listExperts(commandWorkspaceHash || '__local__')
      .then((result) => { if (alive) setExperts(normalizeExperts(result)); })
      .catch(() => { if (alive) setExperts([]); });
    return () => { alive = false; };
  }, [api, commandWorkspaceHash]);
  const consoleCwd = useMemo(() => consoleCwdForContext({
    activeRef: ref,
    selectedHomeWorkspace,
    health,
  }), [health?.cwd, ref, selectedHomeWorkspace]);

  useEffect(() => {
    onCommandWorkspaceChange?.(commandWorkspaceHash);
  }, [commandWorkspaceHash, onCommandWorkspaceChange]);
  useEffect(() => {
    onConsoleCwdChange?.(consoleCwd);
  }, [consoleCwd, onConsoleCwdChange]);

  useEffect(() => { sidRef.current = sid; }, [sid]);
  useEffect(() => { draftSessionKeyRef.current = draftSessionKey; }, [draftSessionKey]);
  useEffect(() => { queueStateRef.current = queueState; }, [queueState]);
  useEffect(() => {
    sideQuestionEpochRef.current += 1;
    setSideQuestion(null);
  }, [sid]);

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
    const targetNoWorkspace = !!target?.noWorkspace;
    const baseOptions = withCreateSessionPreferences(
      createOptions || sessionCreateOptionsForText(text),
      { modelName: homeModelName, permissionMode },
    );
    const expertOptions = homeExpertId ? { expert_id: homeExpertId, expertId: homeExpertId } : {};
    const options = targetNoWorkspace
      ? { ...baseOptions, ...expertOptions, no_workspace: true, noWorkspace: true }
      : { ...baseOptions, ...expertOptions };
    const create = isRealWorkspaceHash(targetHash)
      ? api.createWorkspaceSession(targetHash, options)
      : api.createSession(options);
    setHomeSubmitting(true);
    try {
      const r = await create;
      const id = r && (r.session_id || r.id);
      if (!id) throw new Error('missing session id');
      const next = newSessionRefFrom(ref, id);
      if (targetNoWorkspace) {
        next.noWorkspace = true;
        next.workspaceHash = '';
        next.workspaceName = '';
        next.cwd = '';
      } else if (r.workspace_hash || isRealWorkspaceHash(targetHash)) {
        next.workspaceHash = r.workspace_hash || targetHash;
        next.workspaceName = target?.name || ref?.workspaceName;
        next.cwd = r.cwd || target?.cwd || ref?.cwd;
      }
      next.title = title || text;
      if (homeExpertId) {
        const selectedExpert = experts.find((expert) => expert.id === homeExpertId) || ref?.expert || null;
        next.expertId = homeExpertId;
        next.expert_id = homeExpertId;
        if (selectedExpert) next.expert = selectedExpert;
      }
      if (preserveExtras) preserveComposerExtrasOnSessionChangeRef.current = true;
      onSessionPromoted?.(next);
      notifySessionListChanged({
        reason: 'session-created',
        sessionId: id,
        workspaceHash: targetNoWorkspace ? '' : (next.workspaceHash || ''),
        noWorkspace: targetNoWorkspace,
        session: {
          ...next,
          id,
          workspace_hash: targetNoWorkspace ? '' : (next.workspaceHash || ''),
          no_workspace: targetNoWorkspace,
        },
      });
      return { id, response: r, target };
    } finally {
      setHomeSubmitting(false);
    }
  }, [api, experts, health, homeExpertId, homeModelName, homeSubmitting, onSessionPromoted, permissionMode, ref, selectedHomeWorkspace]);

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
      if (!next) {
        selectionPreviewFingerprintRef.current = '';
        setSelectionPreview((prev) => (prev ? null : prev));
        return;
      }
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
        setHomeModelName(resolveHomeModelName(options, defaultName, ''));
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
  }, [api, modelProfileRevision, ref?.context_window, ref?.deleted, ref?.model, ref?.modelDeleted, ref?.model_deleted, ref?.model_name, ref?.model_preset, ref?.provider, ref?.workspaceHash, sid]);

  const refreshSessionModels = useCallback(async () => {
    if (modelRefreshing) return;
    const targetSid = sid;
    const workspaceHash = ref?.workspaceHash || '';
    setModelRefreshing(true);
    try {
      const requests = targetSid
        ? [api.listModels(), api.getSessionModel(targetSid, workspaceHash)]
        : [api.listModels(), api.getDefaultModel(), api.getDefaultPermissionMode()];
      const [modelsResult, stateResult, permissionResult] = await Promise.allSettled(requests);
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
        setHomeModelName(resolveHomeModelName(nextOptions, defaultName, ''));
        if (permissionResult?.status === 'fulfilled') {
          setPermissionMode(normalizePermissionMode(permissionResult.value?.mode));
        }
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
      let cancelled = false;
      setPermissionSwitching(false);
      api.getDefaultPermissionMode()
        .then((state) => {
          if (!cancelled) setPermissionMode(normalizePermissionMode(state?.mode));
        })
        .catch(() => {
          if (!cancelled) setPermissionMode('default');
        });
      return () => { cancelled = true; };
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

  const measureStickyContext = useCallback((rowMetrics = null) => {
    const el = scrollRef.current;
    if (!el) {
      setStickyUserContext(null);
      return;
    }
    const nextContext = findStickyUserContext({
      items: itemsRef.current,
      rowMetrics: Array.isArray(rowMetrics) ? rowMetrics : collectRowMetrics(el),
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

  const measureConversationTurn = useCallback((rowMetrics = null) => {
    const el = scrollRef.current;
    const turnsForRail = conversationTurnsRef.current;
    if (!el || turnsForRail.length === 0) {
      conversationTurnActivationRef.current = null;
      setActiveConversationTurn(-1);
      return;
    }
    const activation = conversationTurnActivationRef.current;
    const activatedIndex = activation?.sid === sid
      ? resolveActivatedConversationTurnIndex(
        turnsForRail,
        activation,
        el.scrollTop,
      )
      : -1;
    if (activatedIndex >= 0) {
      setActiveConversationTurn((previous) => (
        previous === activatedIndex ? previous : activatedIndex
      ));
      return;
    }
    conversationTurnActivationRef.current = null;
    const nextIndex = resolveActiveConversationTurnIndex(
      turnsForRail,
      Array.isArray(rowMetrics) ? rowMetrics : collectRowMetrics(el),
      el.scrollTop,
    );
    setActiveConversationTurn((previous) => (
      previous === nextIndex ? previous : nextIndex
    ));
  }, [sid]);

  const scheduleTranscriptMeasures = useCallback(() => {
    if (stickyRafRef.current) {
      cancelAnimationFrame(stickyRafRef.current);
      stickyRafRef.current = 0;
    }
    if (conversationTurnRafRef.current) {
      cancelAnimationFrame(conversationTurnRafRef.current);
    }
    conversationTurnRafRef.current = requestAnimationFrame(() => {
      conversationTurnRafRef.current = 0;
      const el = scrollRef.current;
      const rowMetrics = collectRowMetrics(el);
      measureStickyContext(rowMetrics);
      measureConversationTurn(rowMetrics);
    });
  }, [measureConversationTurn, measureStickyContext]);

  const setTailFollowFromAction = useCallback((action) => {
    tailFollowStateRef.current = nextChatTailFollowState(tailFollowStateRef.current, action);
  }, []);

  const cancelTailFollowScroll = useCallback(() => {
    const pending = tailFollowScrollRafRef.current || {};
    if (pending.first) cancelAnimationFrame(pending.first);
    if (pending.second) cancelAnimationFrame(pending.second);
    tailFollowScrollRafRef.current = { first: 0, second: 0 };
  }, []);

  const scheduleTailFollowScroll = useCallback(() => {
    const scrollToBottom = () => {
      if (!shouldAutoFollowChatTail(tailFollowStateRef.current)) return false;
      const el = scrollRef.current;
      if (!el) return false;
      el.scrollTop = el.scrollHeight;
      return true;
    };

    cancelTailFollowScroll();
    if (!scrollToBottom()) return;

    tailFollowScrollRafRef.current.first = requestAnimationFrame(() => {
      tailFollowScrollRafRef.current.first = 0;
      if (!scrollToBottom()) return;
      tailFollowScrollRafRef.current.second = requestAnimationFrame(() => {
        tailFollowScrollRafRef.current.second = 0;
        scrollToBottom();
      });
    });
  }, [cancelTailFollowScroll]);

  const pauseTailFollowForReview = useCallback(() => {
    cancelTailFollowScroll();
    if (!busy && transcriptStatus !== 'running') return;
    setTailFollowFromAction({ type: 'review_pause' });
  }, [busy, cancelTailFollowScroll, setTailFollowFromAction, transcriptStatus]);

  const handleMessagesScroll = useCallback(() => {
    const el = scrollRef.current;
    if (el) {
      const metrics = chatScrollMetrics(el);
      setTailFollowFromAction({
        type: 'scroll',
        metrics,
        prevMetrics: scrollActivityRef.current.prev,
        userGesture: scrollActivityRef.current.pointerActive,
      });
      scrollActivityRef.current.prev = metrics;
    }
    scheduleTranscriptMeasures();
  }, [scheduleTranscriptMeasures, setTailFollowFromAction]);

  // 滚轮上滚是最明确的"用户想往回看"信号,不等 scroll 事件的启发式判定,
  // 直接暂停跟随(仅回合进行中生效,见 pauseTailFollowForReview 内的门)。
  const handleMessagesWheel = useCallback((event) => {
    if (event.deltaY < 0) pauseTailFollowForReview();
  }, [pauseTailFollowForReview]);

  const handleMessagesPointerDown = useCallback(() => {
    scrollActivityRef.current.pointerActive = true;
  }, []);

  useEffect(() => {
    const clearPointerActive = () => {
      scrollActivityRef.current.pointerActive = false;
    };
    window.addEventListener('pointerup', clearPointerActive);
    window.addEventListener('pointercancel', clearPointerActive);
    return () => {
      window.removeEventListener('pointerup', clearPointerActive);
      window.removeEventListener('pointercancel', clearPointerActive);
    };
  }, []);

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

  const jumpToConversationTurn = useCallback((turn, index) => {
    const el = scrollRef.current;
    const targetId = String(turn?.itemId || '');
    if (!el || !targetId) return;

    const targetRow = Array.from(el.querySelectorAll('[data-chat-row="true"]'))
      .find((row) => row.getAttribute('data-chat-item-id') === targetId);
    if (!targetRow) return;

    const containerRect = el.getBoundingClientRect();
    const rowRect = targetRow.getBoundingClientRect();
    const targetScrollTop = scrollTopForStickySourceRow({
      scrollTop: el.scrollTop,
      containerTop: containerRect.top,
      rowTop: rowRect.top,
      topInset: 20,
    });

    pauseTailFollowForReview();
    conversationTurnActivationRef.current = {
      sid,
      itemId: targetId,
      scrollTop: el.scrollTop,
    };
    flushSync(() => {
      setActiveConversationTurn(index);
    });
    el.scrollTop = targetScrollTop;
    conversationTurnActivationRef.current = {
      sid,
      itemId: targetId,
      scrollTop: el.scrollTop,
    };
    window.requestAnimationFrame(scheduleTranscriptMeasures);
  }, [pauseTailFollowForReview, scheduleTranscriptMeasures, sid]);

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
    scrollActivityRef.current = { prev: null, pointerActive: false };
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
    scheduleTailFollowScroll();
    return cancelTailFollowScroll;
  }, [
    activity?.detail,
    activity?.label,
    activity?.phase,
    activity?.toolCallId,
    activity?.toolIndex,
    busy,
    cancelTailFollowScroll,
    changeDockBottomPadding,
    permissionRequests,
    renderedItems,
    scheduleTailFollowScroll,
    sid,
  ]);

  useEffect(() => observeChatTailContent(
    transcriptContentRef.current,
    scheduleTailFollowScroll,
  ), [scheduleTailFollowScroll, sid]);

  useLayoutEffect(() => {
    const previous = searchJumpRetryRef.current || {};
    if (previous.frame) cancelAnimationFrame(previous.frame);
    if (previous.timer) window.clearTimeout(previous.timer);
    searchJumpRetryRef.current = { frame: 0, timer: 0 };

    if (!sid || searchJumpOrdinal === null) return undefined;

    let cancelled = false;
    const task = { frame: 0, timer: 0, attempts: 0, settled: 0 };
    searchJumpRetryRef.current = task;

    const scheduleRetry = (delay) => {
      task.timer = window.setTimeout(() => {
        task.timer = 0;
        task.frame = requestAnimationFrame(run);
      }, delay);
    };

    const run = () => {
      task.frame = 0;
      if (cancelled || searchJumpRetryRef.current !== task) return;
      task.attempts += 1;

      const el = scrollRef.current;
      const targetRow = searchJumpTargetRow(el, searchJumpOrdinal);
      if (el && targetRow) {
        cancelTailFollowScroll();
        setTailFollowFromAction({ type: 'review_pause' });
        el.scrollTop = scrollTopForCenteredRow(el, targetRow);
        scheduleStickyMeasure();
        task.settled += 1;
      }

      if (task.settled >= 3 || task.attempts >= 12) {
        if (searchJumpRetryRef.current === task) {
          searchJumpRetryRef.current = { frame: 0, timer: 0 };
        }
        return;
      }
      scheduleRetry(targetRow ? 70 : 90);
    };

    run();
    return () => {
      cancelled = true;
      if (task.frame) cancelAnimationFrame(task.frame);
      if (task.timer) window.clearTimeout(task.timer);
      if (searchJumpRetryRef.current === task) {
        searchJumpRetryRef.current = { frame: 0, timer: 0 };
      }
    };
  }, [
    cancelTailFollowScroll,
    changeDockBottomPadding,
    ref,
    renderedItems,
    scheduleStickyMeasure,
    searchJumpOrdinal,
    setTailFollowFromAction,
    sid,
  ]);

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
    return bindDesktopComposerAutoFocus({
      enabled: autoFocusOnDesktopWindowFocus,
      onFocus: () => restoreChatInputFocusSoon(true),
    });
  }, [autoFocusOnDesktopWindowFocus, restoreChatInputFocusSoon]);

  useLayoutEffect(() => {
    itemsRef.current = renderedItems;
    scheduleTranscriptMeasures();
  }, [renderedItems, scheduleTranscriptMeasures]);

  useLayoutEffect(() => {
    scheduleTranscriptMeasures();
  }, [permissionRequests, scheduleTranscriptMeasures]);

  useLayoutEffect(() => {
    conversationTurnsRef.current = preparedConversationTurns;
    scheduleTranscriptMeasures();
  }, [preparedConversationTurns, scheduleTranscriptMeasures]);

  useEffect(() => {
    conversationTurnActivationRef.current = null;
    setStickyUserContext(null);
    setActiveConversationTurn(-1);
    scheduleTranscriptMeasures();
  }, [sid, scheduleTranscriptMeasures]);

  useEffect(() => () => {
    if (stickyRafRef.current) {
      cancelAnimationFrame(stickyRafRef.current);
      stickyRafRef.current = 0;
    }
    if (conversationTurnRafRef.current) {
      cancelAnimationFrame(conversationTurnRafRef.current);
      conversationTurnRafRef.current = 0;
    }
  }, []);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;

    const onResize = () => scheduleTranscriptMeasures();
    window.addEventListener('resize', onResize);

    let mutationObserver = null;
    if (typeof MutationObserver !== 'undefined') {
      mutationObserver = new MutationObserver(scheduleTranscriptMeasures);
      mutationObserver.observe(el, { childList: true, subtree: true, characterData: true });
    }

    let resizeObserver = null;
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(scheduleTranscriptMeasures);
      resizeObserver.observe(el);
    }

    scheduleTranscriptMeasures();
    return () => {
      window.removeEventListener('resize', onResize);
      mutationObserver?.disconnect();
      resizeObserver?.disconnect();
    };
  }, [sid, scheduleTranscriptMeasures]);

  useEffect(() => {
    if (sid || !ref?.homeWorkspaceExplicit) return;
    persistHomeWorkspaceHash(ref?.workspaceHash || '');
  }, [persistHomeWorkspaceHash, ref?.homeWorkspaceExplicit, ref?.workspaceHash, sid]);

  useEffect(() => {
    if (sid) setCreateProjectOpen(false);
  }, [sid]);

  useEffect(() => {
    if (sid) return undefined;
    let cancelled = false;

    const load = async () => {
      const explicitHomeWorkspace = !!ref?.homeWorkspaceExplicit;
      let preferredHash = explicitHomeWorkspace ? '' : homeWorkspacePreferenceHash;
      if (!explicitHomeWorkspace) {
        const desktopHash = await readDesktopHomeWorkspaceHash();
        if (desktopHash !== null) {
          preferredHash = desktopHash;
          if (desktopHash !== homeWorkspacePreferenceHash) {
            setHomeWorkspaceSelection({ workspaceHash: desktopHash });
          }
        }
      }

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
        const explicitHash = ref?.homeWorkspaceExplicit ? (ref?.workspaceHash || '') : '';
        return resolveHomeWorkspaceHash({
          preferredHash,
          explicitHash,
          explicitHashSet: explicitHomeWorkspace,
          previousHash: prev,
          options,
        });
      });
    };

    load();
    return () => { cancelled = true; };
  }, [api, health, homeWorkspacePreferenceHash, ref, setHomeWorkspaceSelection, sid]);

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

  const runSideQuestion = useCallback((
    rawQuestion,
    { command = 'btw', recordHistory = false } = {},
  ) => {
    const question = String(rawQuestion || '').trim();
    const targetSid = sidRef.current;
    if (!targetSid) {
      toast({ kind: 'err', text: '请先在已有会话中使用 /btw 或 /side' });
      return null;
    }
    if (!question) {
      toast({ kind: 'err', text: `用法：/${command} <问题>` });
      return null;
    }
    if (sideQuestionInFlightRef.current) {
      toast({ kind: 'err', text: '已有旁路提问正在回答，请稍候' });
      return null;
    }

    sideQuestionInFlightRef.current = true;
    const requestEpoch = sideQuestionEpochRef.current;
    if (recordHistory) recordInputHistory(`/${command} ${question}`);
    setSideQuestion({ status: 'loading', question, answer: '', error: '' });

    return api.askSideQuestion(targetSid, question)
      .then((result) => {
        if (sidRef.current === targetSid && sideQuestionEpochRef.current === requestEpoch) {
          setSideQuestion({
            status: 'success',
            question: String(result?.question || question),
            answer: String(result?.answer || ''),
            error: '',
          });
        }
        return true;
      })
      .catch((e) => {
        const message = e?.message || '旁路提问请求失败';
        if (sidRef.current === targetSid && sideQuestionEpochRef.current === requestEpoch) {
          setSideQuestion({ status: 'error', question, answer: '', error: message });
        }
        return false;
      })
      .finally(() => {
        sideQuestionInFlightRef.current = false;
      });
  }, [api, recordInputHistory]);

  const guideQueued = useCallback((queuedId) => {
    const targetSid = sidRef.current;
    const expectedTurnId = String(activeTurnId || '');
    if (!targetSid || !busy || !expectedTurnId) {
      toast({ kind: 'err', text: '当前没有可引导的运行中回合' });
      return null;
    }
    const queuedItem = queueStateRef.current.items.find(
      (item) => item?.queued?.id === queuedId,
    );
    const requestPayload = queuedInputRequestPayload(queuedItem);
    if (!requestPayload) {
      toast({ kind: 'err', text: '找不到这条排队消息' });
      return null;
    }

    updateQueueState((prev) => beginQueuedGuidance(
      prev,
      queuedId,
      { turnId: expectedTurnId },
    ));
    return api.steerTurn(targetSid, {
      ...requestPayload,
      expected_turn_id: expectedTurnId,
    })
      .then((result) => {
        updateQueueState((prev) => markQueuedGuidanceAccepted(
          prev,
          queuedId,
          { turnId: result?.turn_id || expectedTurnId },
        ));
        return true;
      })
      .catch((e) => {
        updateQueueState((prev) => finishQueuedGuidance(
          prev,
          queuedId,
          { succeeded: false },
        ));
        toast({ kind: 'err', text: '引导提交失败:' + (e?.message || '未知错误') });
        return false;
      });
  }, [activeTurnId, api, busy, updateQueueState]);

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
    if (route.kind === 'desktop_feedback') {
      if (!sid) {
        toast({ kind: 'err', text: tr('feedbackCommand.requiresSession') });
        return;
      }
      if (hasExtras) {
        toast({ kind: 'err', text: tr('feedbackCommand.textOnly') });
        return;
      }
      if (composerSubmitting) return;

      const noWorkspace = !!(ref?.noWorkspace || ref?.no_workspace);
      const requestPayload = buildCurrentSessionDesktopFeedbackPayload({
        feedbackText: route.feedbackText,
        sessionId: sid,
        workspaceHash:
          ref?.workspaceHash || ref?.workspace_hash || draftWorkspaceHash,
        noWorkspace,
      });
      if (!requestPayload) {
        toast({ kind: 'err', text: tr('feedbackCommand.unknownSession') });
        return;
      }

      setComposerSubmitting(true);
      api.submitDesktopFeedback(requestPayload)
        .then((result) => {
          recordInputHistory(route.display_text);
          clearCurrentSessionDraft();
          const packageName = String(result?.package_filename || '').trim();
          toast({
            kind: 'ok',
            text: packageName
              ? tr('feedbackCommand.uploadedWithPackage', { packageName })
              : tr('feedbackCommand.uploaded'),
          });
        })
        .catch((e) => {
          toast({
            kind: 'err',
            text: tr('feedbackCommand.uploadFailed', {
              error: e?.message || tr('common.unknown'),
            }),
          });
        })
        .finally(() => {
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    if (route.kind === 'side_question') {
      if (hasExtras) {
        toast({ kind: 'err', text: `/${route.command} 暂不支持附件或上下文，请仅提交文字问题` });
        return;
      }
      const started = runSideQuestion(route.question, {
        command: route.command,
        recordHistory: true,
      });
      if (started) {
        clearCurrentSessionDraft();
        clearComposerExtras();
        restoreChatInputFocusSoon(false);
      }
      return;
    }
    if (route.kind === 'turn_steer') {
      if (!sid) {
        toast({ kind: 'err', text: '请先在运行中的会话里使用 /turn' });
        return;
      }
      if (!busy || !activeTurnId) {
        toast({ kind: 'err', text: '当前没有可引导的运行中回合' });
        return;
      }
      if (!route.guidance && !hasExtras) {
        toast({ kind: 'err', text: '用法：/turn <引导内容>' });
        return;
      }
      if (composerSubmitting) return;

      const targetSid = sid;
      const expectedTurnId = activeTurnId;
      const steerPayload = {
        ...payload,
        text: route.guidance,
        expected_turn_id: expectedTurnId,
        client_message_id:
          `turn-${targetSid}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      };
      setComposerSubmitting(true);
      api.steerTurn(targetSid, steerPayload)
        .then(() => {
          recordInputHistory(route.display_text);
          clearCurrentSessionDraft();
          clearComposerExtras();
          toast({ kind: 'ok', text: '引导已提交，等待当前回合接收' });
        })
        .catch((e) => {
          toast({ kind: 'err', text: '引导提交失败:' + (e?.message || '未知错误') });
        })
        .finally(() => {
          setComposerSubmitting(false);
          restoreChatInputFocusSoon(false);
        });
      return;
    }
    const isBuiltin = !hasExtras && route.kind === 'builtin';
    if (!isBuiltin || hasExtras) {
      setTailFollowFromAction({ type: 'new_turn' });
      // 新一轮对话开始:上一轮的玻璃 dock(变更汇总 + todo 环)自动收起,
      // 本轮产生新变更 / todo 更新后按签名机制重现。
      dockAutoDismissRef.current();
    }
    if (!sid) {
      // 自动新建会话。普通消息由 daemon auto_start 接管;builtin 先创建
      // 空会话,再走专门 command endpoint。
      const trimmed = String(payload.text || '').trim();
      if ((!trimmed && !hasExtras) || homeSubmitting) return;
      // GitSessionPill 的 worktree 意图:命中时改走 auto_start:false +
      // 首条消息携带 worktree 字段(daemon 在入队前创建并切 cwd)。
      // builtin(/init 等)不带 worktree —— 意图只作用于普通消息。
      const worktreeIntent = !isBuiltin
        ? buildWorktreeIntent({ ...gitPillIntentRef.current, sessionStarted: false })
        : null;
      const createOptions = hasExtras || worktreeIntent
        ? { auto_start: false }
        : sessionCreateOptionsForText(payload.text);
      createHomeComposerSession(payload.text, { createOptions })
        .then(async (created) => {
          const id = created?.id;
          if (!id) return;
          if (isBuiltin) {
            await api.executeCommand(id, route.command);
          } else if (hasExtras || worktreeIntent) {
            applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
            const sendPayload = worktreeIntent
              ? { ...payload, worktree: worktreeIntent }
              : payload;
            await sendInputOrBuiltin(id, sendPayload);
            if (worktreeIntent) {
              setLocalWorktree({ sid: id, name: `ses-${id}` });
              const noWorkspace = !!created?.target?.noWorkspace;
              notifySessionListChanged({
                reason: 'worktree-created',
                sessionId: id,
                workspaceHash: noWorkspace
                  ? ''
                  : (created?.response?.workspace_hash || created?.target?.hash || ''),
                noWorkspace,
              });
            }
          }
          if (payload.text.trim()) recordInputHistory(payload.text);
          if (hasExtras) {
            clearComposerExtras();
          }
        })
        .catch((e) => {
          if (hasExtras || worktreeIntent) {
            applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
          }
          toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
        })
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
    if (!isBuiltin) {
      applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
    }
    // 空会话的首条消息:GitSessionPill 勾了 worktree 时随消息带创建意图。
    const sessionWorktreeIntent = !isBuiltin
      ? buildWorktreeIntent({
          ...gitPillIntentRef.current,
          sessionStarted: rawItemsLengthRef.current > 0,
        })
      : null;
    const sessionSendPayload = sessionWorktreeIntent
      ? { ...payload, worktree: sessionWorktreeIntent }
      : payload;
    sendInputOrBuiltin(targetSid, sessionSendPayload)
      .then(() => {
        if (sessionWorktreeIntent) {
          setLocalWorktree({ sid: targetSid, name: `ses-${targetSid}` });
          const noWorkspace = !!(ref?.noWorkspace || ref?.no_workspace);
          notifySessionListChanged({
            reason: 'worktree-created',
            sessionId: targetSid,
            workspaceHash: noWorkspace
              ? ''
              : (ref?.workspaceHash || ref?.workspace_hash || ''),
            noWorkspace,
          });
        }
        if (payload.text.trim()) recordInputHistory(payload.text);
        if (!ref?.title) setTranscriptTitle(payload.text || composerAttachments[0]?.name || '附件消息');
        clearCurrentSessionDraft();
        clearComposerExtras();
      })
      .catch((e) => {
        toast({ kind: 'err', text: '发送失败:' + (e.message || '') });
        applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
      })
      .finally(() => setComposerSubmitting(false));
  }, [sid, busy, activeTurnId, api, homeSubmitting, recordInputHistory, enqueueInput, applyEvent, setTranscriptTitle, sendInputOrBuiltin, composerSubmitting, clearCurrentSessionDraft, composerAttachments, composerContexts, clearComposerExtras, createHomeComposerSession, restoreChatInputFocusSoon, setTailFollowFromAction, runSideQuestion, draftWorkspaceHash, ref?.noWorkspace, ref?.no_workspace, ref?.workspaceHash, ref?.workspace_hash]);

  const drainQueuedInput = useCallback(() => {
    const targetSid = sidRef.current;
    if (!targetSid || busy || drainRef.current) return;
    const queuedItem = nextQueuedInput(queueStateRef.current, targetSid);
    if (!queuedItem) return;

    drainRef.current = true;
    setTailFollowFromAction({ type: 'new_turn' });
    updateQueueState((prev) => markQueuedInputSending(prev, queuedItem.queued.id));
    const queuedPayload = queuedItem.queued?.payload || queuedItem.content;
    const queuedIsBuiltin = !payloadHasExtras(queuedPayload) &&
      inputRouteForText(payloadText(queuedPayload)).kind === 'builtin';
    const sendPayload = queuedIsBuiltin
      ? queuedPayload
      : (queuedInputRequestPayload(queuedItem) || queuedPayload);
    if (!queuedIsBuiltin) {
      applyEvent({ type: 'busy_changed', payload: { busy: true } }, { emitEffects: false });
    }
    sendInputOrBuiltin(targetSid, sendPayload)
      .then(() => {
        if (!queuedIsBuiltin && sidRef.current === targetSid) {
          const acceptedEvent = acceptedQueuedInputEvent(queuedItem);
          if (acceptedEvent) applyEvent(acceptedEvent, { emitEffects: false });
        }
        updateQueueState((prev) => markQueuedInputCompleted(prev, queuedItem.queued.id));
      })
      .catch((e) => {
        const message = e?.message || '发送失败';
        if (!queuedIsBuiltin && sidRef.current === targetSid) {
          applyEvent({ type: 'busy_changed', payload: { busy: false } }, { emitEffects: false });
        }
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
    const restored = restoreUncommittedGuidanceForSession(
      queueStateRef.current,
      sid,
    );
    if (restored !== queueStateRef.current) {
      updateQueueState(restored);
    }
    if (wasBusy || !hasSendingQueuedInput(queueState, sid)) {
      drainQueuedInput();
    }
  }, [busy, drainQueuedInput, queueState, sid, updateQueueState]);

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
        clientMessageId: item.metadata?.client_message_id,
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

  const selectHomeModel = useCallback(async (name) => {
    const nextName = String(name || '');
    const previousName = String(homeModelName || '');
    if (!nextName || nextName === previousName || modelRefreshing || modelSwitching) return;
    setHomeModelName(nextName);
    setModelSwitching(true);
    try {
      const state = await api.setDefaultModel(nextName);
      const confirmedName = String(state?.default_model_name || state?.name || nextName);
      setHomeModelName(confirmedName || nextName);
      toast({ kind: 'ok', text: '默认模型已设为 ' + (confirmedName || nextName) });
    } catch (e) {
      setHomeModelName(previousName);
      toast({ kind: 'err', text: '默认模型设置失败:' + (e?.message || '') });
    } finally {
      setModelSwitching(false);
    }
  }, [api, homeModelName, modelRefreshing, modelSwitching]);

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

  const changeComposerModel = useCallback((name) => {
    if (sid) void switchSessionModel(name);
    else void selectHomeModel(name);
  }, [selectHomeModel, sid, switchSessionModel]);

  const switchHomeDefaultPermissionMode = useCallback(async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permissionMode);
    if (nextMode === previousMode || permissionSwitching) return;
    setPermissionMode(nextMode);
    setPermissionSwitching(true);
    try {
      const state = await api.setDefaultPermissionMode(nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermissionMode(confirmedMode);
      toast({ kind: 'ok', text: '默认权限模式已设为 ' + permissionModeOption(confirmedMode).label });
    } catch (e) {
      setPermissionMode(previousMode);
      toast({ kind: 'err', text: '默认权限模式设置失败:' + (e?.message || '') });
    } finally {
      setPermissionSwitching(false);
    }
  }, [api, permissionMode, permissionSwitching]);

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

  const changeComposerPermissionMode = useCallback((mode) => {
    if (sid) void switchPermissionMode(mode);
    else void switchHomeDefaultPermissionMode(mode);
  }, [sid, switchHomeDefaultPermissionMode, switchPermissionMode]);

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
    const startWidth = renderedPreviewPanelWidthRef.current;
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
  }, [onPreviewPanelResize, sid]);

  const onPreviewPanelHandleKeyDown = useCallback((event) => {
    if (!onPreviewPanelResize) return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowLeft' ? step : -step;
      const contentWidth = layoutRef.current?.getBoundingClientRect().width || 0;
      onPreviewPanelResize(renderedPreviewPanelWidthRef.current + delta, contentWidth);
    }
  }, [onPreviewPanelResize]);

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
      const noWorkspace = !!(ref?.noWorkspace || r?.no_workspace || r?.noWorkspace);
      const workspaceHash = noWorkspace ? '' : (r.workspace_hash || ref?.workspaceHash || '');
      const cwd = noWorkspace ? '' : (r.cwd || ref?.cwd || '');
      const now = new Date().toISOString();
      const forkedSession = {
        ...r,
        id: r.session_id,
        active: true,
        status: 'idle',
        attention_state: 'read',
        read_state: 'read',
        no_workspace: noWorkspace,
        noWorkspace,
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
        noWorkspace,
      });
      notifySessionListChanged({
        reason: 'fork',
        sessionId: r.session_id,
        workspaceHash,
        noWorkspace,
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

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT) return;
      detail.handled = true;
      const referencePath = normalizeReferencePath(
        target?.relativePath || target?.absolutePath || '',
      );
      if (target?.kind !== 'directory' || !referencePath) {
        toast({ kind: 'err', text: '无法获取文件夹路径' });
        return;
      }
      const insertion = inputRef.current?.insertDirectoryReference?.(referencePath);
      if (!insertion) {
        toast({ kind: 'err', text: '输入框当前不可用' });
        return;
      }
      toast({ kind: 'ok', text: '已加入输入上下文' });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, []);

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
  const selectedHomeModel = modelOptions.find((option) => option.name === homeModelName);
  const homeModelLabel = modelDisplayLabel(
    selectedHomeModel || (homeModelName ? { name: homeModelName } : null),
    homeModelFallback,
  );
  const boundExpertId = String(ref?.expertId || ref?.expert_id || ref?.expert?.id || '');
  const displayedExperts = useMemo(() => {
    if (!boundExpertId || experts.some((expert) => expert.id === boundExpertId)) return experts;
    if (ref?.expert && typeof ref.expert === 'object') {
      return normalizeExperts([ref.expert, ...experts]);
    }
    return [{
      id: boundExpertId,
      display_name: ref?.expert?.missing ? `${boundExpertId}（已缺失）` : boundExpertId,
      type: 'agent',
      source: 'global',
      managed_global: false,
      quick_prompts: [],
    }, ...experts];
  }, [boundExpertId, experts, ref?.expert]);
  const currentContextWindow = Number(modelState?.contextWindow || ref?.context_window || ref?.contextWindow || 0) || 0;
  const tokenBudget = useMemo(() => normalizeTokenBudget({
    usage: tokenUsage,
    contextWindow: currentContextWindow,
  }), [currentContextWindow, tokenUsage]);
  const homeContextWindow = Number(selectedHomeModel?.contextWindow || 0) || 0;
  const homeTokenBudget = useMemo(() => normalizeTokenBudget({
    usage: null,
    contextWindow: homeContextWindow,
  }), [homeContextWindow]);
  const displayedModelOptions = useMemo(() => {
    const currentName = selectedModelName(modelState);
    if (!currentName || modelOptions.some((m) => m.name === currentName)) return modelOptions;
    const normalized = normalizeModelState(modelState);
    return normalized ? [normalized, ...modelOptions] : modelOptions;
  }, [modelOptions, modelState]);
  // 当前/首屏模型的池负载(按 model id 精确匹配 modelPoolName;未命中 → null)。
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
  const latestSuccessfulChangedFiles = useMemo(
    () => latestTurnSuccessfulChangedFiles(items),
    [items],
  );
  // 每轮「本轮改动文件」列表:collectTurnChangeSetsFromItems 按 user 消息切
  // 回合聚合变更;列表渲染在回合末尾 = 下一个 user 行之前,最后一轮挂在
  // transcript 末尾(tail)。锚定基于 renderedItems(折叠投影后的视图)里的
  // user 行 —— user 行不参与活动折叠,id 与 rawItems 一致;万一锚找不到
  // (极端投影差异)兜底进 tail,列表不丢。
  const turnChangeSets = useMemo(() => collectTurnChangeSetsFromItems(items), [items]);
  const turnFileListPlacement = useMemo(() => {
    const before = new Map();
    const tail = [];
    if (!turnChangeSets.length) return { before, tail };
    const userIds = [];
    const userIndexById = new Map();
    for (const it of renderedItems) {
      if (it?.kind === 'msg' && it.role === 'user') {
        userIndexById.set(it.id, userIds.length);
        userIds.push(it.id);
      }
    }
    for (const set of turnChangeSets) {
      let nextUserId;
      if (set.userItemId) {
        const anchorIndex = userIndexById.get(set.userItemId);
        if (anchorIndex === undefined) {
          tail.push(set);
          continue;
        }
        nextUserId = userIds[anchorIndex + 1];
      } else {
        // orphan 变更集(回合头 user 消息缺失,如截断的 resume)排在首个 user 行之前
        nextUserId = userIds[0];
      }
      if (!nextUserId) {
        tail.push(set);
        continue;
      }
      const list = before.get(nextUserId) || [];
      list.push(set);
      before.set(nextUserId, list);
    }
    return { before, tail };
  }, [renderedItems, turnChangeSets]);
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
  // todo 环的下一轮自动收起:提交时记录的快照签名仍与当前一致 → 抑制;
  // agent 更新 todo(签名变化)或切换会话后自动解除。
  const todoSignature = useMemo(() => todoDockSignature(todos, todoSummary), [todos, todoSummary]);
  const todosSuppressed = isTodoDockSuppressed(todoDockSuppression, sid, todoSignature);
  const dockTodos = todosSuppressed ? [] : todos;
  const dockTodoSummary = todosSuppressed ? null : todoSummary;
  const hasVisibleTodos = Array.isArray(dockTodos) && dockTodos.length > 0;
  const showChangeDetails = changeSummary.hasChanges
    && !!changeSignature
    && dismissedDockSignature !== changeSignature;
  const showChangeDock = showChangeDetails || hasVisibleTodos;

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
  }, [
    showChangeDock,
    showChangeDetails,
    hasVisibleTodos,
    changeSummary.fileCount,
    changeSummary.totalAdditions,
    changeSummary.totalDeletions,
  ]);

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
    if (sidePanelCollapsed || sidePanelListCollapsed) onRevealSidePanelList?.();
    setReviewRequest((n) => n + 1);
  }, [onRevealSidePanelList, showSidePanel, sid, sidePanelCollapsed, sidePanelListCollapsed]);

  const dismissChangeDock = useCallback(() => {
    if (!changeDockDismissalKey || !changeSignature) return;
    setDismissedDockSignatures((prev) => dismissChangeDockSignature(
      prev,
      changeDockDismissalKey,
      changeSignature,
    ));
  }, [changeDockDismissalKey, changeSignature, setDismissedDockSignatures]);

  // 渲染期刷新 ref(纯缓存):submit 提交新一轮对话时经此收起整个 dock。
  dockAutoDismissRef.current = () => {
    dismissChangeDock();
    if (sid) setTodoDockSuppression({ sessionKey: sid, signature: todoSignature });
  };

  const questionForView = useMemo(() => {
    if (!questionRequest) return null;
    const reqSid = questionRequest.session_id || '';
    const ownerSid = questionRequest.owner_session_id || '';
    if (sid && ownerSid === sid) return questionRequest;
    if (!reqSid || (sid && reqSid === sid)) return questionRequest;
    // 后台任务(spawn_subagent 子会话)的 AskUserQuestion 冒泡到主会话回答,
    // transcript 窄条不承载交互(答案 payload 自带 session_id,路由回子会话)。
    if (sid && subagentTasks.tasks.some((t) => t.id === reqSid)) return questionRequest;
    return null;
  }, [questionRequest, sid, subagentTasks.tasks]);

  const questionOriginLabel = useMemo(() => {
    if (questionForView?.origin_label) return questionForView.origin_label;
    const reqSid = questionForView?.session_id || '';
    if (!reqSid || reqSid === sid) return '';
    const task = subagentTasks.tasks.find((t) => t.id === reqSid);
    return task ? `来自后台任务:${taskDisplayTitle(task)}` : '';
  }, [questionForView, sid, subagentTasks.tasks]);

  const conversationActivity = useMemo(() => selectConversationActivity({
    foregroundBusy: busy,
    foregroundActivity: activity,
    permissionRequests,
    questionRequest: questionForView,
    subagentTasks: subagentTasks.tasks,
  }), [
    activity,
    busy,
    permissionRequests,
    questionForView,
    subagentTasks.tasks,
  ]);

  const resolveQuestion = useCallback(() => {
    onQuestionResolve?.();
    requestAnimationFrame(() => inputRef.current?.focus());
  }, [onQuestionResolve]);

  const sidePanelFilesEnabled = !(ref?.noWorkspace || ref?.no_workspace);
  const sidePanelCwd = sidePanelFilesEnabled ? (ref?.cwd || health?.cwd || '') : '';
  const sidePanelMounted = showSidePanel && !!sid;
  const sidePanelNavigationCollapsed = sidePanelCollapsed || sidePanelListCollapsed;
  const previewScope = useMemo(
    () => {
      if (!sidePanelFilesEnabled) return sid || '';
      return previewScopeKey({
        cwd: sidePanelCwd,
        workspaceHash: ref?.workspaceHash || '',
      });
    },
    [ref?.workspaceHash, sid, sidePanelCwd, sidePanelFilesEnabled],
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
  // 总开关必须连最大化详情一起隐藏;恢复时仍保留最大化偏好与原页签。
  const previewPanelVisible = previewTabsOpen && !sidePanelCollapsed;
  const previewPanelMaximized = sidePanelMaximized && previewPanelVisible;
  const previewCloseConfirmMessage = previewCloseConfirm
    ? closeVisiblePreviewTabsConfirmationMessage(previewTabs.length) || previewCloseConfirm.message
    : '';
  const selectedChangeFile = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFile || ''
    : '';
  const selectedChangeFileRevision = activePreview?.type === PREVIEW_TAB_TYPES.SESSION_CHANGES
    ? activePreview.expandedFileRevision || 0
    : 0;
  const selectedGitChangeFile = activePreview?.type === PREVIEW_TAB_TYPES.GIT_CHANGES
    ? activePreview.expandedFile || ''
    : '';
  const contentLayout = useMemo(() => solveSingleContentLayout({
    contentWidth: layoutWidth,
    sidePanelWidth,
    previewPanelWidth,
    sidePanelVisible: sidePanelMounted,
    sidePanelCollapsed: sidePanelNavigationCollapsed,
    previewPanelVisible,
    previewPanelMaximized,
    previewPanelAutoFit,
  }), [
    layoutWidth,
    previewPanelAutoFit,
    previewPanelMaximized,
    previewPanelVisible,
    previewPanelWidth,
    sidePanelNavigationCollapsed,
    sidePanelMounted,
    sidePanelWidth,
  ]);
  const effectiveChatWidth = layoutWidth > 0 ? contentLayout.chatWidth : 0;
  const effectivePreviewPanelWidth = layoutWidth > 0 ? contentLayout.previewPanelWidth : previewPanelWidth;
  const effectiveSidePanelWidth = layoutWidth > 0 ? contentLayout.sidePanelWidth : sidePanelWidth;
  renderedPreviewPanelWidthRef.current = effectivePreviewPanelWidth;

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

  useEffect(() => {
    if (previewCloseConfirm && previewTabs.length === 0) {
      setPreviewCloseConfirm(null);
    }
  }, [previewCloseConfirm, previewTabs.length]);

  // line 由聊天正文的文件链接(foo.cpp:42)带入,预览打开后滚动到该行并高亮;
  // 文件树等其它入口不带 line,走原「只打开」语义。
  const openFilePreview = useCallback((path, line = null) => {
    const location = previewFileLocation({ cwd: sidePanelCwd, path });
    if (!sid || !previewScope || !location.cwd || !location.path) return;
    // 侧栏折叠时开预览 tab 也不会显示(previewPanelVisible 依赖非折叠),先展开。
    if (sidePanelCollapsed) onToggleSidePanel?.();
    setPreviewTabState((prev) => openFileTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      cwd: location.cwd,
      path: location.path,
      line,
    }));
  }, [previewScope, sid, sidePanelCwd, sidePanelCollapsed, onToggleSidePanel]);

  const openSessionChangePreview = useCallback((filePath) => {
    if (!sid || !filePath) return;
    if (sidePanelCollapsed) onToggleSidePanel?.();
    setPreviewTabState((prev) => openSessionChangesTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      expandedFile: filePath,
      fileCount: changeSummary.fileCount,
    }));
  }, [changeSummary.fileCount, onToggleSidePanel, previewScope, sid, sidePanelCollapsed]);

  // git 变更点击文件 → 在中间详情栏开/聚焦「变更」页签(复刻会话级变更旧行为)。
  // gitBase 只有从 SidePanel 导航列表点击时才带;详情栏内点文件不带,由
  // openGitChangesTab 保留页签原 base。
  const openGitChangePreview = useCallback((filePath, gitBase, gitFileCount) => {
    if (!sid || !filePath) return;
    if (sidePanelCollapsed) onToggleSidePanel?.();
    setPreviewTabState((prev) => openGitChangesTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      cwd: sidePanelCwd,
      base: gitBase,
      expandedFile: filePath,
      fileCount: gitFileCount,
    }));
  }, [onToggleSidePanel, previewScope, sid, sidePanelCollapsed, sidePanelCwd]);

  // SidePanel 切基线时,若「变更」页签已打开则同步其 base(详情栏跟着换比较对象)。
  const updateGitChangeBase = useCallback((gitBase) => {
    if (!sid) return;
    setPreviewTabState((prev) => updateGitChangesTab(prev, { sessionId: sid, base: gitBase || '' }));
  }, [sid]);

  const activatePreview = useCallback((tabKey) => {
    setPreviewTabState((prev) => activatePreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
  }, [previewScope, sid]);

  const refreshPreview = useCallback((tabKey) => {
    setPreviewTabState((prev) => refreshPreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
  }, [previewScope, sid]);

  useEffect(() => {
    const transition = nextAutoPreviewRefresh(previewAutoRefreshRef.current, {
      sid,
      busy,
      turnKey: lastUserTurnKey,
      activeTab: activePreview,
      changedPaths: latestSuccessfulChangedFiles,
    });
    previewAutoRefreshRef.current = transition.state;
    if (transition.tabKey) refreshPreview(transition.tabKey);
  }, [activePreview, busy, lastUserTurnKey, latestSuccessfulChangedFiles, refreshPreview, sid]);

  const closePreview = useCallback((tabKey) => {
    const closingLastVisibleTab = previewTabs.length <= 1;
    setPreviewTabState((prev) => closePreviewTab(prev, {
      scopeKey: previewScope,
      sessionId: sid,
      tabKey,
    }));
    if (closingLastVisibleTab && sidePanelMaximized) onToggleSidePanelMaximized?.();
  }, [onToggleSidePanelMaximized, previewScope, previewTabs.length, sid, sidePanelMaximized]);

  const closePreviewPanelConfirmed = useCallback(() => {
    setPreviewCloseConfirm(null);
    setPreviewTabState((prev) => closeVisiblePreviewTabs(prev, {
      scopeKey: previewScope,
      sessionId: sid,
    }));
    if (sidePanelMaximized) onToggleSidePanelMaximized?.();
  }, [onToggleSidePanelMaximized, previewScope, sid, sidePanelMaximized]);

  const closePreviewPanel = useCallback(() => {
    const confirmMessage = closeVisiblePreviewTabsConfirmationMessage(previewTabs.length);
    if (confirmMessage) {
      setPreviewCloseConfirm({ message: confirmMessage });
      return;
    }
    closePreviewPanelConfirmed();
  }, [closePreviewPanelConfirmed, previewTabs.length]);

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
    const homeProjectTitle = selectedHomeWorkspace?.noWorkspace
      ? '我们该做什么？'
      : `我们该在 ${homeProjectName} 中做什么？`;
    const homeProjectLabel = homeWorkspaces.find(w => w.hash === homeWorkspaceHash)?.name
      || selectedHomeWorkspace?.name
      || '当前项目';
    const homeHints = [
      { icon: 'edit', title: '编辑代码', desc: '让 Agent 帮你重构、修 bug、加测试' },
      { icon: 'searchSparkle', title: '探索代码库', desc: '问“这个函数在哪里被调用”' },
      { icon: 'run', title: '运行命令', desc: 'bash / npm / git 等 Agent 会逐步确认' },
      { icon: 'lightbulb', title: '使用 Skills', desc: '预定义工作流，从侧边栏开启' },
    ];
    return (
      <div className="flex-1 min-w-0 flex flex-col bg-bg">
        <div className="ace-home-panel flex-1">
          <div className="ace-home-content">
            <img src="/acecode-logo.png" alt="ACECode" width="64" height="64" className="ace-home-logo select-none" draggable="false" />
            <h1 className="ace-home-title">{homeProjectTitle}</h1>
            <div data-tour-target="home-composer" className="ace-home-composer">
              <InputBar
                ref={inputRef}
                variant="hero"
                pathReferenceApi={api}
                cwd={selectedHomeWorkspace?.cwd || ''}
                history={composerHistory}
                onSubmit={submit}
                disabled={!!questionForView || homeSubmitting}
                placeholder="向 ACECode 描述任务，或输入 / 命令..."
                {...composerInputProps}
                sessionControls={{
                  model: homeModelLabel,
                  modelOptions,
                  selectedModelName: homeModelName,
                  modelLoad: homeModelLoad,
                  modelSwitching,
                  modelRefreshing,
                  onModelChange: changeComposerModel,
                  onRefreshModels: refreshSessionModels,
                  onOpenModelSettings,
                  tokenBudget: homeTokenBudget,
                  permissionMode,
                  permissionSwitching,
                  onPermissionModeChange: changeComposerPermissionMode,
                  expertOptions: experts,
                  selectedExpertId: homeExpertId,
                  onExpertChange: setHomeExpertId,
                  expertLocked: false,
                }}
              />
            </div>
            <div className="flex items-center gap-2 mr-auto ml-0">
            <div className="relative">
              <button
                data-tour-target="home-workspace"
                type="button"
                className="ace-home-project-row group"
                onClick={() => setProjectDropdownOpen(!projectDropdownOpen)}
                title={selectedHomeWorkspace?.cwd || homeProjectName}
              >
                <VsIcon name="folder" size={15} />
                <span className="text-fg-mute">项目</span>
                <span className="ace-home-project-select truncate">
                  {homeProjectLabel}
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
                    <button
                      type="button"
                      className="group w-full h-8 px-3 text-left text-[13px] flex items-center gap-2 text-fg hover:bg-surface-hi transition-colors"
                      onClick={() => {
                        setProjectDropdownOpen(false);
                        setCreateProjectOpen(true);
                      }}
                    >
                      <VsIcon name="folderAdd" size={15} className="text-fg-mute group-hover:text-fg shrink-0" />
                      <span className="truncate">新建项目</span>
                    </button>
                    <button
                      type="button"
                      className="group w-full h-8 px-3 text-left text-[13px] flex items-center gap-2 text-fg hover:bg-surface-hi transition-colors"
                      onClick={handleOpenExistingDirectory}
                    >
                      <VsIcon name="folderOpen" size={15} className="text-fg-mute group-hover:text-fg shrink-0" />
                      <span className="truncate">打开现有目录</span>
                    </button>
                    <div className="my-1 border-t border-border/50" aria-hidden="true" />
                    {homeWorkspaces.map((w) => (
                      <button
                        key={w.hash || w.cwd || w.name}
                        type="button"
                        className={clsx(
                          "w-full text-left px-3 py-1.5 text-[13px] flex flex-col gap-[2px] transition-colors",
                          w.hash === homeWorkspaceHash ? "bg-accent/10 text-accent font-medium" : "text-fg hover:bg-surface-hi"
                        )}
                        onClick={() => selectHomeWorkspace(w)}
                      >
                        <div className="truncate leading-tight">{w.name}</div>
                        <div className={clsx("text-[10.5px] truncate leading-tight", w.hash === homeWorkspaceHash ? "text-accent/60" : "text-fg-mute/70")} title={w.cwd}>
                          {w.cwd.replace(/\\/g, '/')}
                        </div>
                      </button>
                    ))}
                    <div className="my-1 border-t border-border/50" aria-hidden="true" />
                    <button
                      type="button"
                      className={clsx(
                        "w-full text-left px-3 py-2 text-[13px] transition-colors",
                        !homeWorkspaceHash ? "bg-accent/10 text-accent font-medium" : "text-fg hover:bg-surface-hi"
                      )}
                      onClick={() => selectHomeWorkspace(noHomeWorkspaceOption())}
                    >
                      <div className="truncate leading-tight">{noHomeWorkspaceOption().name}</div>
                    </button>
                  </div>
                </>
              )}
            </div>
            <GitSessionPill
              key={`home-${homeWorkspaceHash}`}
              api={api}
              cwd={selectedHomeWorkspace?.cwd || ''}
              variant="hero"
              busy={homeSubmitting}
              onIntentChange={handleGitPillIntentChange}
            />
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
        {createProjectOpen && (
          <CreateProjectModal
            api={api}
            onClose={() => setCreateProjectOpen(false)}
            onCreated={handleProjectCreated}
          />
        )}
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

      if (child.kind === 'subagent_group') {
        return (
          <div
            key={key}
            className="flex flex-col"
            data-chat-kind={child.kind || ''}
            data-chat-role="subagent_group"
          >
            <SubagentGroupBlock
              agents={child.agents}
              tasksById={subagentTasksById}
              onOpen={openSubagentTranscript}
            />
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
            <ToolBlock
              entry={child.tool}
              onReviewToggle={pauseTailFollowForReview}
              sessionRunning={status === 'running'}
            />
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
              onOpenFilePreview={openFilePreview}
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
    width: sidePanelNavigationCollapsed ? 0 : Math.max(0, effectiveSidePanelWidth),
  };
  const sidePanelRestoreAction = sidePanelCollapsed && onToggleSidePanel
    ? { onClick: onToggleSidePanel, label: '展开整个右侧面板' }
    : sidePanelListCollapsed && !previewPanelVisible && onToggleSidePanelList
      ? { onClick: onToggleSidePanelList, label: '展开列表面板' }
      : null;

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
          {sid && onFindInConversation && (
            <button
              type="button"
              onClick={onFindInConversation}
              className="w-6 h-6 rounded-md text-fg-mute flex items-center justify-center shrink-0 transition hover:bg-surface-hi hover:text-fg focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
              title="搜索当前对话内容 (Ctrl+F)"
              aria-label="搜索当前对话内容"
            >
              <VsIcon name="search" size={14} />
            </button>
          )}
          {sid && (
            <LspIndicator
              api={api}
              cwd={ref?.cwd || health?.cwd || ''}
              refreshKey={`${turns}:${busy ? 1 : 0}`}
            />
          )}
          {sid && !readOnlyExternalSession && (
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
          {sidePanelMounted && sidePanelRestoreAction && (
            <button
              type="button"
              onClick={sidePanelRestoreAction.onClick}
              className="ace-side-panel-expand-fab"
              title={sidePanelRestoreAction.label}
              aria-label={sidePanelRestoreAction.label}
            >
              <PanelToggleIcon side="right" size={15} />
            </button>
          )}
          {sid && (subagentTasks.tasks.length > 0 || subagentPanelOpen) && (
            <button
              type="button"
              onClick={() => setSubagentPanelOpen((v) => !v)}
              className={clsx(
                'relative w-7 h-7 rounded-md flex items-center justify-center transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25',
                subagentPanelOpen || subagentTasks.runningCount > 0
                  ? 'bg-accent-bg text-accent hover:bg-accent-bg'
                  : 'text-fg-mute hover:bg-surface-hi hover:text-fg',
              )}
              title="后台任务"
              aria-label="后台任务"
            >
              <VsIcon name="embedding" size={15} />
              {subagentTasks.runningCount > 0 && (
                <span className="absolute -top-1 -right-1 min-w-[14px] h-[14px] px-0.5 rounded-full bg-accent text-white text-[9px] leading-[14px] text-center font-semibold">
                  {subagentTasks.runningCount}
                </span>
              )}
            </button>
          )}
        </div>
      </div>

      <div className="relative flex-1 min-h-0 flex">
        <div className="relative flex-1 min-w-0 h-full">
        <div
          ref={scrollRef}
          data-conversation-find-root="true"
          onScroll={handleMessagesScroll}
          onWheel={handleMessagesWheel}
          onPointerDown={handleMessagesPointerDown}
          className="ace-chat-transcript-scroll h-full overflow-y-auto pl-[35px] pr-3.5 py-3"
          style={changeDockBottomPadding > 0 ? { paddingBottom: changeDockBottomPadding } : undefined}
        >
          <div ref={transcriptContentRef} className="flex flex-col gap-3">
          {renderedItems.map((it) => {
            if (it.kind === 'termination_notice') {
              return (
                <div
                  key={it.id}
                  className={chatRowClassName(it)}
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
                  className={chatRowClassName(it)}
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role="completion_summary"
                >
                  <CompletionSummaryBlock item={it} />
                </div>
              );
            }

            if (it.kind === 'subagent_group') {
              return (
                <div
                  key={it.id}
                  className={chatRowClassName(it)}
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role="subagent_group"
                >
                  <SubagentGroupBlock
                    agents={it.agents}
                    tasksById={subagentTasksById}
                    onOpen={openSubagentTranscript}
                  />
                </div>
              );
            }

            if (it.kind === 'activity_summary') {
              const expanded = expandedActivityKeys.has(it.id);
              const detailItems = it.detailItems || it.collapsedItems || [];
              return (
                <Fragment key={it.id}>
                  <div
                    className={chatRowClassName(it)}
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
            const turnFileSetsBefore = it.kind === 'msg' && it.role === 'user'
              ? (turnFileListPlacement.before.get(it.id) || [])
              : [];
            return (
              <Fragment key={it.id}>
                {turnFileSetsBefore.map((set) => (
                  <div
                    key={`turn-files-${set.userItemId || 'head'}`}
                    className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
                    data-chat-row="true"
                    data-chat-kind="turn_files"
                  >
                    <TurnFileList
                      groups={set.groups}
                      summary={set.summary}
                      cwd={sidePanelCwd}
                      onOpenFile={openSessionChangePreview}
                    />
                  </div>
                ))}
                <div
                  className={chatRowClassName(it)}
                  data-chat-row="true"
                  data-chat-item-id={String(it.id)}
                  data-chat-kind={it.kind || ''}
                  data-chat-role={it.kind === 'msg' ? (it.role || '') : (it.kind || '')}
                  data-chat-user-message={it.kind === 'msg' && it.role === 'user' ? 'true' : undefined}
                  data-chat-message-ordinal={it.kind === 'msg' && it.messageOrdinal != null ? String(it.messageOrdinal) : undefined}
                  data-chat-assistant-continuation={continuation ? 'true' : undefined}
                  {...messageContextAttrs(it)}
                >
                  {it.kind === 'tool' ? (
                    <ToolBlock
                      entry={it.tool}
                      onReviewToggle={pauseTailFollowForReview}
                      sessionRunning={status === 'running'}
                    />
                  ) : (
                    <Message
                      role={it.role} content={it.content} ts={it.ts}
                      contentParts={it.contentParts}
                      streaming={it.streaming}
                      messageId={it.messageId}
                      metadata={it.metadata}
                      onFork={forkAndSwitch}
                      onOpenFilePreview={openFilePreview}
                      continuation={continuation}
                      showFooter={showFooter}
                      showAceCodeAvatar={showAceCodeAvatar}
                    />
                  )}
                </div>
              </Fragment>
            );
          })}
          {/* tail = 当前最后一轮的文件列表。回合进行中(busy)不渲染 ——
              流式期间变更集随 tool_end 实时增长,列表会先于/夹着正文出现,
              观感突兀;等整轮吐完(busy 结束)再一次性显示在正文之后。
              历史回合(before 锚定)不受影响。 */}
          {!busy && turnFileListPlacement.tail.map((set) => (
            <div
              key={`turn-files-${set.userItemId || 'head'}`}
              className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
              data-chat-row="true"
              data-chat-kind="turn_files"
            >
              <TurnFileList
                groups={set.groups}
                summary={set.summary}
                cwd={sidePanelCwd}
                onOpenFile={openSessionChangePreview}
              />
            </div>
          ))}
          {permissionRequests.map((request) => (
            <div
              key={`permission-${request.request_id}`}
              className="ace-chat-row flex flex-col ace-chat-row-assistant-gutter"
              data-chat-row="true"
              data-chat-item-id={`permission-${request.request_id}`}
              data-chat-kind="permission"
            >
              <PermissionCard
                request={request}
                originLabel={request.origin_label || ''}
                onDecision={onPermissionDecision}
              />
            </div>
          ))}
          {conversationActivity.kind !== CONVERSATION_ACTIVITY_KIND.IDLE && (
            <ActivityIndicator
              activity={conversationActivity}
              showAceCodeAvatar={showAceCodeAvatar}
            />
          )}
          </div>
        </div>
        {showConversationTurnScrubber && (
          <ConversationTurnScrubberBoundary key={sid}>
            <Suspense fallback={null}>
              <LazyConversationTurnScrubber
                turns={preparedConversationTurns}
                activeIndex={activeConversationTurn}
                onJump={jumpToConversationTurn}
              />
            </Suspense>
          </ConversationTurnScrubberBoundary>
        )}
        <StickyUserContext context={stickyUserContext} onJumpToSource={jumpToStickyUserSource} />
        {showChangeDock && (
          <ChangeGlassDock
            dockRef={changeDockRef}
            summary={changeSummary}
            showChanges={showChangeDetails}
            onReview={openReviewPanel}
            onDismiss={dismissChangeDock}
            todos={dockTodos}
            todoSummary={dockTodoSummary}
          />
        )}
        </div>
        <SubagentPanel
          open={subagentPanelOpen}
          focus={subagentFocus}
          onClose={() => setSubagentPanelOpen(false)}
          tasks={subagentTasks.tasks}
          onAbort={(task) => subagentTasks.abortTask(task.id)}
          onClearSettled={async () => {
            const result = await subagentTasks.clearSettled();
            if (result?.failed > 0) {
              toast({ kind: 'err', text: `有 ${result.failed} 个任务清除失败(可能仍在运行)` });
            }
          }}
        />
      </div>

      {questionForView && (
        <QuestionPicker
          request={questionForView}
          onResolve={resolveQuestion}
          originLabel={questionOriginLabel}
        />
      )}

      <SideQuestionCard
        state={sideQuestion}
        onDismiss={() => setSideQuestion(null)}
      />
      <QueueCardList
        items={visibleQueuedItems}
        onCancel={cancelQueued}
        onRetry={retryQueued}
        onGuide={guideQueued}
        guideDisabled={!busy || !activeTurnId}
      />
      {readOnlyExternalSession ? (
        <div
          role="status"
          className="min-h-11 shrink-0 border-t border-border bg-surface px-4 py-2.5 text-[12px] leading-5 text-fg-mute"
        >
          {tr('externalSession.tuiReadOnly')}
        </div>
      ) : (
        <>
          <InputBar
            ref={inputRef}
            pathReferenceApi={api}
            cwd={ref?.cwd || health?.cwd || ''}
            busy={busy}
            goal={goal}
            goalStopping={goalStopping}
            history={composerHistory}
            value={composerValue}
            onChange={handleComposerChange}
            onSubmit={submit}
            onAbort={stopCurrentWork}
            {...composerInputProps}
            disabled={!!questionForView || composerSubmitting}
            placeholder={questionForView ? '请先回答上方问题…' : undefined}
            sessionControls={{
              model: currentModelLabel,
              modelOptions: displayedModelOptions,
              selectedModelName: currentModelName,
              modelLoad: currentModelLoad,
              modelSwitching,
              modelRefreshing,
              onModelChange: changeComposerModel,
              onRefreshModels: refreshSessionModels,
              onOpenModelSettings,
              tokenBudget,
              permissionMode,
              permissionSwitching,
              onPermissionModeChange: changeComposerPermissionMode,
              expertOptions: displayedExperts,
              selectedExpertId: boundExpertId,
              expertLocked: true,
            }}
          />
          <GitSessionPill
            key={`session-${sid}`}
            api={api}
            cwd={ref?.cwd || health?.cwd || ''}
            variant="bar"
            sessionStarted={rawItems.length > 0}
            worktreeSession={localWorktree && localWorktree.sid === sid
              ? { name: localWorktree.name }
              : (ref?.worktree || null)}
            busy={busy}
            onIntentChange={handleGitPillIntentChange}
          />
        </>
      )}
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
            busy={busy}
            sidePanelListCollapsed={sidePanelListCollapsed}
            onActivateTab={activatePreview}
            onCloseTab={closePreview}
            onCloseOthers={closeOtherPreviews}
            onCloseToRight={closePreviewsToRight}
            onCloseAll={closePreviewPanel}
            onRefreshTab={refreshPreview}
            onReorderTab={reorderPreview}
            onToggleMaximize={onToggleSidePanelMaximized}
            onToggleSidePanelList={onToggleSidePanelList}
            onSelectChangeFile={openSessionChangePreview}
            onSelectGitChangeFile={openGitChangePreview}
            onOpenFilePreview={openFilePreview}
          />
        </div>
      )}
      {previewCloseConfirm && (
        <Modal onClose={() => setPreviewCloseConfirm(null)} width={440}>
          {({ close }) => (
            <div className="p-4">
              <div className="text-[14px] font-semibold mb-2">关闭预览面板</div>
              <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">
                {previewCloseConfirmMessage}
              </div>
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors"
                  onClick={close}
                >
                  取消
                </button>
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity"
                  onClick={closePreviewPanelConfirmed}
                >
                  关闭
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
      {sidePanelMounted && (
        <>
          {!sidePanelNavigationCollapsed && (
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
            data-collapsed={sidePanelNavigationCollapsed ? 'true' : 'false'}
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
              filesEnabled={sidePanelFilesEnabled}
              width={sidePanelWidth}
              collapsed={sidePanelListCollapsed}
              busy={busy}
              onToggleCollapse={onToggleSidePanelList}
              onOpenFilePreview={openFilePreview}
              onOpenSessionChangePreview={openSessionChangePreview}
              onOpenGitChangePreview={openGitChangePreview}
              onGitBaseChange={updateGitChangeBase}
              selectedChangeFile={selectedChangeFile}
              selectedChangeFileRevision={selectedChangeFileRevision}
              selectedGitChangeFile={selectedGitChangeFile}
            />
          </div>
        </>
      )}
    </div>
  );
}
