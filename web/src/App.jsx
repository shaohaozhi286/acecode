// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 270px Sidebar + 主区(单会话/4宫格/9宫格)
// + 22px StatusBar。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api, ApiError, createApi } from './lib/api.js';
import { setToken } from './lib/auth.js';
import { connection } from './lib/connection.js';
import { createNewSessionForActiveWorkspace } from './lib/newSession.js';
import { goBack, goForward, pushNavigation } from './lib/navigationHistory.js';
import {
  addPendingQuestionRequest,
  pendingQuestionSessionIds,
  removePendingQuestionRequest,
} from './lib/pendingQuestions.js';
import { usePreference } from './lib/usePreference.js';
import {
  DEFAULT_UI_PREFS,
  DEFAULT_FONT_SIZE,
  effectiveFontSize,
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
import { PermissionModal } from './components/PermissionModal.jsx';
import { SettingsPage } from './components/SettingsPage.jsx';
import { DesktopContextMenu } from './components/DesktopContextMenu.jsx';
import { Toaster, toast } from './components/Toast.jsx';
import { SlashCommandsProvider } from './components/SlashCommandsContext.jsx';
import { FramelessResizeHandles } from './components/FramelessResizeHandles.jsx';
import { GlobalFindOverlay } from './components/GlobalFindOverlay.jsx';
import { ConsoleDock } from './components/ConsoleDock.jsx';
import {
  CONSOLE_DOCK_DEFAULT_HEIGHT,
  clampDockHeight,
  consoleCwdForContext,
} from './lib/consoleDock.js';
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
  sessionJumpNoWorkspace,
  sessionJumpWorkspaceHash,
  sessionRefFromJumpTarget,
  stripOpenSessionParams,
} from './lib/sessionJump.js';

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

function homeRefFromWorkspace(workspace, fallbackRef, health) {
  const source = workspace && typeof workspace === 'object' ? workspace : {};
  const explicitWorkspace = !!(workspace && typeof workspace === 'object');
  const fallback = fallbackRef && typeof fallbackRef === 'object' ? fallbackRef : {};
  const noWorkspace = !!(source.noWorkspace || source.no_workspace);
  const workspaceHash = source.workspaceHash || source.workspace_hash || source.hash || fallback.workspaceHash || fallback.workspace_hash || '';
  const cwd = source.cwd || fallback.cwd || health?.cwd || '';
  const next = { home: true, homeWorkspaceExplicit: explicitWorkspace };
  if (noWorkspace) {
    next.noWorkspace = true;
    next.workspaceHash = '';
    next.cwd = '';
    return next;
  }
  if (workspaceHash) next.workspaceHash = workspaceHash;
  if (cwd) next.cwd = cwd;
  if (source.name || source.workspaceName) next.workspaceName = source.name || source.workspaceName;
  else if (fallback.workspaceName) next.workspaceName = fallback.workspaceName;
  for (const key of ['contextId', 'port', 'token']) {
    if (source[key] != null) next[key] = source[key];
    else if (fallback[key] != null) next[key] = fallback[key];
  }
  return next;
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
  const [searchOpen,   setSearchOpen]   = useState(false);
  const [updateStatus, setUpdateStatus] = useState(null);
  const [updateStarting, setUpdateStarting] = useState(false);
  const [singleLayout, setSingleLayout] = usePreference(
    SINGLE_LAYOUT_STORAGE_KEY, DEFAULT_SINGLE_LAYOUT, validateLayoutWidths);
  const [uiPrefs, setUiPrefs] = usePreference(
    UI_PREFS_STORAGE_KEY, DEFAULT_UI_PREFS, validateUiPrefs);
  const [consoleDock, setConsoleDock] = usePreference(
    CONSOLE_DOCK_STORAGE_KEY, DEFAULT_CONSOLE_DOCK, validateConsoleDock);
  // grid4/grid9 入口暂时隐藏:主界面固定单会话,避免旧 localStorage 把用户卡在未完善视图。
  const view = 'single';
  const fontSize = effectiveFontSize(uiPrefs);
  const sidePanelCollapsed = uiPrefs.sidePanelCollapsed;
  const sidePanelMaximized = !!uiPrefs.sidePanelMaximized;
  const projectSidebarCollapsed = !!uiPrefs.sidebarCollapsed;
  const showAceCodeAvatar = false;
  const singleShellRef = useRef(null);
  const sidebarResizeActiveRef = useRef(false);
  const [previewPanelVisible, setPreviewPanelVisible] = useState(false);
  const activeRefRef = useRef(activeRef);
  const navHistoryRef = useRef(navHistory);

  useEffect(() => initInactiveSelection(), []);
  useEffect(() => {
    document.documentElement.setAttribute('data-font-size', fontSize);
  }, [fontSize]);
  useEffect(() => { activeRefRef.current = activeRef; }, [activeRef]);
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

  useEffect(() => {
    if (authState !== 'ok') return undefined;
    let cancelled = false;
    api.getUpdateStatus()
      .then((status) => {
        if (!cancelled) setUpdateStatus(status);
      })
      .catch(() => {
        if (!cancelled) setUpdateStatus(null);
      });
    return () => {
      cancelled = true;
    };
  }, [authState]);

  // 解析 URL 上的 ?open=<sessionId>&workspace=<hash>(跨 workspace 跳转后落地用)。
  // 解析后立即从 URL 抹掉,避免刷新二次触发。
  useEffect(() => {
    if (typeof window === 'undefined') return;
    const target = openSessionTargetFromSearch(window.location.search);
    if (!target) return;
    const qs = stripOpenSessionParams(window.location.search);
    const newUrl = window.location.pathname + (qs ? '?' + qs : '');
    window.history.replaceState(null, '', newUrl);
    resumeAndOpenSession(target, { replace: true, allowDesktopActivate: false }).catch(() => {});
  }, [resumeAndOpenSession]);

  // 桌面壳通知 click_handler 走 webview eval 调这两个 window 全局函数,见
  // openspec/changes/add-desktop-attention-notifications。
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

  useEffect(() => {
    const pushUnique = (setter, payload) => {
      if (!payload?.request_id) return;
      setter((prev) => prev.some((x) => x.request_id === payload.request_id)
        ? prev
        : [...prev, payload]);
    };
    const handler = (e) => {
      const msg = e.detail || {};
      const payload = { ...(msg.payload || {}) };
      if (msg.session_id && !payload.session_id) payload.session_id = msg.session_id;
      if (msg.type === 'question_request' && !payload.session_id) {
        const current = activeRefRef.current || {};
        payload.session_id = current.sessionId || current.id || '';
      }
      if (msg.type === 'permission_request') pushUnique(setPermReqs, payload);
      if (msg.type === 'question_request') {
        setQuestionReqs((prev) => addPendingQuestionRequest(prev, payload));
      }
      if (msg.type === 'question_closed') {
        setQuestionReqs((prev) => removePendingQuestionRequest(prev, payload.request_id));
      }
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, []);

  const onSubmitToken = useCallback(async (token) => {
    setToken(token);
    await probe();
  }, [probe]);

  const toggleSidePanel = useCallback(() => {
    setUiPrefs((prev) => ({ ...prev, sidePanelCollapsed: !prev.sidePanelCollapsed }));
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

  const startUpdate = useCallback(async () => {
    if (!updateStatus?.update_available || updateStarting) return;
    setUpdateStarting(true);
    try {
      await api.startUpdate();
      toast({ kind: 'info', text: '已启动升级,请按提示完成并重启 ACECode' });
    } catch (e) {
      toast({ kind: 'err', text: '启动升级失败:' + (e?.message || '') });
    } finally {
      setUpdateStarting(false);
    }
  }, [updateStarting, updateStatus]);

  const openHomeForWorkspace = useCallback((workspace = null) => {
    navigateToRef(homeRefFromWorkspace(workspace, activeRefRef.current, health));
  }, [health, navigateToRef]);

  const createDesktopTraySession = useCallback(async () => {
    try {
      const next = await createNewSessionForActiveWorkspace(api, activeRefRef.current, health);
      navigateToRef(next);
    } catch (e) {
      toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
    }
  }, [health, navigateToRef]);

  const handleSubagentTasksChange = useCallback((info) => {
    setSubagentIndex({
      parentId: info?.parentId || '',
      titles: info?.titles && typeof info.titles === 'object' ? info.titles : {},
    });
  }, []);

  const handlePermissionModeChanged = useCallback(({ sessionId, mode }) => {
    if (mode !== 'yolo' || !sessionId) return;
    setPermReqs((prev) => prev.filter((req) => {
      const reqSid = req?.session_id || activeRef?.sessionId || activeRef?.id || '';
      return reqSid !== sessionId;
    }));
  }, [activeRef?.id, activeRef?.sessionId]);

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
    const sidePanelVisible = !!(activeRef?.sessionId || activeRef?.id) && !sidePanelCollapsed;
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
  }, [activeRef?.id, activeRef?.sessionId, previewPanelVisible, sidePanelCollapsed, setSingleLayout]);

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
    const sidePanelVisible = !!(activeRef?.sessionId || activeRef?.id) && !sidePanelCollapsed;
    setSingleLayout((prev) => {
      const previewPanel = normalizePreviewPanelWidth(nextWidth, {
        contentWidth,
        sidePanelWidth: prev.sidePanel,
        sidePanelVisible,
        sidePanelCollapsed,
      });
      return previewPanel === prev.previewPanel ? prev : { ...prev, previewPanel };
    });
  }, [activeRef?.id, activeRef?.sessionId, setSingleLayout, sidePanelCollapsed]);

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
  const activeRefConsoleCwd = consoleCwdForContext({ activeRef, health });
  const preferredConsoleCwd = activeId ? activeRefConsoleCwd : (consoleCwd || activeRefConsoleCwd);
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
        <GlobalFindOverlay />
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
        <GlobalFindOverlay />
        <DesktopContextMenu />
        <Toaster />
      </>
    );
  }

  const sidebarCollapsed = view !== 'single' || projectSidebarCollapsed;
  const permReq = permReqs[0] || null;
  const visibleQuestionReq = !permReq
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
        onOpenSearch={() => setSearchOpen(true)}
        onToggleConsole={toggleConsoleDock}
        consoleAvailable={consoleAvailable}
        consoleOpen={consoleDock.open}
        sidebarCollapsed={sidebarCollapsed}
        onToggleSidebar={toggleProjectSidebar}
        onGoBack={goBackActiveRef}
        onGoForward={goForwardActiveRef}
        canGoBack={navHistory.back.length > 0}
        canGoForward={navHistory.forward.length > 0}
        updateStatus={updateStatus}
        updateStarting={updateStarting}
        onStartUpdate={startUpdate}
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
          onOpenSettingsSection={openSettingsSection}
          pendingQuestionSessionIds={pendingQuestionSessionIdsForSidebar}
        />
        {view === 'single' && !projectSidebarCollapsed && (
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
            {view === 'single' && (
              <ChatView
                sessionRef={activeRef}
                onSessionPromoted={navigateToRef}
                onCommandWorkspaceChange={setCommandWorkspaceHash}
                onConsoleCwdChange={setConsoleCwd}
                health={health}
                showSidePanel
                sidePanelWidth={singleLayout.sidePanel}
                onSidePanelResize={setSidePanelWidth}
                previewPanelWidth={singleLayout.previewPanel}
                onPreviewPanelResize={setPreviewPanelWidth}
                onPreviewPanelVisibleChange={setPreviewPanelVisible}
                sidePanelCollapsed={sidePanelCollapsed}
                onToggleSidePanel={toggleSidePanel}
                sidePanelMaximized={sidePanelMaximized}
                onToggleSidePanelMaximized={toggleSidePanelMaximized}
                showAceCodeAvatar={showAceCodeAvatar}
                questionRequest={visibleQuestionReq}
                onQuestionResolve={resolveVisibleQuestion}
                onPermissionModeChanged={handlePermissionModeChanged}
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
            onPermissionModeChanged={handlePermissionModeChanged}
            fontSize={fontSize}
            onFontSizeChange={setFontSize}
          />
        )}
        <SearchPalette
          open={searchOpen}
          onClose={() => setSearchOpen(false)}
          currentWorkspaceHash={activeRef?.workspaceHash || ''}
          onSelectSession={handleSelectSession}
        />
        {permReq      && (
          <PermissionModal
            request={permReq}
            originLabel={permReq.session_id && subagentIndex.titles[permReq.session_id]
              ? `来自后台任务:${subagentIndex.titles[permReq.session_id]}`
              : ''}
            onResolve={() => setPermReqs((prev) => prev.slice(1))}
          />
        )}
      </div>
      <FramelessResizeHandles />
      <GlobalFindOverlay />
      <DesktopContextMenu />
      <Toaster />
    </div>
    </SlashCommandsProvider>
  );
}
