// TopBar:logo + 快捷工具(项目栏收缩/新对话/搜索/···) + 中央 single/grid4/grid9 pill + 主题切换 + 设置
//
// 设计稿方向 C 的中央 pill 直接接在这里;占位按钮(搜索/···)留 hover 反馈但 onClick
// 暂为 toast 提醒"待开发",避免点了"无反应"。

import { useTheme } from '../theme.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

const VIEWS = [
  { key: 'single', label: '单会话' },
  { key: 'grid4',  label: '4宫格' },
  { key: 'grid9',  label: '9宫格' },
];

function QuickBtn({ title, onClick, children }) {
  return (
    <button
      type="button"
      title={title}
      onClick={onClick}
      className="w-7 h-7 rounded-md bg-surface-hi/0 hover:bg-surface-hi text-fg-2 hover:text-fg flex items-center justify-center text-[14px] transition"
    >
      {children}
    </button>
  );
}

function isFramelessDesktop() {
  return typeof window !== 'undefined'
    && window.__ACECODE_FRAMELESS_WINDOW__ === true
    && typeof window.aceDesktop_startWindowDrag === 'function';
}

function isInteractiveTarget(target) {
  return !!target?.closest?.('button,a,input,textarea,select,[role="button"],[data-ace-no-window-drag="true"]');
}

function WindowGlyph({ type }) {
  if (type === 'minimize') {
    return (
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <path d="M3.5 10.5h9" />
      </svg>
    );
  }
  if (type === 'maximize') {
    return (
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <rect x="4.25" y="4.25" width="7.5" height="7.5" rx="1" />
      </svg>
    );
  }
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
      <path d="M4.25 4.25l7.5 7.5M11.75 4.25l-7.5 7.5" />
    </svg>
  );
}

function WindowControl({ type, title, onClick }) {
  return (
    <button
      type="button"
      data-ace-no-window-drag="true"
      title={title}
      aria-label={title}
      onClick={onClick}
      className={clsx('ace-window-control', type === 'close' && 'ace-window-control-close')}
    >
      <WindowGlyph type={type} />
    </button>
  );
}

export function TopBar({ view, onViewChange, onSettings, onNewSession, onOpenSearch, sidebarCollapsed = false, onToggleSidebar }) {
  const { theme, toggle } = useTheme();
  const framelessDesktop = isFramelessDesktop();
  const isMac = typeof navigator !== 'undefined' && /Mac|iPhone|iPad|iPod/.test(navigator.platform || '');
  const searchHotkeyHint = isMac ? '搜索 (Cmd+K)' : '搜索 (Ctrl+K)';

  const onTopBarMouseDown = (event) => {
    if (!framelessDesktop || event.button !== 0 || isInteractiveTarget(event.target)) return;
    event.preventDefault();
    if (event.detail >= 2 && typeof window.aceDesktop_toggleMaximizeWindow === 'function') {
      window.aceDesktop_toggleMaximizeWindow();
      return;
    }
    window.aceDesktop_startWindowDrag();
  };

  return (
    <div
      className={clsx(
        'h-11 px-3 flex items-center gap-2 bg-surface border-b border-border relative z-10 shrink-0',
        framelessDesktop && 'ace-desktop-frameless-topbar',
      )}
      onMouseDown={onTopBarMouseDown}
    >
      <div className="flex items-center gap-1.5 select-none mr-1">
        <img src="/acecode-logo.png" alt="" width="20" height="20" className="block" draggable="false" />
        <span className="text-[15px] font-bold tracking-tight">ACECode</span>
      </div>

      <QuickBtn title={sidebarCollapsed ? '展开项目栏' : '收起项目栏'} onClick={onToggleSidebar}>
        <VsIcon
          name="expandRight"
          size={15}
          className={clsx('transition-transform', !sidebarCollapsed && 'rotate-180')}
        />
      </QuickBtn>
      <QuickBtn title="新对话" onClick={onNewSession}>
        <VsIcon name="editWindow" size={16} />
      </QuickBtn>
      <QuickBtn title={searchHotkeyHint} onClick={onOpenSearch}>
        <VsIcon name="search" size={14} />
      </QuickBtn>
      <QuickBtn title="更多" onClick={() => toast({ kind: 'info', text: '更多工具待补充' })}>
        <VsIcon name="ellipsis" size={14} />
      </QuickBtn>

      {/* 中央 pill */}
      <div className="absolute left-1/2 -translate-x-1/2 flex bg-surface-hi rounded-full p-[3px]">
        {VIEWS.map((v) => (
          <button
            key={v.key}
            type="button"
            onClick={() => onViewChange(v.key)}
            className={clsx(
              'px-4 py-[3px] rounded-full text-[12px] font-normal transition-all',
              view === v.key
                ? 'bg-surface text-accent font-semibold ace-shadow'
                : 'text-fg-mute hover:text-fg-2',
            )}
          >
            {v.label}
          </button>
        ))}
      </div>

      <div className="ml-auto flex items-center gap-1">
        <QuickBtn title={theme === 'dark' ? '切到浅色' : '切到深色'} onClick={toggle}>
          <VsIcon name={theme === 'dark' ? 'brightness' : 'darkTheme'} size={14} />
        </QuickBtn>
        <button
          type="button"
          onClick={onSettings}
          className="px-2.5 py-1 rounded-md text-[12px] text-fg-mute hover:text-fg hover:bg-surface-hi transition flex items-center gap-1.5"
        >
          <VsIcon name="settings" size={20} />
          <span>设置</span>
        </button>
        {framelessDesktop && (
          <div className="ace-window-controls" data-ace-no-window-drag="true">
            <WindowControl type="minimize" title="最小化" onClick={() => window.aceDesktop_minimizeWindow?.()} />
            <WindowControl type="maximize" title="最大化或还原" onClick={() => window.aceDesktop_toggleMaximizeWindow?.()} />
            <WindowControl type="close" title="关闭" onClick={() => window.aceDesktop_closeWindow?.()} />
          </div>
        )}
      </div>
    </div>
  );
}
