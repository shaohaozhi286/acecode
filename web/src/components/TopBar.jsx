// TopBar:logo + 快捷工具(新对话/搜索/···) + 中央 single/grid4/grid9 pill + 主题切换 + 设置
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

export function TopBar({ view, onViewChange, onSettings, onNewSession }) {
  const { theme, toggle } = useTheme();

  return (
    <div className="h-11 px-3 flex items-center gap-2 bg-surface border-b border-border relative z-10 shrink-0">
      <div className="flex items-center gap-1.5 select-none mr-1">
        <img src="/acecode-logo.png" alt="" width="20" height="20" className="block" draggable="false" />
        <span className="text-[15px] font-bold tracking-tight">ACECode</span>
      </div>

      <QuickBtn title="新对话" onClick={onNewSession}>
        <VsIcon name="editWindow" size={16} />
      </QuickBtn>
      <QuickBtn title="搜索" onClick={() => toast({ kind: 'info', text: '搜索功能开发中' })}>
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
      </div>
    </div>
  );
}
