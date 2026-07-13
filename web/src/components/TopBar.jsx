// TopBar:logo + 快捷工具(项目栏收缩/新对话/搜索/快捷菜单) + 主题切换 + 设置

import { useEffect, useRef, useState } from 'react';
import { useTheme } from '../theme.jsx';
import { clsx } from '../lib/format.js';
import { TOPBAR_QUICK_ACTIONS, invokeTopBarQuickAction } from '../lib/topBarQuickActions.js';
import { formatProgramVersion } from '../lib/webCoreInfo.js';
import { NavigationArrowIcon, PanelToggleIcon, VsIcon } from './Icon.jsx';
import {
  WindowControls,
  isFramelessDesktop,
  isInteractiveTarget,
  nativePointerEvent,
  useFramelessWindowState,
} from './WindowControls.jsx';

function QuickBtn({
  title,
  onClick,
  children,
  disabled = false,
  className = '',
  pressed = null,
  ...buttonProps
}) {
  const isToggle = typeof pressed === 'boolean';

  return (
    <button
      {...buttonProps}
      type="button"
      title={title}
      aria-label={title}
      aria-pressed={isToggle ? pressed : undefined}
      onClick={onClick}
      disabled={disabled}
      className={clsx(
        'w-7 h-7 rounded-md bg-surface-hi/0 text-fg-2 flex items-center justify-center text-[14px] transition',
        isToggle && 'ace-topbar-toggle-btn',
        disabled ? 'opacity-35 cursor-not-allowed' : 'hover:bg-surface-hi hover:text-fg',
        className,
      )}
    >
      {children}
    </button>
  );
}

export function TopBar({
  onSettings,
  onNewSession,
  onOpenLoop,
  onOpenSearch,
  onToggleConsole,
  consoleAvailable = false,
  consoleOpen = false,
  sidebarCollapsed = false,
  onToggleSidebar,
  onGoBack,
  onGoForward,
  canGoBack = false,
  canGoForward = false,
  updateStatus = null,
  updateStarting = false,
  updateRunning = false,
  updateReady = false,
  onStartUpdate,
  appVersion = '',
}) {
  const { theme, toggle } = useTheme();
  const { framelessDesktop, isMaximized } = useFramelessWindowState();
  const [quickActionsOpen, setQuickActionsOpen] = useState(false);
  const quickActionsRef = useRef(null);
  const isMac = typeof navigator !== 'undefined' && /Mac|iPhone|iPad|iPod/.test(navigator.platform || '');
  const searchHotkeyHint = isMac ? '搜索 (Cmd+K)' : '搜索 (Ctrl+K)';
  const updateAvailable = !!updateStatus?.update_available;
  const updateTitle = updateAvailable
    ? updateReady
      ? '升级已安装，点击查看重启提示'
      : updateRunning
      ? '升级正在进行，点击查看进度'
      : `发现新版 v${updateStatus.latest_version || ''}, 点击升级`
    : '';
  const cleanAppVersion = typeof appVersion === 'string' ? appVersion.trim() : '';
  const appVersionLabel = cleanAppVersion ? formatProgramVersion(cleanAppVersion) : '';
  const selectQuickAction = (actionId) => {
    setQuickActionsOpen(false);
    invokeTopBarQuickAction(actionId, {
      onNewSession,
      onOpenLoop,
      onOpenSearch,
      onSettings,
    });
  };

  useEffect(() => {
    if (!quickActionsOpen) return undefined;
    const onDocumentMouseDown = (event) => {
      if (!quickActionsRef.current?.contains(event.target)) setQuickActionsOpen(false);
    };
    const onDocumentKeyDown = (event) => {
      if (event.key !== 'Escape') return;
      setQuickActionsOpen(false);
      quickActionsRef.current?.querySelector('button')?.focus();
    };
    document.addEventListener('mousedown', onDocumentMouseDown);
    document.addEventListener('keydown', onDocumentKeyDown);
    return () => {
      document.removeEventListener('mousedown', onDocumentMouseDown);
      document.removeEventListener('keydown', onDocumentKeyDown);
    };
  }, [quickActionsOpen]);

  const onTopBarMouseDown = (event) => {
    if (!framelessDesktop || event.button !== 0 || isInteractiveTarget(event.target)) return;
    event.preventDefault();
    if (event.detail >= 2 && typeof window.aceDesktop_toggleMaximizeWindow === 'function') {
      window.aceDesktop_toggleMaximizeWindow();
      return;
    }
    window.aceDesktop_startWindowDrag(nativePointerEvent(event));
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
        {appVersionLabel && (
          <span className="text-[11px] font-medium leading-none text-fg-mute opacity-75 tabular-nums">
            {appVersionLabel}
          </span>
        )}
      </div>

      <QuickBtn
        title={sidebarCollapsed ? '展开项目栏' : '收起项目栏'}
        onClick={onToggleSidebar}
        pressed={!sidebarCollapsed}
      >
        <PanelToggleIcon side="left" size={16} />
      </QuickBtn>
      <QuickBtn title="后退" onClick={onGoBack} disabled={!canGoBack}>
        <NavigationArrowIcon direction="back" size={16} />
      </QuickBtn>
      <QuickBtn title="前进" onClick={onGoForward} disabled={!canGoForward}>
        <NavigationArrowIcon direction="forward" size={16} />
      </QuickBtn>
      {updateAvailable && (
        <button
          type="button"
          title={updateTitle}
          aria-label={updateTitle || '更新'}
          onClick={onStartUpdate}
          disabled={updateStarting}
          className={clsx(
            'h-7 min-w-[44px] px-3 rounded-full bg-accent text-white text-[12px] font-semibold leading-none shadow-sm transition',
            'hover:opacity-90 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/20',
            updateStarting && 'opacity-60 cursor-not-allowed hover:opacity-60',
          )}
        >
          {updateReady ? '已更新' : updateRunning ? '更新中' : '更新'}
        </button>
      )}
      <QuickBtn data-tour-target="topbar-new-session" title="新对话" onClick={onNewSession}>
        <VsIcon name="newSession" size={16} />
      </QuickBtn>
      <QuickBtn title="循环" onClick={onOpenLoop}>
        <VsIcon name="alarm" size={16} />
      </QuickBtn>
      <QuickBtn title={searchHotkeyHint} onClick={onOpenSearch}>
        <VsIcon name="search" size={14} />
      </QuickBtn>
      <div ref={quickActionsRef} className="relative">
        <QuickBtn
          title="更多操作"
          onClick={() => setQuickActionsOpen((open) => !open)}
          pressed={quickActionsOpen}
          aria-haspopup="menu"
          aria-expanded={quickActionsOpen}
          aria-controls="topbar-quick-actions-menu"
        >
          <VsIcon name="ellipsis" size={14} />
        </QuickBtn>
        {quickActionsOpen && (
          <div
            id="topbar-quick-actions-menu"
            role="menu"
            aria-label="快捷操作"
            className="absolute top-full left-0 mt-1 w-[148px] rounded-lg border border-border bg-surface p-1 ace-shadow-lg z-50"
          >
            {TOPBAR_QUICK_ACTIONS.map((action) => (
              <button
                key={action.id}
                type="button"
                role="menuitem"
                onClick={() => selectQuickAction(action.id)}
                className="w-full h-8 px-2 rounded-md flex items-center gap-2 text-[13px] text-fg-2 hover:bg-surface-hi hover:text-fg transition text-left"
              >
                <span className="w-5 shrink-0 flex items-center justify-center">
                  <VsIcon name={action.icon} size={action.iconSize} />
                </span>
                <span>{action.label}</span>
              </button>
            ))}
          </div>
        )}
      </div>

      <div className="ml-auto flex items-center gap-1">
        {consoleAvailable && (
          <QuickBtn
            title={consoleOpen ? '关闭控制台 (Ctrl+`)' : '打开控制台 (Ctrl+`)'}
            onClick={onToggleConsole}
            pressed={consoleOpen}
          >
            <VsIcon name="terminal" size={15} />
          </QuickBtn>
        )}
        <QuickBtn title={theme === 'dark' ? '切到浅色' : '切到深色'} onClick={toggle}>
          <VsIcon name={theme === 'dark' ? 'brightness' : 'darkTheme'} size={14} />
        </QuickBtn>
        <button
          data-tour-target="topbar-settings"
          type="button"
          onClick={onSettings}
          className="px-2.5 py-1 rounded-md text-[12px] text-fg-mute hover:text-fg hover:bg-surface-hi transition flex items-center gap-1.5"
        >
          <VsIcon name="settings" size={20} />
          <span>设置</span>
        </button>
        {framelessDesktop && <WindowControls isMaximized={isMaximized} />}
      </div>
    </div>
  );
}
