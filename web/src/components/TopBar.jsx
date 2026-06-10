// TopBar:logo + 快捷工具(项目栏收缩/新对话/搜索/···) + 主题切换 + 设置
//
// 占位按钮(搜索/···)留 hover 反馈但 onClick 暂为 toast 提醒"待开发",避免点了"无反应"。

import { useTheme } from '../theme.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
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
}) {
  const isToggle = typeof pressed === 'boolean';

  return (
    <button
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
  onStartUpdate,
}) {
  const { theme, toggle } = useTheme();
  const { framelessDesktop, isMaximized } = useFramelessWindowState();
  const isMac = typeof navigator !== 'undefined' && /Mac|iPhone|iPad|iPod/.test(navigator.platform || '');
  const searchHotkeyHint = isMac ? '搜索 (Cmd+K)' : '搜索 (Ctrl+K)';
  const updateAvailable = !!updateStatus?.update_available;
  const updateTitle = updateAvailable
    ? `发现新版 v${updateStatus.latest_version || ''}, 点击升级`
    : '';

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
        <QuickBtn
          title={updateTitle}
          onClick={onStartUpdate}
          disabled={updateStarting}
          className="rounded-full bg-accent text-white hover:bg-accent hover:text-white hover:opacity-90"
        >
          <VsIcon
            name={updateStarting ? 'running' : 'glyphDown'}
            size={14}
            mono={false}
            className="ace-icon-on-accent"
          />
        </QuickBtn>
      )}
      <QuickBtn title="新对话" onClick={onNewSession}>
        <VsIcon name="editWindow" size={16} />
      </QuickBtn>
      <QuickBtn title={searchHotkeyHint} onClick={onOpenSearch}>
        <VsIcon name="search" size={14} />
      </QuickBtn>
      <QuickBtn title="更多" onClick={() => toast({ kind: 'info', text: '更多工具待补充' })}>
        <VsIcon name="ellipsis" size={14} />
      </QuickBtn>

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
