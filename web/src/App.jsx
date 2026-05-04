// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 200px Sidebar + 主区(单会话/4宫格/9宫格)
// + 22px StatusBar。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api, ApiError } from './lib/api.js';
import { setToken } from './lib/auth.js';
import { connection } from './lib/connection.js';
import { TopBar } from './components/TopBar.jsx';
import { Sidebar } from './components/Sidebar.jsx';
import { ChatView } from './components/ChatView.jsx';
import { Grid4View } from './components/Grid4View.jsx';
import { Grid9View } from './components/Grid9View.jsx';
import { ExpandedOverlay } from './components/ExpandedOverlay.jsx';
import { TokenPrompt } from './components/TokenPrompt.jsx';
import { PermissionModal } from './components/PermissionModal.jsx';
import { SkillsPanel } from './components/SkillsPanel.jsx';
import { MCPPanel } from './components/MCPPanel.jsx';
import { SettingsPage } from './components/SettingsPage.jsx';
import { Toaster, toast } from './components/Toast.jsx';

function newSessionRefFrom(ref, sessionId) {
  const next = { sessionId };
  if (!ref || typeof ref !== 'object') return next;
  for (const key of ['workspaceHash', 'contextId', 'port', 'token', 'cwd']) {
    if (ref[key] != null) next[key] = ref[key];
  }
  return next;
}

const SINGLE_LAYOUT_STORAGE_KEY = 'acecode.singleLayoutWidths.v1';
const DEFAULT_SINGLE_LAYOUT = { sidebar: 200, sidePanel: 280 };
const MIN_SIDEBAR_WIDTH = 160;
const MAX_SIDEBAR_WIDTH = 360;
const MIN_SIDE_PANEL_WIDTH = 240;
const MAX_SIDE_PANEL_WIDTH = 560;
const MIN_CHAT_WIDTH = 360;

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function readSingleLayoutWidths() {
  try {
    const raw = window.localStorage.getItem(SINGLE_LAYOUT_STORAGE_KEY);
    const parsed = raw ? JSON.parse(raw) : null;
    return {
      sidebar: clamp(Number(parsed?.sidebar) || DEFAULT_SINGLE_LAYOUT.sidebar, MIN_SIDEBAR_WIDTH, MAX_SIDEBAR_WIDTH),
      sidePanel: clamp(Number(parsed?.sidePanel) || DEFAULT_SINGLE_LAYOUT.sidePanel, MIN_SIDE_PANEL_WIDTH, MAX_SIDE_PANEL_WIDTH),
    };
  } catch {
    return DEFAULT_SINGLE_LAYOUT;
  }
}

export function App() {
  const [authState, setAuthState] = useState('checking'); // 'checking' | 'ok' | 'need-token'
  const [health,    setHealth]    = useState(null);

  const [view,         setView]         = useState('single');
  const [activeRef,    setActiveRef]    = useState(null);
  const [transition,   setTransition]   = useState(false);
  const [expanded,     setExpanded]     = useState(null);
  const [showSkills,   setShowSkills]   = useState(false);
  const [showMcp,      setShowMcp]      = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const [permReqs,     setPermReqs]     = useState([]);
  const [questionReqs, setQuestionReqs] = useState([]);
  const [singleLayout, setSingleLayout] = useState(readSingleLayoutWidths);
  const singleShellRef = useRef(null);
  const sidebarResizeActiveRef = useRef(false);

  useEffect(() => {
    try { window.localStorage.setItem(SINGLE_LAYOUT_STORAGE_KEY, JSON.stringify(singleLayout)); }
    catch { /* ignore storage failures */ }
  }, [singleLayout]);

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

  const switchView = useCallback((next) => {
    if (next === view) return;
    setTransition(true);
    setTimeout(() => {
      setView(next);
      setExpanded(null);
      setTimeout(() => setTransition(false), 220);
    }, 140);
  }, [view]);

  const createNewSession = useCallback(async () => {
    try {
      const r = activeRef?.workspaceHash
        ? await api.createWorkspaceSession(activeRef.workspaceHash, {})
        : await api.createSession({});
      const id = r && (r.session_id || r.id);
      if (!id) return;
      setActiveRef({ ...newSessionRefFrom(activeRef, id), workspaceHash: r.workspace_hash || activeRef?.workspaceHash, cwd: r.cwd || activeRef?.cwd });
      if (view !== 'single') switchView('single');
    } catch (e) {
      toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
    }
  }, [activeRef, switchView, view]);

  const setSidebarWidth = useCallback((nextWidth, shellWidth = 0) => {
    const sidePanelVisible = !!(activeRef?.sessionId || activeRef?.id);
    setSingleLayout((prev) => {
      const sidePanelReserve = sidePanelVisible ? prev.sidePanel : 0;
      const maxByShell = shellWidth > 0
        ? Math.max(MIN_SIDEBAR_WIDTH, shellWidth - sidePanelReserve - MIN_CHAT_WIDTH)
        : MAX_SIDEBAR_WIDTH;
      const sidebar = clamp(Math.round(nextWidth), MIN_SIDEBAR_WIDTH, Math.min(MAX_SIDEBAR_WIDTH, maxByShell));
      return sidebar === prev.sidebar ? prev : { ...prev, sidebar };
    });
  }, [activeRef?.id, activeRef?.sessionId]);

  const setSidePanelWidth = useCallback((nextWidth, contentWidth = 0) => {
    setSingleLayout((prev) => {
      const maxByContent = contentWidth > 0
        ? Math.max(MIN_SIDE_PANEL_WIDTH, contentWidth - MIN_CHAT_WIDTH)
        : MAX_SIDE_PANEL_WIDTH;
      const sidePanel = clamp(Math.round(nextWidth), MIN_SIDE_PANEL_WIDTH, Math.min(MAX_SIDE_PANEL_WIDTH, maxByContent));
      return sidePanel === prev.sidePanel ? prev : { ...prev, sidePanel };
    });
  }, []);

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
      <div className="h-full flex items-center justify-center text-fg-mute text-sm">
        <span className="ace-spinner mr-2" /> 连接 daemon…
      </div>
    );
  }
  if (authState === 'need-token') {
    return <TokenPrompt onSubmit={onSubmitToken} />;
  }

  const activeId = activeRef?.sessionId || activeRef?.id || '';
  const sidebarCollapsed = view !== 'single';
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
    <div className="h-full w-full flex flex-col bg-bg text-fg font-sans">
      <TopBar
        view={view}
        onViewChange={switchView}
        onSettings={() => setShowSettings(true)}
        onNewSession={createNewSession}
      />
      <div ref={singleShellRef} className="flex-1 flex overflow-hidden relative min-h-0 ace-single-shell">
        <Sidebar
          activeId={activeId}
          onSelect={setActiveRef}
          collapsed={sidebarCollapsed}
          width={singleLayout.sidebar}
          onOpenSkills={() => setShowSkills(true)}
          onOpenMcp={() => setShowMcp(true)}
        />
        {view === 'single' && (
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
            transition ? 'opacity-0 scale-[.985]' : 'opacity-100 scale-100',
          ].join(' ')}
        >
          {view === 'single' && (
            <ChatView
              sessionRef={activeRef}
              onSessionPromoted={setActiveRef}
              health={health}
              showSidePanel
              sidePanelWidth={singleLayout.sidePanel}
              onSidePanelResize={setSidePanelWidth}
              questionRequest={visibleQuestionReq}
              onQuestionResolve={resolveVisibleQuestion}
            />
          )}
          {view === 'grid4' && <Grid4View activeRef={activeRef} onExpand={setExpanded} />}
          {view === 'grid9' && <Grid9View activeRef={activeRef} onExpand={setExpanded} />}
        </div>

        {expanded && (
          <ExpandedOverlay session={expanded} onClose={() => setExpanded(null)} />
        )}
        {showSkills   && <SkillsPanel  onClose={() => setShowSkills(false)} />}
        {showMcp      && <MCPPanel     onClose={() => setShowMcp(false)} />}
        {showSettings && <SettingsPage onClose={() => setShowSettings(false)} health={health} />}
        {permReq      && (
          <PermissionModal
            request={permReq}
            onResolve={() => setPermReqs((prev) => prev.slice(1))}
          />
        )}
      </div>
      <Toaster />
    </div>
  );
}
