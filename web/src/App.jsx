// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 270px Sidebar + 主区(单会话/4宫格/9宫格)
// + 22px StatusBar。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api, ApiError } from './lib/api.js';
import { setToken } from './lib/auth.js';
import { connection } from './lib/connection.js';
import { createNewSessionForActiveWorkspace } from './lib/newSession.js';
import { usePreference } from './lib/usePreference.js';
import {
  DEFAULT_UI_PREFS,
  UI_PREFS_STORAGE_KEY,
  effectiveShowAceCodeAvatar,
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

const SINGLE_LAYOUT_STORAGE_KEY = 'acecode.singleLayoutWidths.v1';
const LEGACY_DEFAULT_SINGLE_LAYOUT = { sidebar: 200, sidePanel: 280 };
const DEFAULT_SINGLE_LAYOUT = { sidebar: 270, sidePanel: 280 };
const MIN_SIDEBAR_WIDTH = 160;
const MAX_SIDEBAR_WIDTH = 360;
const MIN_SIDE_PANEL_WIDTH = 240;
const MAX_SIDE_PANEL_WIDTH = 560;
const MIN_CHAT_WIDTH = 360;

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function validateLayoutWidths(v) {
  return v && typeof v === 'object'
    && typeof v.sidebar === 'number' && Number.isFinite(v.sidebar)
    && typeof v.sidePanel === 'number' && Number.isFinite(v.sidePanel);
}

function homeRefFromWorkspace(workspace, fallbackRef, health) {
  const source = workspace && typeof workspace === 'object' ? workspace : {};
  const fallback = fallbackRef && typeof fallbackRef === 'object' ? fallbackRef : {};
  const workspaceHash = source.workspaceHash || source.workspace_hash || source.hash || fallback.workspaceHash || fallback.workspace_hash || '';
  const cwd = source.cwd || fallback.cwd || health?.cwd || '';
  const next = { home: true };
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
  const [commandWorkspaceHash, setCommandWorkspaceHash] = useState('');
  const [showSettings, setShowSettings] = useState(false);
  const [permReqs,     setPermReqs]     = useState([]);
  const [questionReqs, setQuestionReqs] = useState([]);
  const [searchOpen,   setSearchOpen]   = useState(false);
  const [singleLayout, setSingleLayout] = usePreference(
    SINGLE_LAYOUT_STORAGE_KEY, DEFAULT_SINGLE_LAYOUT, validateLayoutWidths);
  const [uiPrefs, setUiPrefs] = usePreference(
    UI_PREFS_STORAGE_KEY, DEFAULT_UI_PREFS, validateUiPrefs);
  // grid4/grid9 入口暂时隐藏:主界面固定单会话,避免旧 localStorage 把用户卡在未完善视图。
  const view = 'single';
  const sidePanelCollapsed = uiPrefs.sidePanelCollapsed;
  const sidePanelMaximized = !!uiPrefs.sidePanelMaximized;
  const projectSidebarCollapsed = !!uiPrefs.sidebarCollapsed;
  const showAceCodeAvatar = effectiveShowAceCodeAvatar(uiPrefs);
  const singleShellRef = useRef(null);
  const sidebarResizeActiveRef = useRef(false);

  useEffect(() => {
    setSingleLayout((prev) => {
      if (prev?.sidebar === LEGACY_DEFAULT_SINGLE_LAYOUT.sidebar
          && prev?.sidePanel === LEGACY_DEFAULT_SINGLE_LAYOUT.sidePanel) {
        return DEFAULT_SINGLE_LAYOUT;
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

  // 解析 URL 上的 ?open=<sessionId>(SearchPalette 跨 workspace 跳转后落地用)。
  // 解析后立即从 URL 抹掉,避免刷新二次触发。
  useEffect(() => {
    if (typeof window === 'undefined') return;
    const params = new URLSearchParams(window.location.search);
    const openId = params.get('open');
    if (!openId) return;
    setActiveRef((prev) => ({ ...(prev || {}), sessionId: openId }));
    params.delete('open');
    const qs = params.toString();
    const newUrl = window.location.pathname + (qs ? '?' + qs : '');
    window.history.replaceState(null, '', newUrl);
  }, []);

  // 桌面壳通知 click_handler 走 webview eval 调这两个 window 全局函数,见
  // openspec/changes/add-desktop-attention-notifications。
  // 同 workspace:focusSessionFromBridge → setActiveRef
  // 跨 workspace:activateAndOpenSession → 复用 SearchPalette 已有的整页 navigate 逻辑
  useEffect(() => {
    if (typeof window === 'undefined') return;
    window.aceDesktop_focusSessionFromBridge = (sessionId) => {
      if (!sessionId) return;
      setActiveRef((prev) => ({ ...(prev || {}), sessionId: String(sessionId) }));
    };
    window.aceDesktop_activateAndOpenSession = async (workspaceHash, sessionId) => {
      if (!sessionId) return;
      const targetHash = workspaceHash || '';
      if (targetHash && typeof window.aceDesktop_activateWorkspace === 'function') {
        try {
          const raw = await window.aceDesktop_activateWorkspace(targetHash);
          const r = typeof raw === 'string' ? JSON.parse(raw) : raw;
          if (r && !r.error && r.port && r.token) {
            const url = `http://127.0.0.1:${r.port}/?token=${encodeURIComponent(r.token)}&open=${encodeURIComponent(sessionId)}`;
            window.location.href = url;
            return;
          }
        } catch {
          // 降级:直接 setActiveRef
        }
      }
      setActiveRef({ workspaceHash: targetHash, sessionId: String(sessionId) });
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
  }, []);

  // 全局 Ctrl/Cmd+K 切换搜索面板。matchShortcut 处理大小写与修饰键。
  useGlobalShortcut(
    (e) => e.key && e.key.toLowerCase() === 'k' && (e.ctrlKey || e.metaKey),
    () => setSearchOpen((o) => !o),
    [],
  );

  const handleSelectSession = useCallback(async (session) => {
    if (!session?.id) return;
    setSearchOpen(false);
    const targetHash = session.workspace_hash || '';
    const sameWorkspace = !targetHash || targetHash === activeRef?.workspaceHash;

    if (!sameWorkspace
        && typeof window !== 'undefined'
        && typeof window.aceDesktop_activateWorkspace === 'function') {
      try {
        const raw = await window.aceDesktop_activateWorkspace(targetHash);
        const r = typeof raw === 'string' ? JSON.parse(raw) : raw;
        if (r && !r.error && r.port && r.token) {
          const url = `http://127.0.0.1:${r.port}/?token=${encodeURIComponent(r.token)}&open=${encodeURIComponent(session.id)}`;
          window.location.href = url;
          return;
        }
      } catch {
        // 降级:无 bridge 或 bridge 失败时按浏览器直访模式跳。
      }
    }

    setActiveRef({
      workspaceHash: targetHash,
      contextId: 'default',
      sessionId: session.id,
      displayTitle: session.displayTitle || session.display_title,
      cwd: session.cwd || '',
      title: session.title,
      summary: session.summary,
      message_count: session.message_count,
      created_at: session.created_at,
      updated_at: session.updated_at,
    });
  }, [activeRef?.workspaceHash]);

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
      if (msg.type === 'permission_request') pushUnique(setPermReqs, payload);
      if (msg.type === 'question_request') pushUnique(setQuestionReqs, payload);
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

  // 最大化 / 还原右侧面板。最大化时强制确保 SidePanel 处于"未折叠"状态,
  // 否则用户进入最大化后看到一片空白(panel 宽 0 + 聊天区已隐藏)。
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

  const setShowAceCodeAvatar = useCallback((show) => {
    setUiPrefs({ showAceCodeAvatar: !!show });
  }, [setUiPrefs]);

  const openHomeForWorkspace = useCallback((workspace = null) => {
    setActiveRef(homeRefFromWorkspace(workspace, activeRef, health));
  }, [activeRef, health]);

  const createDesktopTraySession = useCallback(async () => {
    try {
      const next = await createNewSessionForActiveWorkspace(api, activeRef, health);
      setActiveRef(next);
    } catch (e) {
      toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
    }
  }, [activeRef, health]);

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
      const sidePanelReserve = sidePanelVisible ? prev.sidePanel : 0;
      const maxByShell = shellWidth > 0
        ? Math.max(MIN_SIDEBAR_WIDTH, shellWidth - sidePanelReserve - MIN_CHAT_WIDTH)
        : MAX_SIDEBAR_WIDTH;
      const sidebar = clamp(Math.round(nextWidth), MIN_SIDEBAR_WIDTH, Math.min(MAX_SIDEBAR_WIDTH, maxByShell));
      return sidebar === prev.sidebar ? prev : { ...prev, sidebar };
    });
  }, [activeRef?.id, activeRef?.sessionId, sidePanelCollapsed, setSingleLayout]);

  const setSidePanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    setSingleLayout((prev) => {
      const maxByContent = contentWidth > 0
        ? Math.max(MIN_SIDE_PANEL_WIDTH, contentWidth - MIN_CHAT_WIDTH)
        : MAX_SIDE_PANEL_WIDTH;
      const sidePanel = clamp(Math.round(nextWidth), MIN_SIDE_PANEL_WIDTH, Math.min(MAX_SIDE_PANEL_WIDTH, maxByContent));
      return sidePanel === prev.sidePanel ? prev : { ...prev, sidePanel };
    });
  }, [setSingleLayout]);

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

  const activeId = activeRef?.sessionId || activeRef?.id || '';
  const sidebarCollapsed = view !== 'single' || projectSidebarCollapsed;
  const permReq = permReqs[0] || null;
  const visibleQuestionReq = !permReq
    ? questionReqs.find((req) => {
        const reqSid = req?.session_id || '';
        return !reqSid || (activeId && reqSid === activeId);
      }) || null
    : null;
  const resolveVisibleQuestion = () => {
    if (!visibleQuestionReq?.request_id) return;
    setQuestionReqs((prev) => prev.filter((req) => req.request_id !== visibleQuestionReq.request_id));
  };

  return (
    <SlashCommandsProvider workspaceHash={commandWorkspaceHash}>
    <div className="h-full w-full flex flex-col bg-bg text-fg font-sans">
      <TopBar
        onSettings={() => setShowSettings(true)}
        onNewSession={() => openHomeForWorkspace()}
        onOpenSearch={() => setSearchOpen(true)}
        sidebarCollapsed={sidebarCollapsed}
        onToggleSidebar={toggleProjectSidebar}
      />
      <div ref={singleShellRef} className="flex-1 flex overflow-hidden relative min-h-0 ace-single-shell">
        <Sidebar
          activeId={activeId}
          onSelect={setActiveRef}
          collapsed={sidebarCollapsed}
          width={singleLayout.sidebar}
          onOpenHome={openHomeForWorkspace}
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
            'flex-1 flex overflow-hidden transition-all duration-200',
            'opacity-100 scale-100',
          ].join(' ')}
        >
          {view === 'single' && (
            <ChatView
              sessionRef={activeRef}
              onSessionPromoted={setActiveRef}
              onCommandWorkspaceChange={setCommandWorkspaceHash}
              health={health}
              showSidePanel
              sidePanelWidth={singleLayout.sidePanel}
              onSidePanelResize={setSidePanelWidth}
              sidePanelCollapsed={sidePanelCollapsed}
              onToggleSidePanel={toggleSidePanel}
              sidePanelMaximized={sidePanelMaximized}
              onToggleSidePanelMaximized={toggleSidePanelMaximized}
              showAceCodeAvatar={showAceCodeAvatar}
              questionRequest={visibleQuestionReq}
              onQuestionResolve={resolveVisibleQuestion}
              onPermissionModeChanged={handlePermissionModeChanged}
            />
          )}
        </div>
        {showSettings && (
          <SettingsPage
            onClose={() => setShowSettings(false)}
            health={health}
            activeSessionId={activeId}
            onPermissionModeChanged={handlePermissionModeChanged}
            showAceCodeAvatar={showAceCodeAvatar}
            onShowAceCodeAvatarChanged={setShowAceCodeAvatar}
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
            onResolve={() => setPermReqs((prev) => prev.slice(1))}
          />
        )}
      </div>
      <FramelessResizeHandles />
      <DesktopContextMenu />
      <Toaster />
    </div>
    </SlashCommandsProvider>
  );
}
