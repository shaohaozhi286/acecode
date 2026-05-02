// 顶层 App:鉴权 gate(401 → TokenPrompt)+ 主壳。
//
// 视觉对齐设计稿方向 C:顶部 44px TopBar + 200px Sidebar + 主区(单会话/4宫格/9宫格)
// + 22px StatusBar。所有面板/弹框作为 overlay 渲染在主区之上。

import { useCallback, useEffect, useState } from 'react';
import { api, ApiError } from './lib/api.js';
import { setToken } from './lib/auth.js';
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
  const [permReq,      setPermReq]      = useState(null);
  const [questionReq,  setQuestionReq]  = useState(null);

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

  return (
    <div className="h-full w-full flex flex-col bg-bg text-fg font-sans">
      <TopBar
        view={view}
        onViewChange={switchView}
        onSettings={() => setShowSettings(true)}
        onNewSession={() => window.dispatchEvent(new CustomEvent('ace:new-session'))}
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
              onPermissionRequest={setPermReq}
              onQuestionRequest={setQuestionReq}
            />
          )}
          {view === 'grid4' && <Grid4View onExpand={setExpanded} />}
          {view === 'grid9' && <Grid9View onExpand={setExpanded} />}
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
            onResolve={() => setPermReq(null)}
          />
        )}
        {questionReq  && (
          <QuestionModal
            request={questionReq}
            onResolve={() => setQuestionReq(null)}
          />
        )}
      </div>
      <Toaster />
    </div>
  );
}
