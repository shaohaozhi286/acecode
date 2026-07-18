// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 270px Sidebar + 主区(单会话/4宫格/9宫格)
// 会话控制内嵌在聊天输入框。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api, ApiError, createApi } from './lib/api.js';
import { setToken } from './lib/auth.js';
import { connection } from './lib/connection.js';
import {
  createDesktopNotificationMonitor,
  notificationEventKey,
} from './lib/desktopNotificationMonitor.js';
import {
  maybeNotify,
  notificationBodyFromEvent,
} from './lib/desktopNotify.js';
import { createNewSessionForActiveWorkspace } from './lib/newSession.js';
import { goBack, goForward, pushNavigation } from './lib/navigationHistory.js';
import {
  addPendingQuestionRequest,
  pendingQuestionSessionIds,
  removePendingQuestionRequest,
} from './lib/pendingQuestions.js';
import { sessionDisplayTitle } from './lib/sessionTitle.js';
import { notifySessionListChanged } from './lib/sessionListEvents.js';
import { usePreference } from './lib/usePreference.js';
import {
  DEFAULT_UI_PREFS,
  DEFAULT_FONT_SIZE,
  effectiveFontSize,
  effectiveSidePanelListCollapsed,
  FONT_SIZE_VALUES,
  UI_PREFS_STORAGE_KEY,
  validateUiPrefs,
} from './lib/uiPrefs.js';
import { useGlobalShortcut } from './lib/useGlobalShortcut.js';
import { TopBar } from './components/TopBar.jsx';
import { Sidebar } from './components/Sidebar.jsx';
import { ChatView } from './components/ChatView.jsx';
import { SearchPalette } from './components/SearchPalette.jsx';
import { TokenPrompt } from './components/TokenPrompt.jsx';
import { SettingsPage } from './components/SettingsPage.jsx';
import { DesktopContextMenu } from './components/DesktopContextMenu.jsx';
import { Toaster, toast } from './components/Toast.jsx';
import { SlashCommandsProvider } from './components/SlashCommandsContext.jsx';
import { FramelessResizeHandles } from './components/FramelessResizeHandles.jsx';
import { GlobalFindOverlay } from './components/GlobalFindOverlay.jsx';
import { canOpenConversationFind } from './lib/globalFind.js';
import { ConsoleDock } from './components/ConsoleDock.jsx';
import { DesktopGuidedTour } from './components/DesktopGuidedTour.jsx';
import { UpdateDialog } from './components/UpdateDialog.jsx';
import { LoopPage } from './components/LoopPage.jsx';
import {
  CONSOLE_DOCK_DEFAULT_HEIGHT,
  clampDockHeight,
  consoleCwdForContext,
} from './lib/consoleDock.js';
import { homeRefFromWorkspace, noHomeWorkspaceOption } from './lib/homeWorkspaceSelection.js';
import {
  DEFAULT_SINGLE_LAYOUT,
  LEGACY_DEFAULT_SINGLE_LAYOUT,
  normalizePreviewPanelWidth,
  normalizeSidePanelWidth,
  normalizeSidebarWidth,
  validateLayoutWidths,
} from './lib/singleLayout.js';
import { initInactiveSelection } from './lib/inactiveSelection.js';
import {
  desktopOpenSessionUrl,
  openSessionTargetFromSearch,
  sessionJumpId,
  sessionJumpMessageOrdinal,
  sessionJumpNoWorkspace,
  sessionJumpWorkspaceHash,
  sessionRefFromJumpTarget,
  stripOpenSessionParams,
} from './lib/sessionJump.js';
import { desktopUiMode } from './lib/desktopShellMode.js';
import { shouldAutoFocusDesktopComposer } from './lib/composerCaretRestore.js';
import { requestDesktopAppExit, showDesktopAboutDialog } from './lib/desktopAppActions.js';
import {
  desktopUpdateRestartAvailable,
  requestDesktopUpdateRestart,
  updateJobIsActive,
} from './lib/updateJob.js';
import {
  clearResolvedPermissionRequests,
  closePermissionRequest,
  hasUnresolvedPermission,
  markPermissionSubmitting,
  pendingPermissionSessionIds,
  permissionOriginLabel,
  pushPermissionRequest,
  visiblePermissionRequests,
} from './lib/permissionRequestQueue.js';
import {
  desktopGuidedTourHasModel,
  desktopGuidedTourModeEligible,
  desktopGuidedTourTargetsReady,
  shouldAutoStartDesktopGuidedTour,
  shouldPrepareDesktopGuidedTour,
} from './lib/desktopGuidedTour.js';

const SINGLE_LAYOUT_STORAGE_KEY = 'acecode.singleLayoutWidths.v1';

// 控制台停靠区偏好(add-console-dock):开关 + 高度跨刷新持久化。
const CONSOLE_DOCK_STORAGE_KEY = 'acecode.consoleDock.v1';
const DEFAULT_CONSOLE_DOCK = { open: false, height: CONSOLE_DOCK_DEFAULT_HEIGHT };
function validateConsoleDock(value) {
  return !!value && typeof value === 'object'
    && typeof value.open === 'boolean'
    && Number.isFinite(value.height);
}

function parseDesktopBridgeResult(raw) {
  if (!raw) return null;
  if (typeof raw === 'string') return JSON.parse(raw);
  return raw;
}

export function App() {
  const [authState, setAuthState] = useState('checking'); // 'checking' | 'ok' | 'need-token'
  const [health,    setHealth]    = useState(null);

  const [activeRef,    setActiveRef]    = useState(null);
  const [navHistory, setNavHistory] = useState({ back: [], forward: [] });
  const [commandWorkspaceHash, setCommandWorkspaceHash] = useState('');
  const [consoleCwd, setConsoleCwd] = useState('');
  const [showSettings, setShowSettings] = useState(false);
  const [settingsNavKey, setSettingsNavKey] = useState('general');
  const [permReqs,     setPermReqs]     = useState([]);
  const [questionReqs, setQuestionReqs] = useState([]);
  // 当前主会话的后台任务(spawn_subagent 子会话)索引,由 ChatView 上报。
  // 用于:1) 子任务的 question_request 在主会话可见;2) 权限/问题弹窗的
  // 「来自后台任务」来源标记。
  const [subagentIndex, setSubagentIndex] = useState({ parentId: '', titles: {} });
  // 已经发现过的 child -> parent/title 关系跨会话切换保留,供后台权限请求
  // 在父会话不活跃时仍能把侧边栏提示和系统通知路由到父会话。
  const [subagentDirectory, setSubagentDirectory] = useState({ owners: {}, titles: {} });
  const [searchOpen,   setSearchOpen]   = useState(false);
  const [conversationFindRequest, setConversationFindRequest] = useState(0);
  const [workspaceActivationRequest, setWorkspaceActivationRequest] = useState(null);
  const [updateStatus, setUpdateStatus] = useState(null);
  const [updateStarting, setUpdateStarting] = useState(false);
  const [updateChecking, setUpdateChecking] = useState(false);
  const [updateRestarting, setUpdateRestarting] = useState(false);
  const [updateJob, setUpdateJob] = useState(null);
  const [updateDialogOpen, setUpdateDialogOpen] = useState(false);
  const [singleLayout, setSingleLayout] = usePreference(
    SINGLE_LAYOUT_STORAGE_KEY, DEFAULT_SINGLE_LAYOUT, validateLayoutWidths);
  const [uiPrefs, setUiPrefs] = usePreference(
    UI_PREFS_STORAGE_KEY, DEFAULT_UI_PREFS, validateUiPrefs);
  const [consoleDock, setConsoleDock] = usePreference(
    CONSOLE_DOCK_STORAGE_KEY, DEFAULT_CONSOLE_DOCK, validateConsoleDock);
  // grid4/grid9 入口暂时隐藏:主界面固定单会话,避免旧 localStorage 把用户卡在未完善视图。
  const view = 'single';
  const fontSize = effectiveFontSize(uiPrefs);
  // sidePanelCollapsed 是列表 + 详情的总开关;listCollapsed 只隐藏最右导航列表。
  const sidePanelCollapsed = uiPrefs.sidePanelCollapsed;
  const sidePanelListCollapsed = effectiveSidePanelListCollapsed(uiPrefs);
  const sidePanelNavigationCollapsed = sidePanelCollapsed || sidePanelListCollapsed;
  const sidePanelMaximized = !!uiPrefs.sidePanelMaximized;
  const projectSidebarCollapsed = !!uiPrefs.sidebarCollapsed;
  const showAceCodeAvatar = false;
  const singleShellRef = useRef(null);
  const sidebarResizeActiveRef = useRef(false);
  const [previewPanelVisible, setPreviewPanelVisible] = useState(false);
  const activeRefRef = useRef(activeRef);
  const healthRef = useRef(health);
  const visibleSessionRef = useRef(null);
  const subagentIndexRef = useRef(subagentIndex);
  const subagentDirectoryRef = useRef(subagentDirectory);
  const notificationContextRef = useRef({
    assistantText: new Map(),
    titles: new Map(),
    workspaces: new Map(),
  });
  const notificationMonitorRef = useRef(null);
  if (!notificationMonitorRef.current) {
    notificationMonitorRef.current = createDesktopNotificationMonitor({
      retainSession: (sessionId) => connection.retainSession(sessionId),
      releaseSession: (sessionId) => connection.releaseSession(sessionId),
    });
  }
  const navHistoryRef = useRef(navHistory);
  const updatePollRef = useRef(0);
  const desktopModeRef = useRef(desktopUiMode());
  const startupOpenTargetRef = useRef(
    typeof window === 'undefined' ? null : openSessionTargetFromSearch(window.location.search),
  );
  const startupNavigationStartedRef = useRef(false);
  const [startupNavigationSettled, setStartupNavigationSettled] = useState(
    () => !startupOpenTargetRef.current,
  );
  const [guidedTourState, setGuidedTourState] = useState({
    loaded: false,
    dismissed: true,
    hasModel: true,
  });
  const [guidedTourPreparing, setGuidedTourPreparing] = useState(false);
  const [guidedTourRun, setGuidedTourRun] = useState(false);
  const [guidedTourForced, setGuidedTourForced] = useState(false);
  const guidedTourAutoAttemptedRef = useRef(false);
  const guidedTourHasActiveSession = !!(activeRef?.sessionId || activeRef?.id);
  const guidedTourBlocked = showSettings || searchOpen || updateDialogOpen
    || questionReqs.length > 0;

  useEffect(() => initInactiveSelection(), []);
  useEffect(() => {
    document.documentElement.setAttribute('data-font-size', fontSize);
  }, [fontSize]);
  useEffect(() => { activeRefRef.current = activeRef; }, [activeRef]);
  useEffect(() => { healthRef.current = health; }, [health]);
  useEffect(() => { subagentIndexRef.current = subagentIndex; }, [subagentIndex]);
  useEffect(() => { subagentDirectoryRef.current = subagentDirectory; }, [subagentDirectory]);
  useEffect(() => {
    const sessionId = activeRef?.sessionId || activeRef?.id || '';
    const chatVisible = view === 'single'
      && !!sessionId
      && !activeRef?.loop
      && !showSettings
      && !searchOpen
      && !updateDialogOpen
      && !guidedTourPreparing
      && !guidedTourRun;
    visibleSessionRef.current = chatVisible
      ? { sessionId, workspaceHash: activeRef?.workspaceHash || '' }
      : null;
  }, [
    activeRef?.id,
    activeRef?.loop,
    activeRef?.sessionId,
    activeRef?.workspaceHash,
    guidedTourPreparing,
    guidedTourRun,
    searchOpen,
    showSettings,
    updateDialogOpen,
    view,
  ]);
  useEffect(() => { navHistoryRef.current = navHistory; }, [navHistory]);

  const replaceActiveRef = useCallback((nextRefOrUpdater) => {
    const current = activeRefRef.current;
    const next = typeof nextRefOrUpdater === 'function'
      ? nextRefOrUpdater(current)
      : nextRefOrUpdater;
    activeRefRef.current = next;
    setActiveRef(next);
  }, []);

  const navigateToRef = useCallback((nextRefOrUpdater) => {
    const current = activeRefRef.current;
    const next = typeof nextRefOrUpdater === 'function'
      ? nextRefOrUpdater(current)
      : nextRefOrUpdater;
    const nextHistory = pushNavigation(navHistoryRef.current, current, next);
    navHistoryRef.current = nextHistory;
    activeRefRef.current = next;
    setNavHistory(nextHistory);
    setActiveRef(next);
  }, []);

  const goBackActiveRef = useCallback(() => {
    const result = goBack(navHistoryRef.current, activeRefRef.current);
    navHistoryRef.current = result.history;
    activeRefRef.current = result.activeRef;
    setNavHistory(result.history);
    setActiveRef(result.activeRef);
  }, []);

  const goForwardActiveRef = useCallback(() => {
    const result = goForward(navHistoryRef.current, activeRefRef.current);
    navHistoryRef.current = result.history;
    activeRefRef.current = result.activeRef;
    setNavHistory(result.history);
    setActiveRef(result.activeRef);
  }, []);

  const resumeAndOpenSession = useCallback(async (target, options = {}) => {
    const sessionId = sessionJumpId(target);
    if (!sessionId) return false;
    const noWorkspace = sessionJumpNoWorkspace(target);
    const targetHash = sessionJumpWorkspaceHash(target);
    const shouldResume = options.forceResume || target?.active !== true;
    const commitRef = options.replace ? replaceActiveRef : navigateToRef;
    const resumeWith = async (client, workspaceHash) => {
      if (!shouldResume) return {};
      if (noWorkspace || !workspaceHash) return client.resumeSession(sessionId);
      return client.resumeWorkspaceSession(workspaceHash, sessionId);
    };

    if (!noWorkspace
        && targetHash
        && options.allowDesktopActivate !== false
        && targetHash !== activeRefRef.current?.workspaceHash
        && typeof window !== 'undefined'
        && typeof window.aceDesktop_activateWorkspace === 'function') {
      try {
        const r = parseDesktopBridgeResult(await window.aceDesktop_activateWorkspace(targetHash));
        if (r && !r.error && r.port && r.token) {
          let resumed = {};
          try {
            resumed = await resumeWith(createApi({ port: r.port, token: r.token }), targetHash);
          } catch (e) {
            toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
            return false;
          }
          const url = desktopOpenSessionUrl({
            port: r.port,
            token: r.token,
            sessionId,
            workspaceHash: targetHash,
            messageOrdinal: sessionJumpMessageOrdinal(target),
            protocol: window.location?.protocol || 'http:',
          });
          if (url) {
            window.location.href = url;
            return true;
          }
          commitRef(sessionRefFromJumpTarget(target, resumed, { workspaceHash: targetHash }));
          return true;
        }
      } catch {
        // Bridge 不可用或返回格式异常时，降级到当前 daemon 的 workspace-scoped resume。
      }
    }

    try {
      const resumed = await resumeWith(api, targetHash);
      commitRef(sessionRefFromJumpTarget(target, resumed, {
        workspaceHash: targetHash,
        noWorkspace,
      }));
      return true;
    } catch (e) {
      toast({ kind: 'err', text: '恢复失败:' + (e.message || '') });
      return false;
    }
  }, [navigateToRef, replaceActiveRef]);

  const openSettingsSection = useCallback((key = 'general') => {
    setSettingsNavKey(key || 'general');
    setShowSettings(true);
  }, []);

  useEffect(() => {
    setSingleLayout((prev) => {
      if (prev?.sidebar === LEGACY_DEFAULT_SINGLE_LAYOUT.sidebar
          && prev?.sidePanel === LEGACY_DEFAULT_SINGLE_LAYOUT.sidePanel) {
        return DEFAULT_SINGLE_LAYOUT;
      }
      if (prev && prev.previewPanel == null) {
        return { ...prev, previewPanel: DEFAULT_SINGLE_LAYOUT.previewPanel };
      }
      return prev;
    });
  }, [setSingleLayout]);

  const probe = useCallback(async () => {
    try {
      const h = await api.health();
      setHealth(h);
      setAuthState('ok');
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) {
        setAuthState('need-token');
      } else {
        toast({ kind: 'err', text: '连接 daemon 失败:' + e.message });
        setAuthState('need-token');
      }
    }
  }, []);

  useEffect(() => { probe(); }, [probe]);

  const pollUpdateJob = useCallback((jobId) => {
    if (!jobId) return;
    if (updatePollRef.current) window.clearTimeout(updatePollRef.current);
    let failures = 0;
    const tick = async () => {
      try {
        const job = await api.getUpdateJob(jobId);
        failures = 0;
        setUpdateJob(job);
        if (updateJobIsActive(job)) {
          updatePollRef.current = window.setTimeout(tick, 350);
          return;
        }
        updatePollRef.current = 0;
        setUpdateDialogOpen(true);
        if (job?.state === 'succeeded') {
          toast({
            kind: 'ok',
            text: desktopUpdateRestartAvailable()
              ? '升级安装完成，可立即重启 ACECode'
              : '升级安装完成，请完全退出并重新启动 ACECode',
          });
        } else if (job?.state === 'failed') {
          toast({ kind: 'err', text: '升级失败:' + (job.error || '未知错误') });
        }
      } catch (e) {
        failures += 1;
        if (failures < 5) {
          updatePollRef.current = window.setTimeout(tick, 600);
          return;
        }
        updatePollRef.current = 0;
        setUpdateJob((prev) => ({
          ...(prev || { job_id: jobId }),
          state: 'failed',
          error: e?.message || '无法获取升级进度',
        }));
        setUpdateDialogOpen(true);
      }
    };
    tick();
  }, []);

  useEffect(() => () => {
    if (updatePollRef.current) window.clearTimeout(updatePollRef.current);
  }, []);

  useEffect(() => {
    if (authState !== 'ok') return undefined;
    let cancelled = false;
    Promise.allSettled([api.getUpdateStatus(), api.getLatestUpdateJob()])
      .then(([statusResult, jobResult]) => {
        if (cancelled) return;
        setUpdateStatus(statusResult.status === 'fulfilled' ? statusResult.value : null);
        if (jobResult.status !== 'fulfilled') return;
        const job = jobResult.value;
        setUpdateJob(job);
        if (updateJobIsActive(job)) {
          setUpdateDialogOpen(true);
          pollUpdateJob(job.job_id);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [authState, pollUpdateJob]);

  useEffect(() => {
    if (authState !== 'ok' || !desktopGuidedTourModeEligible(desktopModeRef.current)) {
      return undefined;
    }
    let cancelled = false;
    Promise.allSettled([api.getDesktopOnboarding(), api.listModels()])
      .then(([statusResult, modelsResult]) => {
        if (cancelled || statusResult.status !== 'fulfilled') return;
        setGuidedTourState({
          loaded: true,
          dismissed: !!statusResult.value?.dismissed,
          hasModel: modelsResult.status === 'fulfilled'
            ? desktopGuidedTourHasModel(modelsResult.value)
            : true,
        });
      });
    return () => {
      cancelled = true;
    };
  }, [authState]);

  // 解析 URL 上的 ?open=<sessionId>&workspace=<hash>(跨 workspace 跳转后落地用)。
  // 解析后立即从 URL 抹掉,避免刷新二次触发。
  useEffect(() => {
    if (typeof window === 'undefined' || authState !== 'ok') return;
    const target = startupOpenTargetRef.current;
    if (!target || startupNavigationStartedRef.current) return;
    startupNavigationStartedRef.current = true;
    const qs = stripOpenSessionParams(window.location.search);
    const newUrl = window.location.pathname + (qs ? '?' + qs : '');
    window.history.replaceState(null, '', newUrl);
    resumeAndOpenSession(target, { replace: true, allowDesktopActivate: false })
      .catch(() => {})
      .finally(() => setStartupNavigationSettled(true));
  }, [authState, resumeAndOpenSession]);

  // 桌面壳通知 click_handler 走 webview eval 调这两个 window 全局函数,见
  // openspec/changes/add-windows-wintoast-completion-notifications。
  // 同 workspace:focusSessionFromBridge → resume + setActiveRef
  // 跨 workspace:activateAndOpenSession → 激活 workspace 后 resume + 整页 navigate
  useEffect(() => {
    if (typeof window === 'undefined') return;
    window.aceDesktop_focusSessionFromBridge = function focusSessionFromBridge(sessionId, workspaceHash) {
      if (!sessionId) return;
      const hasWorkspaceArg = workspaceHash !== undefined;
      const targetHash = hasWorkspaceArg ? String(workspaceHash || '') : (activeRefRef.current?.workspaceHash || '');
      resumeAndOpenSession({
        sessionId: String(sessionId),
        workspaceHash: targetHash,
        noWorkspace: hasWorkspaceArg && !targetHash,
      }, { allowDesktopActivate: false }).catch(() => {});
    };
    window.aceDesktop_activateAndOpenSession = async (workspaceHash, sessionId) => {
      if (!sessionId) return;
      const targetHash = String(workspaceHash || '');
      await resumeAndOpenSession({
        sessionId: String(sessionId),
        workspaceHash: targetHash,
        noWorkspace: !targetHash,
      });
    };
    return () => {
      try {
        delete window.aceDesktop_focusSessionFromBridge;
        delete window.aceDesktop_activateAndOpenSession;
      } catch {
        // strict mode 下 delete window prop 偶发抛错;static assignment 兜底
        window.aceDesktop_focusSessionFromBridge = undefined;
        window.aceDesktop_activateAndOpenSession = undefined;
      }
    };
  }, [resumeAndOpenSession]);

  // 全局 Ctrl/Cmd+K 切换搜索面板。matchShortcut 处理大小写与修饰键。
  useGlobalShortcut(
    (e) => e.key && e.key.toLowerCase() === 'k' && (e.ctrlKey || e.metaKey),
    () => setSearchOpen((o) => !o),
    [],
  );

  // Ctrl+` toggle 控制台(终端聚焦时 xterm 的 customKeyEventHandler 放行该
  // 组合,事件照常冒泡到 window)。后端 console 不可用时快捷键惰化。
  const consoleAvailable = !!health?.console?.available;
  const toggleConsoleDock = useCallback(() => {
    if (!consoleAvailable) return;
    setConsoleDock((prev) => ({ ...prev, open: !prev.open }));
  }, [consoleAvailable, setConsoleDock]);
  useGlobalShortcut(
    // e.code 是物理键位:中文 IME 下反引号键的 e.key 是 '·',精确匹配 key 会失效。
    (e) => (e.code === 'Backquote' || e.key === '`') && e.ctrlKey && !e.altKey && !e.metaKey,
    toggleConsoleDock,
    [toggleConsoleDock],
  );
  const setConsoleDockHeight = useCallback((next) => {
    setConsoleDock((prev) => ({
      ...prev,
      height: clampDockHeight(next, window.innerHeight),
    }));
  }, [setConsoleDock]);
  const setConsoleDockOpen = useCallback((open) => {
    setConsoleDock((prev) => ({ ...prev, open: !!open }));
  }, [setConsoleDock]);

  const handleSelectSession = useCallback(async (session) => {
    if (!session?.id) return;
    setSearchOpen(false);
    await resumeAndOpenSession(session);
  }, [resumeAndOpenSession]);
  const openConversationFind = useCallback(() => {
    setConversationFindRequest((request) => request + 1);
  }, []);
  const handleSelectWorkspace = useCallback((workspace) => {
    const workspaceHash = workspace?.hash || workspace?.workspaceHash || '';
    if (!workspaceHash) return;
    setSearchOpen(false);
    setWorkspaceActivationRequest((previous) => ({
      requestId: (previous?.requestId || 0) + 1,
      workspaceHash,
    }));
  }, []);

  useEffect(() => {
    const monitor = notificationMonitorRef.current;
    const context = notificationContextRef.current;

    const rememberCurrentTitle = (sessionId) => {
      const current = activeRefRef.current || {};
      const currentId = current.sessionId || current.id || '';
      if (!sessionId || currentId !== sessionId) return;
      const title = sessionDisplayTitle(current);
      if (title) context.titles.set(sessionId, title);
    };

    const permissionOwnerForSession = (sessionId) => {
      if (!sessionId) return '';
      const directory = subagentDirectoryRef.current || {};
      if (directory.owners?.[sessionId]) return directory.owners[sessionId];
      const currentIndex = subagentIndexRef.current || {};
      if (currentIndex.parentId && currentIndex.titles?.[sessionId]) {
        return currentIndex.parentId;
      }
      return sessionId;
    };

    const sendDesktopNotification = (type, msg, payload) => {
      const sessionId = payload.session_id || msg.session_id || '';
      if (!sessionId) return;
      const key = notificationEventKey(type, sessionId, msg, payload);
      if (!monitor.markSeen(key)) return;
      rememberCurrentTitle(sessionId);
      const workspaceHash = payload.workspace_hash
        || msg.workspace_hash
        || context.workspaces.get(sessionId)
        || '';
      if (workspaceHash) context.workspaces.set(sessionId, workspaceHash);
      const current = activeRefRef.current || {};
      const currentId = current.sessionId || current.id || '';
      const sessionTitle = context.titles.get(sessionId)
        || subagentIndexRef.current?.titles?.[sessionId]
        || (currentId === sessionId ? sessionDisplayTitle(current) : '');
      maybeNotify({
        type,
        sessionId,
        workspaceHash,
        sessionTitle,
        bodyText: notificationBodyFromEvent(type, payload),
        activeRef: visibleSessionRef.current,
        hasFocus: typeof document !== 'undefined'
          && typeof document.hasFocus === 'function'
          ? document.hasFocus()
          : true,
        cfg: healthRef.current?.notifications,
      });
    };

    const finishMonitoredSession = (sessionId, msg, payload) => {
      const outcome = String(payload.outcome || '');
      const assistantText = context.assistantText.get(sessionId) || '';
      if ((!outcome || outcome === 'completed') && assistantText.trim()) {
        sendDesktopNotification('completion', msg, {
          ...payload,
          session_id: sessionId,
          final_assistant_text: assistantText,
        });
      }
      context.assistantText.delete(sessionId);
      monitor.release(sessionId);
    };

    const handler = (e) => {
      const msg = e.detail || {};
      const payload = { ...(msg.payload || {}) };
      if (msg.session_id && !payload.session_id) payload.session_id = msg.session_id;
      if ((msg.type === 'question_request'
          || msg.type === 'permission_request'
          || msg.type === 'permission_closed')
          && !payload.session_id) {
        const current = activeRefRef.current || {};
        payload.session_id = current.sessionId || current.id || '';
      }
      const sessionId = payload.session_id || msg.session_id || '';
      const workspaceHash = payload.workspace_hash || msg.workspace_hash || '';
      if (sessionId && workspaceHash) {
        context.workspaces.set(sessionId, workspaceHash);
      }
      if (sessionId) rememberCurrentTitle(sessionId);

      if (msg.type === 'session_updated' && sessionId && payload.title) {
        context.titles.set(sessionId, String(payload.title));
      }
      if (msg.type === 'message' && sessionId && payload.role === 'assistant') {
        const text = String(payload.content || '');
        if (text.trim()) context.assistantText.set(sessionId, text);
      }
      if (msg.type === 'message' && sessionId && payload.role === 'error') {
        context.assistantText.delete(sessionId);
      }
      if (msg.type === 'token' && sessionId && payload.text) {
        const previous = context.assistantText.get(sessionId) || '';
        context.assistantText.set(
          sessionId,
          (previous + String(payload.text)).slice(-4096),
        );
      }
      if (msg.type === 'busy_changed' && sessionId) {
        if (payload.busy === true) {
          context.assistantText.delete(sessionId);
          monitor.retain(sessionId);
        } else {
          if (payload.outcome) {
            setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
          }
          finishMonitoredSession(sessionId, msg, payload);
        }
      }
      if (msg.type === 'session_status_snapshot') {
        const sessions = Array.isArray(payload.sessions) ? payload.sessions : [];
        for (const status of sessions) {
          const statusSessionId = status?.session_id || '';
          if (!statusSessionId || status?.busy !== true) continue;
          if (status.workspace_hash) {
            context.workspaces.set(statusSessionId, status.workspace_hash);
          }
          monitor.retain(statusSessionId);
        }
      }
      if (msg.type === 'session_status' && sessionId) {
        if (payload.busy === true) monitor.retain(sessionId);
      }
      if (msg.type === 'permission_request') {
        if (sessionId) monitor.retain(sessionId);
        const ownerSessionId = permissionOwnerForSession(sessionId);
        setPermReqs((prev) => pushPermissionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
        sendDesktopNotification('permission', msg, {
          ...payload,
          session_id: ownerSessionId || sessionId,
        });
      }
      if (msg.type === 'permission_closed') {
        const ownerSessionId = permissionOwnerForSession(sessionId);
        setPermReqs((prev) => closePermissionRequest(prev, payload, {
          ownerSessionId: ownerSessionId !== sessionId ? ownerSessionId : '',
        }));
      }
      if (msg.type === 'question_request') {
        if (sessionId) monitor.retain(sessionId);
        setQuestionReqs((prev) => addPendingQuestionRequest(prev, payload));
        sendDesktopNotification('question', msg, payload);
      }
      if (msg.type === 'question_closed') {
        setQuestionReqs((prev) => removePendingQuestionRequest(prev, payload.request_id));
      }
      if (msg.type === 'done' && sessionId) {
        setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
        finishMonitoredSession(sessionId, msg, payload);
      }
      const permissionTimeoutDiagnostic = msg.type === 'error'
        && payload.reason === 'permission_timeout'
        && !!payload.request_id;
      if ((msg.type === 'error' || msg.type === 'turn_aborted')
          && sessionId
          && !permissionTimeoutDiagnostic) {
        setPermReqs((prev) => clearResolvedPermissionRequests(prev, sessionId));
        context.assistantText.delete(sessionId);
        monitor.release(sessionId);
      }
    };
    connection.addEventListener('message', handler);
    return () => {
      connection.removeEventListener('message', handler);
      monitor.dispose();
    };
  }, []);

  const onSubmitToken = useCallback(async (token) => {
    setToken(token);
    await probe();
  }, [probe]);

  const toggleSidePanel = useCallback(() => {
    setUiPrefs((prev) => ({ ...prev, sidePanelCollapsed: !prev.sidePanelCollapsed }));
  }, [setUiPrefs]);

  const toggleSidePanelList = useCallback(() => {
    setUiPrefs((prev) => ({
      ...prev,
      sidePanelListCollapsed: !effectiveSidePanelListCollapsed(prev),
    }));
  }, [setUiPrefs]);

  const revealSidePanelList = useCallback(() => {
    setUiPrefs((prev) => {
      if (!prev.sidePanelCollapsed && !effectiveSidePanelListCollapsed(prev)) return prev;
      return {
        ...prev,
        sidePanelCollapsed: false,
        sidePanelListCollapsed: false,
      };
    });
  }, [setUiPrefs]);

  // 最大化 / 还原中间预览面板。沿用旧字段名保存偏好,但 UI 控件已迁到预览面板。
  // 最大化时强制确保右侧 SidePanel 未折叠,符合"右侧文件栏仍然可用"的行为。
  const toggleSidePanelMaximized = useCallback(() => {
    setUiPrefs((prev) => {
      const nextMax = !prev.sidePanelMaximized;
      return {
        ...prev,
        sidePanelMaximized: nextMax,
        sidePanelCollapsed: nextMax ? false : prev.sidePanelCollapsed,
      };
    });
  }, [setUiPrefs]);

  const toggleProjectSidebar = useCallback(() => {
    setUiPrefs((prev) => ({
      ...prev,
      sidebarCollapsed: !prev.sidebarCollapsed,
    }));
  }, [setUiPrefs]);

  const setFontSize = useCallback((nextFontSize) => {
    setUiPrefs((prev) => ({
      ...prev,
      fontSize: FONT_SIZE_VALUES.includes(nextFontSize) ? nextFontSize : DEFAULT_FONT_SIZE,
    }));
  }, [setUiPrefs]);

  const openUpdateDialog = useCallback(() => {
    if (!updateJobIsActive(updateJob)
        && updateJob?.target_version
        && updateStatus?.latest_version
        && updateJob.target_version !== updateStatus.latest_version) {
      setUpdateJob(null);
    }
    setUpdateDialogOpen(true);
  }, [updateJob, updateStatus]);

  const checkForUpdates = useCallback(async () => {
    if (updateChecking) return;
    setUpdateChecking(true);
    try {
      const status = await api.getUpdateStatus();
      setUpdateStatus(status);
      if (status?.update_available
          || updateJobIsActive(updateJob)
          || (updateJob?.state === 'succeeded' && updateJob?.restart_required)) {
        setUpdateDialogOpen(true);
        return;
      }
      const currentVersion = String(status?.current_version || health?.version || '').trim();
      toast({
        kind: 'ok',
        text: currentVersion
          ? `当前已是最新版本 v${currentVersion.replace(/^v/i, '')}`
          : '当前已是最新版本',
      });
    } catch (e) {
      toast({ kind: 'err', text: '检查更新失败:' + (e?.message || '未知错误') });
    } finally {
      setUpdateChecking(false);
    }
  }, [health?.version, updateChecking, updateJob]);

  const showAboutAceCode = useCallback(async () => {
    const result = await showDesktopAboutDialog();
    if (!result?.ok) openSettingsSection('about');
  }, [openSettingsSection]);

  const exitAceCode = useCallback(async () => {
    const result = await requestDesktopAppExit();
    if (result?.ok) return;
    toast({
      kind: 'err',
      text: result?.unavailable
        ? '当前环境不支持退出 ACECode'
        : '退出 ACECode 失败:' + (result?.error || '未知错误'),
    });
  }, []);

  const startUpdate = useCallback(async () => {
    if (!updateStatus?.update_available || updateStarting || updateJobIsActive(updateJob)) return;
    setUpdateStarting(true);
    try {
      const job = await api.startUpdate();
      setUpdateJob(job);
      setUpdateDialogOpen(true);
      pollUpdateJob(job?.job_id);
    } catch (e) {
      if (e?.code === 'UPDATE_IN_PROGRESS' && e?.body?.job) {
        const job = e.body.job;
        setUpdateJob(job);
        setUpdateDialogOpen(true);
        pollUpdateJob(job.job_id);
        return;
      }
      toast({ kind: 'err', text: '启动升级失败:' + (e?.message || '') });
    } finally {
      setUpdateStarting(false);
    }
  }, [pollUpdateJob, updateJob, updateStarting, updateStatus]);

  const restartAfterUpdate = useCallback(async () => {
    if (updateRestarting
        || updateJob?.state !== 'succeeded'
        || !updateJob?.restart_required) return;
    setUpdateRestarting(true);
    try {
      await requestDesktopUpdateRestart();
    } catch (e) {
      setUpdateRestarting(false);
      toast({ kind: 'err', text: '自动重启失败:' + (e?.message || '未知错误') });
    }
  }, [updateJob, updateRestarting]);

  const openHomeForWorkspace = useCallback((workspace = null) => {
    const target = workspace == null ? noHomeWorkspaceOption() : workspace;
    navigateToRef(homeRefFromWorkspace(target, activeRefRef.current, health));
  }, [health, navigateToRef]);

  const openLoopPage = useCallback(() => {
    navigateToRef({ loop: true });
  }, [navigateToRef]);

  const replaceHomeWorkspace = useCallback((workspace) => {
    replaceActiveRef((current) => homeRefFromWorkspace(workspace, current, health));
  }, [health, replaceActiveRef]);

  const abortGuidedTour = useCallback(() => {
    setGuidedTourRun(false);
    setGuidedTourPreparing(false);
    setGuidedTourForced(false);
  }, []);

  const dismissGuidedTour = useCallback(async ({ openModels = false } = {}) => {
    setGuidedTourRun(false);
    setGuidedTourPreparing(false);
    setGuidedTourForced(false);
    try {
      const state = await api.dismissDesktopOnboarding();
      setGuidedTourState((prev) => ({
        ...prev,
        loaded: true,
        dismissed: !!state?.dismissed,
      }));
    } catch (e) {
      toast({ kind: 'err', text: '指引已关闭，但状态保存失败，下次启动可能再次显示：' + (e?.message || '') });
    }
    if (openModels) openSettingsSection('models');
  }, [openSettingsSection]);

  const replayGuidedTour = useCallback(async () => {
    setShowSettings(false);
    setSearchOpen(false);
    setGuidedTourRun(false);
    setGuidedTourForced(true);
    navigateToRef(homeRefFromWorkspace(activeRefRef.current || {}, activeRefRef.current, health));
    try {
      const models = await api.listModels();
      setGuidedTourState((prev) => ({
        ...prev,
        hasModel: desktopGuidedTourHasModel(models),
      }));
    } catch {
      // 重播仍可继续；模型列表读取失败时沿用最近一次已知状态。
    } finally {
      setGuidedTourPreparing(true);
    }
  }, [health, navigateToRef]);

  useEffect(() => {
    if (guidedTourForced) return;
    const shouldStart = shouldAutoStartDesktopGuidedTour({
      mode: desktopModeRef.current,
      authState,
      stateLoaded: guidedTourState.loaded,
      dismissed: guidedTourState.dismissed,
      startupNavigationSettled,
      hasActiveSession: guidedTourHasActiveSession,
      blocked: guidedTourBlocked,
      targetsReady: desktopGuidedTourTargetsReady(),
      attempted: guidedTourAutoAttemptedRef.current,
    });
    if (!shouldStart) return;
    guidedTourAutoAttemptedRef.current = true;
    setGuidedTourPreparing(true);
  }, [
    authState,
    guidedTourBlocked,
    guidedTourForced,
    guidedTourHasActiveSession,
    guidedTourState.dismissed,
    guidedTourState.loaded,
    startupNavigationSettled,
  ]);

  useEffect(() => {
    if (!guidedTourPreparing || !shouldPrepareDesktopGuidedTour({
      mode: desktopModeRef.current,
      authState,
      startupNavigationSettled,
      hasActiveSession: guidedTourHasActiveSession,
      blocked: guidedTourBlocked,
    })) {
      return undefined;
    }
    const timer = window.setTimeout(() => {
      if (!desktopGuidedTourTargetsReady()) {
        abortGuidedTour();
        return;
      }
      setGuidedTourRun(true);
      setGuidedTourPreparing(false);
    }, 280);
    return () => window.clearTimeout(timer);
  }, [
    abortGuidedTour,
    authState,
    guidedTourBlocked,
    guidedTourHasActiveSession,
    guidedTourPreparing,
    startupNavigationSettled,
  ]);

  useEffect(() => {
    if (!guidedTourHasActiveSession) return;
    // 导航到会话属于非终止性中断；回到 Home 后允许当前未关闭版本重新尝试。
    guidedTourAutoAttemptedRef.current = false;
    if (guidedTourPreparing || guidedTourRun) abortGuidedTour();
  }, [abortGuidedTour, guidedTourHasActiveSession, guidedTourPreparing, guidedTourRun]);

  const createDesktopTraySession = useCallback(async () => {
    try {
      const next = await createNewSessionForActiveWorkspace(api, activeRefRef.current, health);
      const sessionId = next?.sessionId || next?.id || '';
      const noWorkspace = !!(next?.noWorkspace || next?.no_workspace);
      notifySessionListChanged({
        reason: 'session-created',
        sessionId,
        workspaceHash: noWorkspace ? '' : (next?.workspaceHash || next?.workspace_hash || ''),
        noWorkspace,
        session: {
          ...next,
          id: sessionId,
          workspace_hash: noWorkspace ? '' : (next?.workspaceHash || next?.workspace_hash || ''),
          no_workspace: noWorkspace,
        },
      });
      navigateToRef(next);
    } catch (e) {
      toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
    }
  }, [health, navigateToRef]);

  const handleSubagentTasksChange = useCallback((info) => {
    const parentId = info?.parentId || '';
    const titles = info?.titles && typeof info.titles === 'object' ? info.titles : {};
    setSubagentIndex({ parentId, titles });
    if (!parentId || Object.keys(titles).length === 0) return;
    setSubagentDirectory((previous) => {
      const owners = { ...previous.owners };
      const mergedTitles = { ...previous.titles };
      for (const [childId, title] of Object.entries(titles)) {
        if (!childId) continue;
        owners[childId] = parentId;
        mergedTitles[childId] = title;
      }
      const next = { owners, titles: mergedTitles };
      subagentDirectoryRef.current = next;
      return next;
    });
  }, []);

  const handlePermissionDecision = useCallback((request, choice) => {
    if (!request?.request_id || !choice) return;
    setPermReqs((prev) => markPermissionSubmitting(prev, request.request_id, choice));
    connection.sendDecision(request.request_id, choice, request.session_id);
  }, []);

  const handleDesktopNotificationsChanged = useCallback((notifications) => {
    setHealth((previous) => {
      const next = {
        ...(previous || {}),
        notifications: {
          ...(previous?.notifications || {}),
          ...(notifications || {}),
        },
      };
      healthRef.current = next;
      return next;
    });
  }, []);

  // 暴露 aceDesktop_createNewSession 给 desktop 壳的托盘 "新建会话" 菜单调用。
  // 设计:openspec/changes/enhance-desktop-tray-menu。
  useEffect(() => {
    if (typeof window === 'undefined') return undefined;
    window.aceDesktop_createNewSession = () => {
      createDesktopTraySession();
    };
    return () => {
      if (window.aceDesktop_createNewSession) delete window.aceDesktop_createNewSession;
    };
  }, [createDesktopTraySession]);

  const setSidebarWidth = useCallback((nextWidth, shellWidth = 0) => {
    const sidePanelVisible = !!(activeRef?.sessionId || activeRef?.id)
      && !sidePanelNavigationCollapsed;
    setSingleLayout((prev) => {
      const sidebar = normalizeSidebarWidth(nextWidth, {
        shellWidth,
        sidePanelWidth: prev.sidePanel,
        sidePanelVisible,
        previewPanelWidth: prev.previewPanel,
        previewPanelVisible,
      });
      return sidebar === prev.sidebar ? prev : { ...prev, sidebar };
    });
  }, [
    activeRef?.id,
    activeRef?.sessionId,
    previewPanelVisible,
    setSingleLayout,
    sidePanelNavigationCollapsed,
  ]);

  const setSidePanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    setSingleLayout((prev) => {
      const sidePanel = normalizeSidePanelWidth(nextWidth, {
        contentWidth,
        previewPanelWidth: prev.previewPanel,
        previewPanelVisible,
        previewPanelMaximized: sidePanelMaximized,
      });
      return sidePanel === prev.sidePanel ? prev : { ...prev, sidePanel };
    });
  }, [previewPanelVisible, setSingleLayout, sidePanelMaximized]);

  const setPreviewPanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    const sidePanelVisible = !!(activeRef?.sessionId || activeRef?.id)
      && !sidePanelNavigationCollapsed;
    setSingleLayout((prev) => {
      const previewPanel = normalizePreviewPanelWidth(nextWidth, {
        contentWidth,
        sidePanelWidth: prev.sidePanel,
        sidePanelVisible,
        sidePanelCollapsed: sidePanelNavigationCollapsed,
      });
      return previewPanel === prev.previewPanel ? prev : { ...prev, previewPanel };
    });
  }, [
    activeRef?.id,
    activeRef?.sessionId,
    setSingleLayout,
    sidePanelNavigationCollapsed,
  ]);

  const startSidebarResize = useCallback((event) => {
    if (view !== 'single') return;
    if (event.button != null && event.button !== 0) return;
    if (sidebarResizeActiveRef.current) return;
    sidebarResizeActiveRef.current = true;
    event.preventDefault();
    const shellWidth = singleShellRef.current?.getBoundingClientRect().width || 0;
    const startX = event.clientX;
    const startWidth = singleLayout.sidebar;
    document.body.classList.add('ace-resizing');
    if (event.pointerId != null) event.currentTarget.setPointerCapture?.(event.pointerId);

    const onMove = (moveEvent) => {
      setSidebarWidth(startWidth + moveEvent.clientX - startX, shellWidth);
    };
    const onStop = () => {
      sidebarResizeActiveRef.current = false;
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
  }, [setSidebarWidth, singleLayout.sidebar, view]);

  const onSidebarHandleKeyDown = useCallback((event) => {
    if (view !== 'single') return;
    const step = event.shiftKey ? 32 : 12;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      const delta = event.key === 'ArrowRight' ? step : -step;
      const shellWidth = singleShellRef.current?.getBoundingClientRect().width || 0;
      setSidebarWidth(singleLayout.sidebar + delta, shellWidth);
    }
  }, [setSidebarWidth, singleLayout.sidebar, view]);

  const activeId = activeRef?.sessionId || activeRef?.id || '';
  const openLoopRun = useCallback((run) => {
    const sessionId = run?.session_id || '';
    if (!sessionId) return;
    resumeAndOpenSession({
      id: sessionId,
      sessionId,
      workspaceHash: run.workspace_hash || '',
      cwd: run.workspace_cwd || '',
      no_workspace: !run.workspace_hash,
      active: false,
    }, { forceResume: true });
  }, [resumeAndOpenSession]);
  const activeRefConsoleCwd = consoleCwdForContext({ activeRef, health });
  const preferredConsoleCwd = activeId ? activeRefConsoleCwd : (consoleCwd || activeRefConsoleCwd);
  const permissionOwnership = useMemo(() => ({
    owners: subagentDirectory.owners,
    titles: {
      ...subagentDirectory.titles,
      ...subagentIndex.titles,
    },
    parentId: subagentIndex.parentId,
  }), [subagentDirectory, subagentIndex]);
  const visiblePermissionEntries = useMemo(
    () => visiblePermissionRequests(permReqs, activeId, permissionOwnership)
      .map((entry) => ({
        ...entry,
        origin_label: permissionOriginLabel(entry, permissionOwnership),
      })),
    [activeId, permReqs, permissionOwnership],
  );
  const visiblePermissionUnresolved = hasUnresolvedPermission(visiblePermissionEntries);
  const pendingPermissionSessionIdsForSidebar = useMemo(
    () => pendingPermissionSessionIds(permReqs, activeId, permissionOwnership),
    [activeId, permReqs, permissionOwnership],
  );
  const pendingQuestionSessionIdsForSidebar = useMemo(
    () => pendingQuestionSessionIds(questionReqs, activeId),
    [questionReqs, activeId],
  );

  if (authState === 'checking') {
    return (
      <>
        <div className="h-full flex items-center justify-center text-fg-mute text-sm">
          <span className="ace-spinner mr-2" /> 连接 daemon…
        </div>
        <FramelessResizeHandles />
        <DesktopContextMenu />
        <Toaster />
      </>
    );
  }
  if (authState === 'need-token') {
    return (
      <>
        <TokenPrompt onSubmit={onSubmitToken} />
        <FramelessResizeHandles />
        <DesktopContextMenu />
        <Toaster />
      </>
    );
  }

  const sidebarCollapsed = view !== 'single'
    || (projectSidebarCollapsed && !guidedTourPreparing && !guidedTourRun);
  const visibleQuestionReq = !visiblePermissionUnresolved
    ? questionReqs.find((req) => {
        const reqSid = req?.session_id || '';
        if (!reqSid || (activeId && reqSid === activeId)) return true;
        // 后台任务子会话的提问在其父会话(当前主会话)里显示与回答。
        return !!(activeId &&
                  subagentIndex.parentId === activeId &&
                  subagentIndex.titles[reqSid]);
      }) || null
    : null;
  const resolveVisibleQuestion = () => {
    if (!visibleQuestionReq?.request_id) return;
    setQuestionReqs((prev) => removePendingQuestionRequest(prev, visibleQuestionReq.request_id));
  };
  const autoFocusChatOnDesktopWindowFocus = shouldAutoFocusDesktopComposer({
    desktopMode: desktopModeRef.current,
    chatVisible: view === 'single' && !activeRef?.loop,
    blockingSurfaceOpen: showSettings || searchOpen || updateDialogOpen
      || !!visibleQuestionReq || guidedTourPreparing || guidedTourRun,
  });
  const conversationFindEnabled = canOpenConversationFind({
    view,
    activeSessionId: activeId,
    loop: !!activeRef?.loop,
    showSettings,
    searchOpen,
    updateDialogOpen,
    permissionOpen: false,
    questionOpen: !!visibleQuestionReq,
    guidedTourPreparing,
    guidedTourRun,
  });

  return (
    <SlashCommandsProvider workspaceHash={commandWorkspaceHash}>
    <div
      className={[
        'h-full w-full flex flex-col text-fg font-sans bg-bg',
      ].join(' ')}
    >
      <TopBar
        onSettings={() => openSettingsSection('general')}
        onNewSession={() => openHomeForWorkspace()}
        onOpenLoop={openLoopPage}
        onOpenSearch={() => setSearchOpen(true)}
        onAbout={showAboutAceCode}
        onExit={exitAceCode}
        onToggleConsole={toggleConsoleDock}
        consoleAvailable={consoleAvailable}
        consoleOpen={consoleDock.open}
        rightPanelAvailable={!!(activeRef?.sessionId || activeRef?.id) && !activeRef?.loop}
        rightPanelCollapsed={sidePanelCollapsed}
        onToggleRightPanel={toggleSidePanel}
        sidebarCollapsed={sidebarCollapsed}
        sidebarWidth={singleLayout.sidebar}
        onToggleSidebar={toggleProjectSidebar}
        onGoBack={goBackActiveRef}
        onGoForward={goForwardActiveRef}
        canGoBack={navHistory.back.length > 0}
        canGoForward={navHistory.forward.length > 0}
        updateStatus={updateStatus}
        updateChecking={updateChecking}
        updateStarting={updateStarting}
        updateRunning={updateJobIsActive(updateJob)}
        updateReady={updateJob?.state === 'succeeded' && !!updateJob?.restart_required}
        onStartUpdate={openUpdateDialog}
        onCheckUpdates={checkForUpdates}
        appVersion={health?.version || ''}
      />
      <div ref={singleShellRef} className="flex-1 flex overflow-hidden relative min-h-0 ace-single-shell">
        <Sidebar
          activeId={activeId}
          activeRef={activeRef}
          onSelect={navigateToRef}
          collapsed={sidebarCollapsed}
          width={singleLayout.sidebar}
          onOpenHome={openHomeForWorkspace}
          onNewTask={() => openHomeForWorkspace()}
          onNewLoop={openLoopPage}
          onSearchTasks={() => setSearchOpen(true)}
          workspaceActivationRequest={workspaceActivationRequest}
          onOpenSettingsSection={openSettingsSection}
          pendingPermissionSessionIds={pendingPermissionSessionIdsForSidebar}
          pendingQuestionSessionIds={pendingQuestionSessionIdsForSidebar}
        />
        {view === 'single' && !sidebarCollapsed && (
          <div
            role="separator"
            aria-label="调整左侧栏宽度"
            aria-orientation="vertical"
            tabIndex={0}
            className="ace-resize-handle ace-resize-handle-left"
            onPointerDown={startSidebarResize}
            onMouseDown={startSidebarResize}
            onKeyDown={onSidebarHandleKeyDown}
            title="拖动调整左侧栏宽度"
          />
        )}
        <div
          className={[
            'flex-1 flex flex-col overflow-hidden transition-all duration-200',
            'opacity-100 scale-100',
          ].join(' ')}
        >
          <div className="flex-1 flex overflow-hidden min-h-0">
            {view === 'single' && activeRef?.loop && (
              <LoopPage onOpenSession={openLoopRun} />
            )}
            {view === 'single' && !activeRef?.loop && (
              <ChatView
                sessionRef={activeRef}
                onSessionPromoted={navigateToRef}
                onHomeWorkspaceChange={replaceHomeWorkspace}
                onCommandWorkspaceChange={setCommandWorkspaceHash}
                onConsoleCwdChange={setConsoleCwd}
                onFindInConversation={openConversationFind}
                health={health}
                autoFocusOnDesktopWindowFocus={autoFocusChatOnDesktopWindowFocus}
                showSidePanel
                sidePanelWidth={singleLayout.sidePanel}
                onSidePanelResize={setSidePanelWidth}
                previewPanelWidth={singleLayout.previewPanel}
                onPreviewPanelResize={setPreviewPanelWidth}
                onPreviewPanelVisibleChange={setPreviewPanelVisible}
                sidePanelCollapsed={sidePanelCollapsed}
                sidePanelListCollapsed={sidePanelListCollapsed}
                onToggleSidePanel={toggleSidePanel}
                onToggleSidePanelList={toggleSidePanelList}
                onRevealSidePanelList={revealSidePanelList}
                sidePanelMaximized={sidePanelMaximized}
                onToggleSidePanelMaximized={toggleSidePanelMaximized}
                showAceCodeAvatar={showAceCodeAvatar}
                permissionRequests={visiblePermissionEntries}
                onPermissionDecision={handlePermissionDecision}
                questionRequest={visibleQuestionReq}
                onQuestionResolve={resolveVisibleQuestion}
                onSubagentTasksChange={handleSubagentTasksChange}
              />
            )}
          </div>
          {consoleAvailable && (
            <ConsoleDock
              open={consoleDock.open}
              height={consoleDock.height}
              onHeightChange={setConsoleDockHeight}
              onToggle={setConsoleDockOpen}
              consoleInfo={health?.console}
              preferredCwd={preferredConsoleCwd}
            />
          )}
        </div>
        {showSettings && (
          <SettingsPage
            onClose={() => setShowSettings(false)}
            initialNavKey={settingsNavKey}
            health={health}
            activeSessionId={activeId}
            onDesktopNotificationsChanged={handleDesktopNotificationsChanged}
            onReplayGuidedTour={desktopGuidedTourModeEligible(desktopModeRef.current)
              ? replayGuidedTour
              : undefined}
            fontSize={fontSize}
            onFontSizeChange={setFontSize}
          />
        )}
        <SearchPalette
          open={searchOpen}
          onClose={() => setSearchOpen(false)}
          currentWorkspaceHash={activeRef?.workspaceHash || ''}
          onSelectSession={handleSelectSession}
          onSelectWorkspace={handleSelectWorkspace}
        />
      </div>
      <FramelessResizeHandles />
      <GlobalFindOverlay
        enabled={conversationFindEnabled}
        openRequest={conversationFindRequest}
        scopeKey={activeId}
      />
      <DesktopContextMenu />
      <UpdateDialog
        open={updateDialogOpen}
        updateStatus={updateStatus}
        job={updateJob}
        starting={updateStarting}
        restarting={updateRestarting}
        restartAvailable={desktopUpdateRestartAvailable()}
        onConfirm={startUpdate}
        onRetry={startUpdate}
        onRestart={restartAfterUpdate}
        onClose={() => setUpdateDialogOpen(false)}
      />
      <DesktopGuidedTour
        run={guidedTourRun && !guidedTourBlocked}
        hasModel={guidedTourState.hasModel}
        onDismiss={dismissGuidedTour}
        onAbort={abortGuidedTour}
      />
      <Toaster />
    </div>
    </SlashCommandsProvider>
  );
}
