// 全屏设置页:左栏导航 + 右栏内容(Codex 风格)。
//
// 设计来源:Claude Design 高保真原型 (panels.jsx)。NAV 顺序与设计稿一致。
// 后端真实接入的 section:常规 (权限模式) / 外观 (主题) / 模型 (ModelManager)。
// 其余 section (配置 / 个性化 / MCP / 工具 / 已归档会话 / 使用情况) 当前仅 UI 占位
// — 状态走本地 useState,提交按钮无网络副作用,待后端接口就绪后接入。

import { useEffect, useMemo, useState } from 'react';
import { useTheme } from '../theme.jsx';
import { api } from '../lib/api.js';
import { Toggle } from './Modal.jsx';
import { clsx, relativeTime } from '../lib/format.js';
import { normalizePermissionMode } from '../lib/permissionMode.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { VsIcon } from './Icon.jsx';
import { ModelManager } from './ModelManager.jsx';
import { toast } from './Toast.jsx';
import {
  WindowControls,
  isInteractiveTarget,
  useFramelessWindowState,
} from './WindowControls.jsx';

const NAV = ['常规', '外观', '配置', '个性化', 'MCP 服务器', '模型', '工具', '已归档会话', '使用情况'];

export function SettingsPage({
  onClose,
  health,
  activeSessionId = '',
  onPermissionModeChanged,
  showAceCodeAvatar = true,
  onShowAceCodeAvatarChanged,
}) {
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
          {activeNav === 0 && (
            <SectionGeneral
              health={health}
              activeSessionId={activeSessionId}
              onPermissionModeChanged={onPermissionModeChanged}
            />
          )}
          {activeNav === 1 && <SectionAppearance theme={theme} setTheme={setTheme} />}
          {activeNav === 2 && <SectionConfig />}
          {activeNav === 3 && (
            <SectionPersonalization
              showAceCodeAvatar={showAceCodeAvatar}
              onShowAceCodeAvatarChanged={onShowAceCodeAvatarChanged}
            />
          )}
          {activeNav === 4 && <SectionMCP />}
          {activeNav === 5 && (
            <>
              <h2 className="text-xl font-bold mb-1">模型</h2>
              <p className="text-[12px] text-fg-mute mb-5">
                管理已保存的模型预设;★ 表示当前默认。聊天界面顶栏切换的就是这里的列表。
              </p>
              <ModelManager />
            </>
          )}
          {activeNav === 6 && <SectionTools />}
          {activeNav === 7 && <SectionArchived />}
          {activeNav === 8 && <SectionUsage />}
        </div>
      </div>
    </div>
  );
}

// ─── 常规 ──────────────────────────────────────────────────────────────────
// 真实接入:权限模式(api.getSessionPermissionMode / setSessionPermissionMode)、
// Daemon 状态(/api/health 透传 health prop)。其余字段(工作模式 / 默认打开目标 /
// 最大轮次)目前是 UI 占位,本地 state。

function SectionGeneral({ health, activeSessionId = '', onPermissionModeChanged }) {
  const [permMode, setPermMode] = useState('default');
  const [permBusy, setPermBusy] = useState(false);
  const [maxTurns, setMaxTurns] = useState(50);
  const [workMode, setWorkMode] = useState('coding');
  const [openTarget, setOpenTarget] = useState('vscode');

  useEffect(() => {
    if (!activeSessionId) {
      setPermMode('default');
      setPermBusy(false);
      return undefined;
    }
    let cancelled = false;
    setPermBusy(false);
    api.getSessionPermissionMode(activeSessionId)
      .then((state) => {
        if (!cancelled) setPermMode(normalizePermissionMode(state?.mode));
      })
      .catch(() => {
        if (!cancelled) setPermMode('default');
      });
    return () => { cancelled = true; };
  }, [activeSessionId]);

  const switchPermissionMode = async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permMode);
    if (!activeSessionId || permBusy || nextMode === previousMode) return;
    setPermMode(nextMode);
    setPermBusy(true);
    try {
      const state = await api.setSessionPermissionMode(activeSessionId, nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermMode(confirmedMode);
      onPermissionModeChanged?.({ sessionId: activeSessionId, mode: confirmedMode });
      toast({ kind: 'ok', text: '权限模式已更新' });
    } catch (e) {
      setPermMode(previousMode);
      toast({ kind: 'err', text: '权限模式更新失败:' + (e?.message || '') });
    } finally {
      setPermBusy(false);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">常规</h2>

      <div className="text-[14px] font-semibold mb-1">工作模式</div>
      <p className="text-[12px] text-fg-mute mb-3">选择 Agent 显示多少技术细节</p>
      <div className="grid grid-cols-2 gap-3 max-w-md mb-5">
        {[
          { key: 'coding', label: '用于编程', desc: '更专业的回复与控制' },
          { key: 'daily',  label: '适合日常工作', desc: '同样强大,技术细节更少' },
        ].map((opt) => {
          const active = workMode === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              onClick={() => setWorkMode(opt.key)}
              className={clsx(
                'relative p-3 rounded-lg border text-left transition',
                active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
              )}
            >
              <div className="text-[13px] font-semibold">{opt.label}</div>
              <div className="text-[11px] text-fg-mute mt-1">{opt.desc}</div>
              {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </button>
          );
        })}
      </div>

      <div className="h-px bg-border my-5" />

      <div className="text-[14px] font-semibold mb-1">权限模式</div>
      <p className="text-[12px] text-fg-mute mb-3">
        {activeSessionId ? '控制当前会话调用工具时的确认行为' : '请先打开一个会话后再切换权限模式'}
      </p>
      {[
        { id: 'default',      name: '默认',         desc: '可读取并编辑工作区中的文件;写/执行操作前会请求确认' },
        { id: 'accept-edits', name: '自动接受编辑',   desc: '文件编辑自动通过,bash/网络命令仍需确认' },
        { id: 'yolo',         name: '完全访问 (Yolo)', desc: '所有工具调用跳过确认,适合受信任的工作流' },
      ].map((p, i) => (
        <div
          key={i}
          role="radio"
          aria-checked={permMode === p.id}
          tabIndex={activeSessionId ? 0 : -1}
          onClick={() => switchPermissionMode(p.id)}
          onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); switchPermissionMode(p.id); } }}
          className={clsx(
            'flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
            activeSessionId ? 'cursor-pointer hover:bg-surface-hi' : 'opacity-60 cursor-not-allowed',
          )}
        >
          <div>
            <div className="text-[13px] font-medium">{p.name}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{p.desc}</div>
          </div>
          <Toggle on={permMode === p.id} onChange={(v) => { if (v) switchPermissionMode(p.id); }} />
        </div>
      ))}

      <div className="h-px bg-border my-5" />

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">默认打开目标</div>
          <div className="text-[11px] text-fg-mute mt-0.5">默认打开文件和文件夹的位置</div>
        </div>
        <select
          value={openTarget}
          onChange={(e) => setOpenTarget(e.target.value)}
          className="h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
        >
          <option value="vscode">VS Code</option>
          <option value="vim">Vim</option>
          <option value="terminal">终端</option>
        </select>
      </div>
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

// ─── 外观 ──────────────────────────────────────────────────────────────────

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

// ─── 配置 ──────────────────────────────────────────────────────────────────
// UI 占位:程序版本 / 工作空间依赖项 / 诊断 / 重置。设计 panels.jsx::renderConfigContent。

function SectionConfig() {
  const [depPython, setDepPython] = useState(true);
  const [depNode, setDepNode] = useState(true);
  const [depCsharp, setDepCsharp] = useState(false);
  const [diagRunning, setDiagRunning] = useState(false);
  const [resetRunning, setResetRunning] = useState(false);

  const runDiag = () => {
    setDiagRunning(true);
    setTimeout(() => {
      setDiagRunning(false);
      toast({ kind: 'ok', text: '诊断完成(占位)' });
    }, 1600);
  };
  const runReset = () => {
    setResetRunning(true);
    setTimeout(() => {
      setResetRunning(false);
      toast({ kind: 'ok', text: '重置完成(占位)' });
    }, 2400);
  };

  const dependencies = [
    { key: 'python', label: 'Python 工具', desc: 'uv / ruff / mypy 等',     checked: depPython, toggle: () => setDepPython((v) => !v) },
    { key: 'node',   label: 'Node.js 工具', desc: 'pnpm / npm / tsx 等',     checked: depNode,   toggle: () => setDepNode((v) => !v) },
    { key: 'csharp', label: 'C# 工具',     desc: 'dotnet SDK / Roslyn 等',  checked: depCsharp, toggle: () => setDepCsharp((v) => !v) },
  ];

  return (
    <>
      <h2 className="text-xl font-bold mb-5">配置</h2>

      <div className="text-[14px] font-semibold mb-1">工作空间依赖项</div>
      <p className="text-[12px] text-fg-mute mb-3">管理 ACECode 安装并提供给 Agent 使用的开发工具</p>

      {/* 程序版本 */}
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">当前版本</div>
          <div className="text-[11px] text-fg-mute mt-0.5">ACECode 桌面 / TUI / Daemon 同版本号</div>
        </div>
        <span className="font-mono text-[12px] text-fg-2">dev (placeholder)</span>
      </div>

      {/* 依赖项 checkbox 组 */}
      <div className="rounded-md bg-surface border border-border px-3.5 py-3 mb-2">
        <div className="text-[13px] font-medium mb-0.5">ACECode 依赖项</div>
        <div className="text-[11px] text-fg-mute mb-2.5">选择捆绑安装的语言工具链</div>
        <div className="space-y-0.5">
          {dependencies.map((dep) => (
            <button
              key={dep.key}
              type="button"
              onClick={dep.toggle}
              aria-checked={dep.checked}
              role="checkbox"
              className="w-full flex items-center gap-2.5 px-2 py-1.5 rounded text-left hover:bg-surface-hi transition"
            >
              <span
                className={clsx(
                  'w-[18px] h-[18px] rounded flex items-center justify-center text-white text-[11px] font-bold leading-none transition shrink-0',
                  dep.checked ? 'bg-accent border-2 border-accent' : 'border-2 border-border bg-transparent',
                )}
              >
                {dep.checked && <span>✓</span>}
              </span>
              <span className="flex-1 min-w-0">
                <span className="text-[13px] font-medium block">{dep.label}</span>
                <span className="text-[11px] text-fg-mute block">{dep.desc}</span>
              </span>
            </button>
          ))}
        </div>
      </div>

      <div className="h-px bg-border my-5" />

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0 pr-3">
          <div className="text-[13px] font-medium">诊断 ACECode 工作空间</div>
          <div className="text-[11px] text-fg-mute mt-0.5">检查当前捆绑包并记录诊断日志</div>
        </div>
        <button
          type="button"
          onClick={runDiag}
          disabled={diagRunning}
          className="shrink-0 inline-flex items-center gap-1.5 px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition disabled:opacity-60"
        >
          {diagRunning ? (
            <>
              <span className="ace-spinner" style={{ width: 12, height: 12 }} />
              诊断中…
            </>
          ) : '诊断'}
        </button>
      </div>

      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0 pr-3">
          <div className="text-[13px] font-medium">重置并重装工作空间</div>
          <div className="text-[11px] text-fg-mute mt-0.5">删除本地捆绑包,重新下载后再加载工具</div>
        </div>
        <button
          type="button"
          onClick={runReset}
          disabled={resetRunning}
          className="shrink-0 inline-flex items-center gap-1.5 px-3 py-1 rounded-md text-[12px] text-danger bg-danger-bg hover:opacity-80 transition disabled:opacity-60"
        >
          {resetRunning ? (
            <>
              <span
                className="inline-block w-3 h-3 rounded-full border-2 border-danger border-t-transparent"
                style={{ animation: 'ace-spin 0.8s linear infinite' }}
              />
              重置中…
            </>
          ) : '重新安装'}
        </button>
      </div>
    </>
  );
}

// ─── 个性化 ────────────────────────────────────────────────────────────────
// UI 占位:自定义指令文本框 + 保存按钮。设计 panels.jsx::renderPersonalizationContent。

function SectionPersonalization({ showAceCodeAvatar = true, onShowAceCodeAvatarChanged }) {
  const [text, setText] = useState('');
  const [saved, setSaved] = useState(false);

  const save = () => {
    setSaved(true);
    toast({ kind: 'ok', text: '已保存(占位)' });
    setTimeout(() => setSaved(false), 1500);
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">个性化</h2>

      <div
        role="checkbox"
        aria-checked={showAceCodeAvatar}
        tabIndex={0}
        onClick={() => onShowAceCodeAvatarChanged?.(!showAceCodeAvatar)}
        onKeyDown={(e) => {
          if (e.key === ' ' || e.key === 'Enter') {
            e.preventDefault();
            onShowAceCodeAvatarChanged?.(!showAceCodeAvatar);
          }
        }}
        className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-5 cursor-pointer hover:bg-surface-hi transition"
      >
        <div className="min-w-0 pr-3">
          <div className="text-[13px] font-medium">ACECode 头像显示</div>
          <div className="text-[11px] text-fg-mute mt-0.5">控制聊天窗口中 ACECode 头像和名称是否显示</div>
        </div>
        <Toggle on={showAceCodeAvatar} onChange={onShowAceCodeAvatarChanged} />
      </div>

      <div className="text-[14px] font-semibold mb-1">自定义指令</div>
      <p className="text-[12px] text-fg-mute mb-3">为你的项目向 ACECode 提供额外说明和上下文</p>

      <textarea
        value={text}
        onChange={(e) => { setText(e.target.value); setSaved(false); }}
        placeholder="例如:这个项目使用 React 18 + Vite,组件库选 Tailwind 风格,提交信息用中文..."
        rows={10}
        className="w-full px-3 py-2.5 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition leading-relaxed resize-y"
        style={{ minHeight: 240 }}
      />

      <div className="flex justify-end mt-3">
        <button
          type="button"
          onClick={save}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
          )}
        >
          {saved ? '✓ 已保存' : '保存'}
        </button>
      </div>
    </>
  );
}

// ─── MCP 服务器 ────────────────────────────────────────────────────────────
// UI 占位:JSON 文本编辑器 + 格式化/保存。设计 panels.jsx::renderMCPContent。
// 当前不接 PUT /api/config 之类接口,仅本地编辑预览。

const MCP_INITIAL = JSON.stringify({
  mcp_servers: {
    filesystem: {
      command: 'npx',
      args: ['-y', '@modelcontextprotocol/server-filesystem', '/path'],
    },
    'remote-sse': {
      transport: 'sse',
      url: 'https://mcp.example.com',
      sse_endpoint: '/sse',
    },
    'remote-http': {
      transport: 'http',
      url: 'https://mcp.example.com',
      sse_endpoint: '/mcp',
    },
  },
}, null, 2);

function SectionMCP() {
  const [text, setText] = useState(MCP_INITIAL);
  const [error, setError] = useState('');
  const [saved, setSaved] = useState(false);

  const format = () => {
    try {
      const parsed = JSON.parse(text);
      setText(JSON.stringify(parsed, null, 2));
      setError('');
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };
  const save = () => {
    try {
      JSON.parse(text);
      setError('');
      setSaved(true);
      toast({ kind: 'ok', text: 'MCP 配置已保存(占位)' });
      setTimeout(() => setSaved(false), 1500);
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">MCP 服务器</h2>

      <div className="text-[14px] font-semibold mb-1">服务器配置</div>
      <p className="text-[12px] text-fg-mute mb-3">
        直接编辑 JSON 配置 MCP 服务器连接(stdio / sse / http)
      </p>

      <textarea
        value={text}
        onChange={(e) => { setText(e.target.value); setError(''); setSaved(false); }}
        spellCheck={false}
        rows={18}
        className={clsx(
          'w-full px-4 py-3 text-[12px] rounded-md border bg-code-bg text-code-fg font-mono outline-none transition leading-relaxed resize-y',
          error ? 'border-danger' : 'border-border focus:border-accent',
        )}
        style={{ minHeight: 380, tabSize: 2 }}
      />
      {error && (
        <div className="mt-2 text-[12px] text-danger">{error}</div>
      )}

      <div className="flex justify-end gap-2 mt-3">
        <button
          type="button"
          onClick={format}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi transition"
        >
          格式化
        </button>
        <button
          type="button"
          onClick={save}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
          )}
        >
          {saved ? '✓ 已保存' : '保存'}
        </button>
      </div>
    </>
  );
}

// ─── 工具 ──────────────────────────────────────────────────────────────────
// UI 占位:Browser Use 等内置工具开关。设计 panels.jsx::renderToolsContent。

function SectionTools() {
  const [browserUse, setBrowserUse] = useState(true);

  const tools = [
    {
      key: 'browser_use',
      name: 'Browser Use',
      desc: '让 ACECode 控制内置浏览器进行页面操作 / 截图 / 表单填写',
      icon: (
        <svg
          width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor"
          strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"
          aria-hidden="true"
        >
          <circle cx="12" cy="12" r="9" />
          <path d="M3 12h18" />
          <path d="M12 3a13 13 0 0 1 0 18M12 3a13 13 0 0 0 0 18" />
        </svg>
      ),
      on: browserUse,
      toggle: () => setBrowserUse((v) => !v),
    },
  ];

  return (
    <>
      <h2 className="text-xl font-bold mb-5">工具</h2>

      <div className="text-[14px] font-semibold mb-1">内置工具</div>
      <p className="text-[12px] text-fg-mute mb-3">启用后 Agent 可在任务中自动调用</p>

      {tools.map((tool) => (
        <div
          key={tool.key}
          className="flex items-center gap-3 px-3.5 py-3 rounded-md bg-surface border border-border mb-2"
        >
          <div className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
            {tool.icon}
          </div>
          <div className="flex-1 min-w-0">
            <div className="text-[13px] font-medium">{tool.name}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{tool.desc}</div>
          </div>
          <Toggle on={tool.on} onChange={tool.toggle} />
        </div>
      ))}

      {/* 占位:更多工具即将加入 */}
      <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mt-2">
        更多内置工具即将加入
      </div>
    </>
  );
}

// ─── 已归档会话 ────────────────────────────────────────────────────────────
function SectionArchived() {
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.listAllArchivedSessions()
      .then(async (result) => {
        let sessions = Array.isArray(result?.sessions) ? result.sessions : [];
        if (sessions.length === 0 && Array.isArray(result?.errors) && result.errors.length === 0) {
          const workspaces = await api.listWorkspaces().catch(() => []);
          if (!Array.isArray(workspaces) || workspaces.length === 0) {
            const local = await api.listSessions({ archived: true }).catch(() => []);
            sessions = (Array.isArray(local) ? local : []).map((item) => ({
              ...item,
              workspace_hash: item.workspace_hash || '__local__',
              workspaceName: item.workspaceName || '当前会话',
            }));
          }
        }
        if (!cancelled) setList(sessions);
      })
      .catch((e) => {
        if (!cancelled) setError(e.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const unarchive = async (item) => {
    const id = item?.id || item?.session_id || item?.sessionId || '';
    const workspaceHash = item?.workspace_hash || item?.workspaceHash || '';
    if (!id) return;
    try {
      if (workspaceHash && workspaceHash !== '__local__') {
        await api.unarchiveWorkspaceSession(workspaceHash, id);
      } else {
        await api.unarchiveSession(id);
      }
      setList((prev) => prev.filter((x) => (x.id || x.session_id || x.sessionId) !== id));
      window.dispatchEvent(new Event('ace-session-archive-changed'));
      toast({ kind: 'ok', text: '已取消归档' });
    } catch (e) {
      toast({ kind: 'err', text: '取消归档失败:' + (e.message || '') });
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">已归档会话</h2>

      <div className="text-[14px] font-semibold mb-1">归档列表</div>
      <p className="text-[12px] text-fg-mute mb-3">已归档的会话不会出现在侧栏,可随时取消归档恢复</p>

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : error ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-danger text-center">
          加载失败:{error}
        </div>
      ) : list.length === 0 ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          暂无已归档会话
        </div>
      ) : (
        list.map((item) => (
          <div
            key={item.id}
            className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2"
          >
            <div className="min-w-0 pr-3">
              <div className="text-[13px] font-medium truncate">{sessionDisplayTitle(item, item.name || '')}</div>
              <div className="text-[11px] text-fg-mute mt-0.5 truncate">
                {relativeTime(item.updated_at || item.created_at)} · {item.workspaceName || item.cwd || item.workspace_hash || 'workspace'}
              </div>
            </div>
            <button
              type="button"
              onClick={() => unarchive(item)}
              className="shrink-0 px-3 py-1 rounded-md text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border transition"
            >
              取消归档
            </button>
          </div>
        ))
      )}
    </>
  );
}

// ─── 使用情况 ──────────────────────────────────────────────────────────────
// UI 占位:总览卡片 + 每日柱状图 + 模型用量明细。设计 panels.jsx::renderUsageContent。
// mock 数据,后端 usage 接口接通后切真实数值。

const USAGE_PLACEHOLDER = {
  period: '2026年5月',
  totalTokens: 2847563,
  totalCost: 42.86,
  models: [
    { name: 'gpt-4o',             inputTokens: 1245800, outputTokens: 623400, calls: 342, color: 'var(--ace-ok)' },
    { name: 'claude-3.5-sonnet',  inputTokens: 412300,  outputTokens: 198700, calls: 87,  color: 'var(--ace-accent)' },
    { name: 'gpt-4o-mini',        inputTokens: 198400,  outputTokens: 89200,  calls: 156, color: 'var(--ace-warn)' },
    { name: 'deepseek-r1',        inputTokens: 52600,   outputTokens: 27163,  calls: 23,  color: 'var(--ace-danger)' },
  ],
  daily: [
    { day: '5/1', tokens: 82000 },
    { day: '5/2', tokens: 134000 },
    { day: '5/3', tokens: 97000 },
    { day: '5/4', tokens: 210000 },
    { day: '5/5', tokens: 185000 },
    { day: '5/6', tokens: 263000 },
    { day: '5/7', tokens: 198000 },
    { day: '5/8', tokens: 312000 },
    { day: '5/9', tokens: 276000 },
    { day: '5/10', tokens: 145000 },
  ],
};

function formatTokens(n) {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(2) + 'M';
  if (n >= 1_000)     return (n / 1_000).toFixed(1) + 'K';
  return String(n);
}

function SectionUsage() {
  const data = USAGE_PLACEHOLDER;
  const maxDaily       = useMemo(() => Math.max(...data.daily.map((d) => d.tokens)), [data.daily]);
  const maxModelTotal  = useMemo(() => Math.max(...data.models.map((m) => m.inputTokens + m.outputTokens)), [data.models]);
  const totalCalls     = useMemo(() => data.models.reduce((a, m) => a + m.calls, 0), [data.models]);

  const summary = [
    { label: '本月总 Tokens', value: formatTokens(data.totalTokens), sub: data.period },
    { label: '预估费用',      value: '$' + data.totalCost.toFixed(2), sub: 'USD' },
    { label: 'API 调用次数',  value: String(totalCalls),              sub: '次请求' },
  ];

  return (
    <>
      <h2 className="text-xl font-bold mb-5">使用情况</h2>

      {/* 总览卡片 */}
      <div className="grid grid-cols-3 gap-3 mb-6">
        {summary.map((c) => (
          <div key={c.label} className="px-4 py-3.5 rounded-md bg-surface border border-border">
            <div className="text-[10px] text-fg-mute uppercase tracking-wider mb-1.5">{c.label}</div>
            <div className="text-[24px] font-bold text-fg leading-none mb-1">{c.value}</div>
            <div className="text-[11px] text-fg-mute">{c.sub}</div>
          </div>
        ))}
      </div>

      {/* 每日趋势柱状图 */}
      <div className="text-[14px] font-semibold mb-1">每日用量趋势</div>
      <p className="text-[12px] text-fg-mute mb-3">近 10 天 token 消耗</p>
      <div className="px-4 pt-4 pb-2 rounded-md bg-surface border border-border mb-6">
        <div className="flex items-end gap-1.5 h-[120px]">
          {data.daily.map((d) => {
            const h = (d.tokens / maxDaily) * 100;
            return (
              <div key={d.day} className="flex-1 flex flex-col items-center gap-1.5">
                <div className="text-[9px] font-mono text-fg-mute opacity-70 whitespace-nowrap">
                  {formatTokens(d.tokens)}
                </div>
                <div
                  className="w-full rounded-sm bg-accent transition-all"
                  style={{ height: `${h}%`, minHeight: 4, opacity: 0.85 }}
                />
                <div className="text-[10px] text-fg-mute">{d.day}</div>
              </div>
            );
          })}
        </div>
      </div>

      {/* 模型用量明细 */}
      <div className="text-[14px] font-semibold mb-1">模型用量明细</div>
      <p className="text-[12px] text-fg-mute mb-3">每个模型的输入 / 输出 token 与调用次数</p>
      <div className="rounded-md bg-surface border border-border overflow-hidden">
        {data.models.map((m, i) => {
          const total = m.inputTokens + m.outputTokens;
          const barWidth = (total / maxModelTotal) * 100;
          const inputPct = (m.inputTokens / total) * 100;
          return (
            <div
              key={m.name}
              className={clsx(
                'px-4 py-3.5',
                i < data.models.length - 1 && 'border-b border-border',
              )}
            >
              <div className="flex items-center justify-between mb-2">
                <div className="flex items-center gap-2 min-w-0">
                  <span className="w-2.5 h-2.5 rounded-sm shrink-0" style={{ background: m.color }} />
                  <span className="text-[13px] font-mono font-semibold truncate">{m.name}</span>
                </div>
                <div className="flex items-center gap-4 shrink-0 ml-2">
                  <span className="text-[12px] text-fg-mute">{m.calls} 次</span>
                  <span className="text-[13px] font-mono font-semibold">{formatTokens(total)}</span>
                </div>
              </div>
              {/* 输入/输出比例条 */}
              <div className="h-1.5 rounded-sm bg-surface-hi overflow-hidden mb-1.5">
                <div className="h-full flex" style={{ width: `${barWidth}%` }}>
                  <div className="h-full" style={{ width: `${inputPct}%`, background: m.color }} />
                  <div className="h-full flex-1" style={{ background: m.color, opacity: 0.4 }} />
                </div>
              </div>
              <div className="flex gap-4 text-[11px] text-fg-mute">
                <span>输入 {formatTokens(m.inputTokens)}</span>
                <span>输出 {formatTokens(m.outputTokens)}</span>
              </div>
            </div>
          );
        })}
      </div>

      <div className="mt-4 px-3.5 py-2.5 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center">
        数据为示例占位,接入 usage 后端接口后将展示真实数值
      </div>
    </>
  );
}
