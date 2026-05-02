// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 200px Sidebar + 主区(单会话/4宫格/9宫格)
// + 22px StatusBar。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useState } from 'react';
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
import { QuestionModal } from './components/QuestionModal.jsx';
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
  const questionReq = questionReqs[0] || null;

  return (
    <div className="h-full w-full flex flex-col bg-bg text-fg font-sans">
      <TopBar
        view={view}
        onViewChange={switchView}
        onSettings={() => setShowSettings(true)}
        onNewSession={createNewSession}
      />
      <div className="flex-1 flex overflow-hidden relative min-h-0">
        <Sidebar
          activeId={activeId}
          onSelect={setActiveRef}
          collapsed={sidebarCollapsed}
          onOpenSkills={() => setShowSkills(true)}
          onOpenMcp={() => setShowMcp(true)}
        />
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
        {questionReq  && (
          <QuestionModal
            request={questionReq}
            onResolve={() => setQuestionReqs((prev) => prev.slice(1))}
          />
        )}
      </div>
      <Toaster />
    </div>
  );
}
