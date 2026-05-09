// 全屏设置页:左栏导航 + 右栏内容(Codex 风格)。
// v1 设置仅本地展示,daemon 端配置实际由 ~/.acecode/config.json 管理 — 此处主要
// 展示当前 daemon 状态 + 工作模式选择(本地)。后续可补 PUT /api/config 等。

import { useEffect, useState } from 'react';
import { useTheme } from '../theme.jsx';
import { Toggle } from './Modal.jsx';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import { ModelManager } from './ModelManager.jsx';
import {
  WindowControls,
  isInteractiveTarget,
  useFramelessWindowState,
} from './WindowControls.jsx';

const NAV = ['常规', '外观', '配置', '个性化', 'MCP 服务器', '模型', '环境', '项目指令', '已归档对话', '使用情况'];

export function SettingsPage({ onClose, health }) {
  const { theme, set: setTheme } = useTheme();
  const [activeNav, setActiveNav] = useState(0);
  const [show, setShow] = useState(false);
  const { framelessDesktop, isMaximized } = useFramelessWindowState();

  useEffect(() => { requestAnimationFrame(() => setShow(true)); }, []);
  const close = () => { setShow(false); setTimeout(onClose, 240); };

  const onHeaderMouseDown = (event) => {
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
        'fixed inset-0 z-[300] bg-bg flex flex-col transition-all duration-250',
        show ? 'opacity-100 translate-y-0' : 'opacity-0 translate-y-4',
      )}
    >
      <div
        className={clsx(
          'h-11 pl-4 pr-0 flex items-center bg-surface border-b border-border shrink-0',
          framelessDesktop && 'ace-desktop-frameless-topbar',
        )}
        onMouseDown={onHeaderMouseDown}
      >
        <button
          type="button"
          onClick={close}
          className="px-3 h-7 rounded-md text-fg-2 text-[13px] hover:bg-surface-hi transition flex items-center gap-1.5"
        ><VsIcon name="back" size={13} />返回</button>
        <span className="flex-1 text-center text-[15px] font-semibold">设置</span>
        {/* 占位:让标题居中,与 TopBar 视觉对齐;frameless 模式下右侧由 WindowControls 占据 */}
        <div className={clsx(framelessDesktop ? 'flex items-center pr-0' : 'w-16 pr-4')}>
          {framelessDesktop && <WindowControls isMaximized={isMaximized} />}
        </div>
      </div>
      <div className="flex-1 flex overflow-hidden">
        <nav className="w-[200px] bg-surface-alt border-r border-border py-2 overflow-y-auto shrink-0">
          {NAV.map((label, i) => (
            <button
              key={i}
              type="button"
              onClick={() => setActiveNav(i)}
              className={clsx(
                'w-full text-left px-4 py-2 text-[13px] transition border-l-[3px]',
                activeNav === i
                  ? 'text-accent font-semibold bg-accent-bg border-accent'
                  : 'text-fg hover:bg-surface-hi border-transparent',
              )}
            >
              {label}
            </button>
          ))}
        </nav>
        <div className="flex-1 overflow-y-auto px-12 py-6">
          {activeNav === 0 && <SectionGeneral health={health} />}
          {activeNav === 1 && <SectionAppearance theme={theme} setTheme={setTheme} />}
          {activeNav === 5 && (
            <>
              <h2 className="text-xl font-bold mb-1">模型</h2>
              <p className="text-[12px] text-fg-mute mb-5">
                管理已保存的模型预设;★ 表示当前默认。聊天界面顶栏切换的就是这里的列表。
              </p>
              <ModelManager />
            </>
          )}
          {activeNav !== 0 && activeNav !== 1 && activeNav !== 5 && (
            <div className="text-fg-mute text-sm">{NAV[activeNav]} — 待补充</div>
          )}
        </div>
      </div>
    </div>
  );
}

function SectionGeneral({ health }) {
  // 权限模式三选一,任何时刻只有一档生效。Toggle 仅作视觉指示;onChange 忽略
  // 关闭事件(关掉当前档没有语义),点击非选中行的 toggle / 整行任意位置都把
  // 选中切到该行。
  const [permIndex, setPermIndex] = useState(0);
  const [maxTurns, setMaxTurns] = useState(50);
  return (
    <>
      <h2 className="text-xl font-bold mb-5">常规</h2>

      <div className="text-[14px] font-semibold mb-1">权限模式</div>
      <p className="text-[12px] text-fg-mute mb-3">控制 Agent 调用工具时的确认行为</p>
      {[
        { name: '默认',         desc: '可读取并编辑工作区中的文件;写/执行操作前会请求确认' },
        { name: '自动接受编辑',   desc: '文件编辑自动通过,bash/网络命令仍需确认' },
        { name: '完全访问 (Yolo)', desc: '所有工具调用跳过确认,适合受信任的工作流' },
      ].map((p, i) => (
        <div
          key={i}
          role="radio"
          aria-checked={permIndex === i}
          tabIndex={0}
          onClick={() => setPermIndex(i)}
          onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); setPermIndex(i); } }}
          className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 cursor-pointer hover:bg-surface-hi transition"
        >
          <div>
            <div className="text-[13px] font-medium">{p.name}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{p.desc}</div>
          </div>
          <Toggle on={permIndex === i} onChange={(v) => { if (v) setPermIndex(i); }} />
        </div>
      ))}

      <div className="h-px bg-border my-5" />

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">最大轮次</div>
          <div className="text-[11px] text-fg-mute mt-0.5">单次 agent loop 的最大迭代数</div>
        </div>
        <input
          type="number"
          value={maxTurns}
          onChange={(e) => setMaxTurns(Number(e.target.value) || 50)}
          className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
        />
      </div>
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">Daemon 状态</div>
          <div className="text-[11px] text-fg-mute mt-0.5">{health?.cwd || '—'}</div>
        </div>
        <span className="flex items-center gap-1.5 text-[12px] text-ok">
          <span className="w-2 h-2 rounded-full bg-ok shadow-[0_0_4px_var(--ace-ok)]" />
          运行中 · 端口 {health?.port || 28080}
        </span>
      </div>
    </>
  );
}

function SectionAppearance({ theme, setTheme }) {
  return (
    <>
      <h2 className="text-xl font-bold mb-5">外观</h2>

      <div className="text-[14px] font-semibold mb-1">主题</div>
      <p className="text-[12px] text-fg-mute mb-3">浅色 / 深色;新窗口打开时跟随系统设置</p>
      <div className="grid grid-cols-2 gap-3 max-w-md">
        {[
          { key: 'light', label: '浅色',
            sw1: '#ffffff', sw2: '#f5f5f2', sw3: '#2563eb' },
          { key: 'dark', label: '深色',
            sw1: '#1a1a1a', sw2: '#0f0f0f', sw3: '#3b82f6' },
        ].map((opt) => {
          const active = theme === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              onClick={() => setTheme(opt.key)}
              className={clsx(
                'relative p-3 rounded-lg border text-left transition',
                active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
              )}
            >
              <div className="flex gap-1 mb-2">
                <span className="w-6 h-6 rounded border border-border" style={{ background: opt.sw1 }} />
                <span className="w-6 h-6 rounded border border-border" style={{ background: opt.sw2 }} />
                <span className="w-6 h-6 rounded border border-border" style={{ background: opt.sw3 }} />
              </div>
              <div className="text-[13px] font-semibold">{opt.label}</div>
              {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </button>
          );
        })}
      </div>
    </>
  );
}
