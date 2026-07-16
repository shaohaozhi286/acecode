// 全屏设置页:左栏导航 + 右栏内容(Codex 风格)。
//
// 设计来源:Claude Design 高保真原型 (panels.jsx)。NAV 顺序与设计稿一致。
// 后端真实接入的 section:常规 (权限模式) / 外观 (主题) / 配置 / 个性化 / 技能 / 模型 / 工具。
// 其余 section (MCP / 已归档会话 / 使用情况) 当前部分为 UI 占位
// — 状态走本地 useState,提交按钮无网络副作用,待后端接口就绪后接入。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTheme } from '../theme.jsx';
import { api } from '../lib/api.js';
import { openExternalUrl } from '../lib/externalUrl.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { Toggle } from './Modal.jsx';
import { clsx, relativeTime } from '../lib/format.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { buildMcpServerList, countEnabledMcp, applyMcpToggle } from '../lib/mcpServers.js';
import { normalizeConnectorList, applyConnectorToggle } from '../lib/connectors.js';
import {
  DEFAULT_MODEL_CAPABILITIES,
  MODEL_CAPABILITY_OPTIONS,
  ANTHROPIC_DEFAULT_BASE_URL,
  OPENAI_DEFAULT_BASE_URL,
  baseUrlForProviderSwitch,
  buildModelDraftsFromSelection,
  canDeleteSavedModel,
  filterSavedModels,
  filterModelIds,
  formatRequestHeadersJson,
  formatContextWindowK,
  normalizeModelCapabilities,
  normalizeModelProbeResult,
  parseContextWindowK,
  parseRequestHeadersJson,
  splitModelIds,
  validateModelDraft,
} from '../lib/modelManager.js';
import { PERMISSION_MODES, normalizePermissionMode } from '../lib/permissionMode.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { formatUsageTokens, normalizeUsageStats, usageDataNote } from '../lib/usageStats.js';
import {
  NO_FEEDBACK_SESSION_KEY,
  buildDesktopFeedbackPayload,
  feedbackSessionKey,
  normalizeDesktopFeedbackSessions,
  selectedFeedbackSessionFromKey,
} from '../lib/desktopFeedback.js';
import {
  hookActionState,
  hookEmptyState,
  hookSettingsErrorMessage,
  hookStatusLabel,
  normalizeHookSnapshot,
} from '../lib/hooksSettings.js';
import {
  formatProgramVersion,
  formatWebCoreDetail,
  formatWebCoreLabel,
  getCurrentWebCoreInfo,
} from '../lib/webCoreInfo.js';
import {
  enabledRatioLabel,
  filterSkills,
  groupSkillsBySource,
  normalizeSkillList,
  normalizeWorkspaceList,
  skillsEnabledSummary,
  workspaceAutoExpand,
} from '../lib/skillsSettings.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import {
  WindowControls,
  isInteractiveTarget,
  nativePointerEvent,
  useFramelessWindowState,
} from './WindowControls.jsx';

const NAV = [
  { key: 'general', label: '常规', icon: 'settings' },
  { key: 'appearance', label: '外观', icon: 'brightness' },
  { key: 'config', label: '配置', icon: 'terminal' },
  { key: 'personalization', label: '个性化', icon: 'eye' },
  { key: 'skills', label: '技能', icon: 'lightbulb' },
  { key: 'mcp', label: 'MCP 服务器', icon: 'mcp' },
  { key: 'connectors', label: '连接器', icon: 'extension' },
  { key: 'models', label: '模型', icon: 'brain' },
  { key: 'tools', label: '工具', icon: 'tool' },
  { key: 'hooks', label: '钩子', icon: 'hook' },
  { key: 'archived', label: '已归档会话', icon: 'archive' },
  { key: 'usage', label: '使用情况', icon: 'list' },
  { key: 'feedback', label: '问题反馈', icon: 'help' },
  { key: 'about', label: '关于', icon: 'info' },
];

const DEFAULT_UPGRADE_SERVICE_URL = 'http://2017studio.imwork.net:82/aupdate/';
const FONT_SIZE_OPTIONS = [
  { key: 'small', label: '小' },
  { key: 'medium', label: '中' },
  { key: 'large', label: '大' },
];

function navIndexForKey(key) {
  const idx = NAV.findIndex((item) => item.key === key);
  return idx >= 0 ? idx : 0;
}

export function SettingsPage({
  onClose,
  health,
  activeSessionId = '',
  onPermissionModeChanged,
  onReplayGuidedTour,
  initialNavKey = 'general',
  fontSize = 'medium',
  onFontSizeChange = () => {},
}) {
  const { theme, set: setTheme } = useTheme();
  const [activeNav, setActiveNav] = useState(() => navIndexForKey(initialNavKey));
  const [show, setShow] = useState(false);
  const { framelessDesktop, isMaximized } = useFramelessWindowState();
  const activeNavKey = NAV[activeNav]?.key || 'general';

  useEffect(() => { requestAnimationFrame(() => setShow(true)); }, []);
  useEffect(() => {
    setActiveNav(navIndexForKey(initialNavKey));
  }, [initialNavKey]);
  const close = () => { setShow(false); setTimeout(onClose, 240); };

  const onHeaderMouseDown = (event) => {
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
          {NAV.map((item, i) => (
            <button
              key={item.key}
              type="button"
              onClick={() => setActiveNav(i)}
              className={clsx(
                'w-full px-4 py-2 text-[13px] transition border-l-[3px] flex items-center gap-2 text-left',
                activeNav === i
                  ? 'text-accent font-semibold bg-accent-bg border-accent'
                  : 'text-fg hover:bg-surface-hi border-transparent',
              )}
            >
              <VsIcon name={item.icon} size={15} className="shrink-0 opacity-80" />
              <span className="truncate">{item.label}</span>
            </button>
          ))}
        </nav>
        <div className="flex-1 overflow-y-auto px-12 py-6">
          {activeNavKey === 'general' && (
            <SectionGeneral
              health={health}
              activeSessionId={activeSessionId}
              onPermissionModeChanged={onPermissionModeChanged}
              onReplayGuidedTour={onReplayGuidedTour}
            />
          )}
          {activeNavKey === 'appearance' && (
            <SectionAppearance
              theme={theme}
              setTheme={setTheme}
              fontSize={fontSize}
              onFontSizeChange={onFontSizeChange}
            />
          )}
          {activeNavKey === 'config' && <SectionConfig />}
          {activeNavKey === 'personalization' && <SectionPersonalization />}
          {activeNavKey === 'skills' && <SectionSkills />}
          {activeNavKey === 'mcp' && <SectionMCP />}
          {activeNavKey === 'connectors' && <SectionConnectors />}
          {activeNavKey === 'models' && <SectionModel />}
          {activeNavKey === 'tools' && <SectionTools />}
          {activeNavKey === 'hooks' && <SectionHooks />}
          {activeNavKey === 'archived' && <SectionArchived />}
          {activeNavKey === 'usage' && <SectionUsage />}
          {activeNavKey === 'feedback' && <SectionFeedback />}
          {activeNavKey === 'about' && <SectionAbout health={health} />}
        </div>
      </div>
    </div>
  );
}

// ─── 常规 ──────────────────────────────────────────────────────────────────
// 真实接入:默认权限模式(api.getDefaultPermissionMode / setDefaultPermissionMode)、
// Daemon 状态(/api/health 透传 health prop)。其余字段(工作模式 / 默认打开目标 /
// 最大轮次)目前是 UI 占位,本地 state。

function SectionGeneral({
  health,
  activeSessionId = '',
  onPermissionModeChanged,
  onReplayGuidedTour,
}) {
  const [permMode, setPermMode] = useState('default');
  const [permBusy, setPermBusy] = useState(false);
  const [maxTurns, setMaxTurns] = useState(50);
  const [workMode, setWorkMode] = useState('coding');
  const [openTarget, setOpenTarget] = useState('vscode');

  useEffect(() => {
    let cancelled = false;
    setPermBusy(false);
    api.getDefaultPermissionMode()
      .then((state) => {
        if (!cancelled) setPermMode(normalizePermissionMode(state?.mode));
      })
      .catch(() => {
        if (!cancelled) setPermMode('default');
      });
    return () => { cancelled = true; };
  }, []);

  const switchPermissionMode = async (mode) => {
    const nextMode = normalizePermissionMode(mode);
    const previousMode = normalizePermissionMode(permMode);
    if (permBusy || nextMode === previousMode) return;
    setPermMode(nextMode);
    setPermBusy(true);
    try {
      const state = await api.setDefaultPermissionMode(nextMode);
      const confirmedMode = normalizePermissionMode(state?.mode || nextMode);
      setPermMode(confirmedMode);
      if (activeSessionId) {
        try {
          await api.setSessionPermissionMode(activeSessionId, confirmedMode);
          onPermissionModeChanged?.({ sessionId: activeSessionId, mode: confirmedMode });
        } catch (syncError) {
          toast({ kind: 'err', text: '默认权限模式已更新,当前会话同步失败:' + (syncError?.message || '') });
          return;
        }
      }
      toast({ kind: 'ok', text: '默认权限模式已更新' });
    } catch (e) {
      setPermMode(previousMode);
      toast({ kind: 'err', text: '默认权限模式更新失败:' + (e?.message || '') });
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

      {onReplayGuidedTour && (
        <>
          <div className="flex items-center justify-between gap-4 px-3.5 py-3 rounded-md bg-surface border border-border mb-2">
            <div>
              <div className="text-[13px] font-medium">新手指引</div>
              <div className="text-[11px] text-fg-mute mt-0.5">从添加项目、开始新对话到模型设置，重新查看 7 步入门说明</div>
            </div>
            <button
              type="button"
              onClick={onReplayGuidedTour}
              className="h-8 shrink-0 px-3 rounded-md bg-accent text-white text-[12px] font-semibold hover:opacity-90 transition"
            >
              重新查看新手指引
            </button>
          </div>
          <div className="h-px bg-border my-5" />
        </>
      )}

      <div className="text-[14px] font-semibold mb-1">权限模式</div>
      <p className="text-[12px] text-fg-mute mb-3">
        {activeSessionId ? '新建会话默认使用此模式,当前会话会同步切换' : '新建会话默认使用此模式'}
      </p>
      {PERMISSION_MODES.map((p, i) => (
        <div
          key={i}
          role="radio"
          aria-checked={permMode === p.id}
          tabIndex={0}
          onClick={() => switchPermissionMode(p.id)}
          onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); switchPermissionMode(p.id); } }}
          className={clsx(
            'flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 transition',
            'cursor-pointer hover:bg-surface-hi',
            permBusy && 'opacity-75 cursor-wait',
          )}
        >
          <div>
            <div className="text-[13px] font-medium">{p.label}</div>
            <div className="text-[11px] text-fg-mute mt-0.5">{p.hint}</div>
          </div>
          <div onClick={(e) => e.stopPropagation()}>
            <Toggle
              on={permMode === p.id}
              disabled={permBusy}
              onChange={(v) => { if (v) switchPermissionMode(p.id); }}
            />
          </div>
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

function SectionAppearance({ theme, setTheme, fontSize, onFontSizeChange }) {
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

      <div className="mt-7 text-[14px] font-semibold mb-1">字体大小</div>
      <div className="grid grid-cols-3 gap-1 p-1 rounded-lg border border-border bg-surface max-w-[240px]">
        {FONT_SIZE_OPTIONS.map((opt) => {
          const active = fontSize === opt.key;
          return (
            <button
              key={opt.key}
              type="button"
              aria-pressed={active}
              onClick={() => onFontSizeChange(opt.key)}
              className={clsx(
                'h-8 rounded-md text-[13px] font-medium transition',
                active
                  ? 'bg-accent text-white shadow-sm'
                  : 'text-fg-2 hover:bg-surface-hi hover:text-fg',
              )}
            >
              {opt.label}
            </button>
          );
        })}
      </div>
    </>
  );
}

// ─── 关于 ──────────────────────────────────────────────────────────────────

function SectionAbout({ health }) {
  const [webCoreInfo, setWebCoreInfo] = useState(null);

  useEffect(() => {
    let cancelled = false;
    getCurrentWebCoreInfo()
      .then((info) => {
        if (!cancelled) setWebCoreInfo(info);
      })
      .catch(() => {
        if (!cancelled) setWebCoreInfo(null);
      });
    return () => { cancelled = true; };
  }, []);

  const programVersionLabel = formatProgramVersion(health?.version);
  const webCoreLabel = formatWebCoreLabel(webCoreInfo);
  const webCoreDetail = formatWebCoreDetail(webCoreInfo);

  return (
    <>
      <h2 className="text-xl font-bold mb-5">关于</h2>

      {/* 程序版本 */}
      <div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-medium">当前版本</div>
          <div className="text-[11px] text-fg-mute mt-0.5">ACECode 桌面 / TUI / Daemon 同版本号</div>
        </div>
        <span className="text-[12px] text-fg-2">{programVersionLabel}</span>
      </div>

      <div className="flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0">
          <div className="text-[13px] font-medium">Web 核心</div>
          <div className="text-[11px] text-fg-mute mt-0.5">当前桌面 WebView / 浏览器渲染核心</div>
        </div>
        <div className="min-w-0 max-w-[62%] text-right">
          <div
            className="text-[12px] text-fg-2 truncate"
            title={webCoreLabel}
          >
            {webCoreLabel}
          </div>
          {webCoreDetail && (
            <div
              className="text-[10px] text-fg-mute truncate"
              title={webCoreDetail}
            >
              {webCoreDetail}
            </div>
          )}
        </div>
      </div>
    </>
  );
}

function SectionConfig() {
  const [upgradeUrl, setUpgradeUrl] = useState(DEFAULT_UPGRADE_SERVICE_URL);
  const [upgradeLoading, setUpgradeLoading] = useState(true);
  const [upgradeSaving, setUpgradeSaving] = useState(false);
  const [upgradeSaved, setUpgradeSaved] = useState(false);
  const [upgradeError, setUpgradeError] = useState('');
  const [depPython, setDepPython] = useState(true);
  const [depNode, setDepNode] = useState(true);
  const [depCsharp, setDepCsharp] = useState(false);
  const [diagRunning, setDiagRunning] = useState(false);
  const [resetRunning, setResetRunning] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setUpgradeLoading(true);
    setUpgradeError('');
    api.getUpgradeConfig()
      .then((cfg) => {
        if (!cancelled) setUpgradeUrl(cfg?.base_url || DEFAULT_UPGRADE_SERVICE_URL);
      })
      .catch((e) => {
        if (!cancelled) setUpgradeError(e?.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setUpgradeLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const saveUpgradeUrl = async () => {
    const baseUrl = upgradeUrl.trim();
    if (!baseUrl || !/^https?:\/\//i.test(baseUrl)) {
      setUpgradeError('升级服务 URL 必须使用 http 或 https');
      return;
    }
    setUpgradeSaving(true);
    setUpgradeSaved(false);
    setUpgradeError('');
    try {
      const saved = await api.setUpgradeConfig({ base_url: baseUrl });
      setUpgradeUrl(saved?.base_url || baseUrl);
      setUpgradeSaved(true);
      toast({ kind: 'ok', text: '升级服务 URL 已保存' });
      setTimeout(() => setUpgradeSaved(false), 1500);
    } catch (e) {
      const message = e?.message || String(e);
      setUpgradeError(message);
      toast({ kind: 'err', text: message });
    } finally {
      setUpgradeSaving(false);
    }
  };

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

      <div className="text-[14px] font-semibold mb-1">升级服务</div>
      <div className="rounded-md bg-surface border border-border px-3.5 py-3 mb-5">
        <label htmlFor="upgrade-service-url" className="text-[13px] font-medium mb-2 block">
          升级服务 URL
        </label>
        <div className="flex gap-2">
          <input
            id="upgrade-service-url"
            type="url"
            value={upgradeUrl}
            onChange={(e) => {
              setUpgradeUrl(e.target.value);
              setUpgradeSaved(false);
              setUpgradeError('');
            }}
            disabled={upgradeLoading || upgradeSaving}
            spellCheck={false}
            className={clsx(
              'flex-1 min-w-0 h-8 px-2.5 rounded-md border bg-bg text-fg text-[12px] outline-none transition',
              upgradeError ? 'border-danger' : 'border-border focus:border-accent',
            )}
            placeholder={DEFAULT_UPGRADE_SERVICE_URL}
          />
          <button
            type="button"
            onClick={() => {
              setUpgradeUrl(DEFAULT_UPGRADE_SERVICE_URL);
              setUpgradeSaved(false);
              setUpgradeError('');
            }}
            disabled={upgradeLoading || upgradeSaving}
            className="shrink-0 px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
          >
            默认
          </button>
          <button
            type="button"
            onClick={saveUpgradeUrl}
            disabled={upgradeLoading || upgradeSaving}
            className={clsx(
              'shrink-0 inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md text-[12px] font-medium transition disabled:opacity-50 disabled:cursor-not-allowed',
              upgradeSaved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
            )}
          >
            {upgradeSaving ? (
              <>
                <span className="ace-spinner" style={{ width: 12, height: 12 }} />
                保存中...
              </>
            ) : (
              <>
                <VsIcon
                  name={upgradeSaved ? 'ok' : 'save'}
                  size={13}
                  mono={false}
                  className="ace-icon-on-accent"
                />
                {upgradeSaved ? '已保存' : '保存'}
              </>
            )}
          </button>
        </div>
        {upgradeError && <div className="mt-2 text-[12px] text-danger">{upgradeError}</div>}
      </div>

      <div className="text-[14px] font-semibold mb-1">工作空间依赖项</div>
      <p className="text-[12px] text-fg-mute mb-3">管理 ACECode 安装并提供给 Agent 使用的开发工具</p>

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
function SectionPersonalization() {
  const [text, setText] = useState('');
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api.getCustomInstructions()
      .then((cfg) => {
        if (!cancelled) setText(typeof cfg?.text === 'string' ? cfg.text : '');
      })
      .catch((e) => {
        if (!cancelled) {
          toast({ kind: 'err', text: '加载自定义指令失败:' + (e?.message || '') });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const save = async () => {
    if (saving || loading) return;
    setSaving(true);
    setSaved(false);
    try {
      const result = await api.setCustomInstructions({ text });
      if (typeof result?.text === 'string') setText(result.text);
      setSaved(true);
      toast({ kind: 'ok', text: '自定义指令已保存' });
      setTimeout(() => setSaved(false), 1500);
    } catch (e) {
      toast({ kind: 'err', text: '保存自定义指令失败:' + (e?.message || '') });
    } finally {
      setSaving(false);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">个性化</h2>

      <div className="text-[14px] font-semibold mb-1">自定义指令</div>
      <p className="text-[12px] text-fg-mute mb-3">为你的项目向 ACECode 提供额外说明和上下文</p>
      <p className="text-[12px] text-fg-mute mb-3">
        提示:自定义指令会参与每次请求的提示词上下文,频繁修改可能降低提示词缓存命中率。
      </p>

      <textarea
        value={text}
        onChange={(e) => { setText(e.target.value); setSaved(false); }}
        disabled={loading || saving}
        placeholder="例如:这个项目使用 React 18 + Vite,组件库选 Tailwind 风格,提交信息用中文..."
        rows={10}
        className="w-full px-3 py-2.5 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition leading-relaxed resize-y disabled:opacity-70"
        style={{ minHeight: 240 }}
      />

      <div className="flex justify-end mt-3">
        <button
          type="button"
          onClick={save}
          disabled={loading || saving}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
            (loading || saving) && 'opacity-60 cursor-not-allowed',
          )}
        >
          {saving ? '保存中...' : saved ? '已保存' : '保存'}
        </button>
      </div>
    </>
  );
}

// ─── 技能 ──────────────────────────────────────────────────────────────────
// 真实接入:GET /api/skills(?workspace= 可选,带 source/enabled 全量元数据)、
// PUT /api/skills/:name(?workspace= 供跨工作区校验)、GET /api/skills/root
// (path / global_path)、GET /api/workspaces。
// 结构:顶部全局技能(扁平列表 + 打开全局目录按钮),下方「工作区 Skill 目录」
// 每个已注册工作区一个折叠组,默认折叠;mount 后台预取各工作区技能做计数,
// 展开即渲染缓存,避免一次性渲染全部工作区的技能行。
// 过滤 / 分组 / 计数逻辑在 lib/skillsSettings.js(有 Node 单测)。

function parseDesktopBridgeResult(value) {
  // webview native binding 通常返回已解析的 JS 值;调试 shim 可能给原始字符串。
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function SkillRow({ skill, busy, onToggle }) {
  return (
    <div className="flex items-center gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
      <div
        className={clsx(
          'w-9 h-9 rounded-md border flex items-center justify-center shrink-0 transition',
          skill.enabled
            ? 'bg-accent-bg border-accent/40 text-accent'
            : 'bg-surface-alt border-border text-fg-mute',
        )}
      >
        <VsIcon name="lightbulb" size={18} />
      </div>
      <div className="flex-1 min-w-0">
        <div className="text-[13px] font-medium truncate">{skill.name}</div>
        <div className="text-[11px] text-fg-mute mt-0.5 truncate" title={skill.description || ''}>
          {skill.description || '—'}
        </div>
      </div>
      <Toggle on={skill.enabled} disabled={busy} onChange={(v) => onToggle(skill.name, v)} />
    </div>
  );
}

// 单个工作区的折叠组:头行(展开箭头 + 名称/cwd + N/M 计数 + 打开目录)+
// 展开后的技能行列表。skills 为 null/undefined 表示尚未加载完成。
function WorkspaceSkillGroup({
  ws,
  skills,
  expanded,
  onToggleExpand,
  query,
  busy,
  onToggleSkill,
  onOpenDir,
  openingDir,
}) {
  const loaded = Array.isArray(skills);
  const shown = loaded ? filterSkills(skills, query) : [];
  const hasQuery = !!String(query || '').trim();
  return (
    <div className="mb-2">
      <div
        role="button"
        tabIndex={0}
        aria-expanded={expanded}
        onClick={onToggleExpand}
        onKeyDown={(e) => {
          if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); onToggleExpand(); }
        }}
        className="flex items-center gap-2.5 px-3.5 py-2.5 rounded-md bg-surface border border-border cursor-pointer hover:bg-surface-hi transition"
      >
        <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={12} className="shrink-0 text-fg-mute" />
        <VsIcon name="folder" size={15} className="shrink-0 text-fg-2" />
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-medium truncate">{ws.name}</div>
          <div className="text-[11px] text-fg-mute truncate">{ws.cwd}</div>
        </div>
        <span className="text-[11px] text-fg-mute tabular-nums shrink-0">
          {loaded ? enabledRatioLabel(skills) : '…'}
        </span>
        <button
          type="button"
          title="打开该工作区的 Skill 目录"
          onClick={(e) => { e.stopPropagation(); onOpenDir(); }}
          disabled={!!openingDir}
          className="w-7 h-7 inline-flex items-center justify-center rounded text-fg-mute hover:text-fg hover:bg-surface-alt transition shrink-0 disabled:opacity-50"
        >
          <VsIcon name="folderOpen" size={14} />
        </button>
      </div>
      {expanded && (
        <div className="mt-2 pl-6">
          {!loaded && (
            <div className="px-3.5 py-3 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center mb-2">
              <span className="ace-spinner mr-2" /> 加载中
            </div>
          )}
          {loaded && shown.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mb-2">
              {hasQuery ? '无匹配技能' : '该工作区没有技能;放到项目的 .acecode/skills 目录即可被发现'}
            </div>
          )}
          {shown.map((s) => (
            <SkillRow key={s.name} skill={s} busy={busy} onToggle={onToggleSkill} />
          ))}
        </div>
      )}
    </div>
  );
}

function SectionSkills() {
  const [globalSkills, setGlobalSkills] = useState([]);
  const [workspaces, setWorkspaces] = useState([]);
  // hash -> 该工作区的项目技能数组;键缺失 = 还没加载完。
  const [wsSkills, setWsSkills] = useState({});
  const [expanded, setExpanded] = useState({});
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [search, setSearch] = useState('');
  const [savingName, setSavingName] = useState('');
  const [openingDir, setOpeningDir] = useState('');
  const slashCommandsCtx = useSlashCommands();

  const load = useCallback(async (refresh = false) => {
    if (refresh) setRefreshing(true); else setLoading(true);
    try {
      const [skillsRes, wsRes] = await Promise.allSettled([
        api.listSkills(),
        api.listWorkspaces(),
      ]);
      if (skillsRes.status === 'fulfilled') {
        setGlobalSkills(groupSkillsBySource(normalizeSkillList(skillsRes.value)).global);
      } else if (!refresh) {
        toast({ kind: 'err', text: '加载技能失败:' + (skillsRes.reason?.message || '') });
      }
      let wsList = wsRes.status === 'fulfilled' ? normalizeWorkspaceList(wsRes.value) : [];
      if (wsList.length === 0 && typeof window.aceDesktop_listWorkspaces === 'function') {
        try {
          wsList = normalizeWorkspaceList(
            parseDesktopBridgeResult(await window.aceDesktop_listWorkspaces()),
          );
        } catch { /* bridge 兜底失败就当没有工作区 */ }
      }
      setWorkspaces(wsList);
      if (refresh) setWsSkills({});
      // 后台预取各工作区的项目技能:折叠行的 N/M 计数需要它,展开时也能
      // 即刻渲染。逐个到达逐个填充,失败置空数组避免计数一直显示省略号。
      wsList.forEach((ws) => {
        api.listSkills(ws.hash)
          .then((list) => {
            const project = groupSkillsBySource(normalizeSkillList(list)).project;
            setWsSkills((prev) => ({ ...prev, [ws.hash]: project }));
          })
          .catch(() => {
            setWsSkills((prev) => ({ ...prev, [ws.hash]: [] }));
          });
      });
    } finally {
      if (refresh) setRefreshing(false); else setLoading(false);
    }
  }, []);

  useEffect(() => { load(); }, [load]);

  const summary = useMemo(() => skillsEnabledSummary(globalSkills), [globalSkills]);
  const filteredGlobal = useMemo(
    () => filterSkills(globalSkills, search),
    [globalSkills, search],
  );
  const hasQuery = !!search.trim();

  // 启停对 disabled 是全局生效的:同名技能出现在全局列表和多个工作区列表时
  // 一起翻转,失败整体回滚。
  const toggle = async (name, next, wsHash = '') => {
    if (savingName) return;
    const beforeGlobal = globalSkills;
    const beforeWs = wsSkills;
    const flip = (list) => list.map((s) => (s.name === name ? { ...s, enabled: next } : s));
    setGlobalSkills(flip(globalSkills));
    setWsSkills((prev) => {
      const out = {};
      for (const [hash, list] of Object.entries(prev)) out[hash] = flip(list);
      return out;
    });
    setSavingName(name);
    try {
      await api.setSkillEnabled(name, next, wsHash);
      slashCommandsCtx.invalidate?.();
    } catch (e) {
      setGlobalSkills(beforeGlobal);
      setWsSkills(beforeWs);
      toast({ kind: 'err', text: '切换技能失败:' + (e?.message || '') });
    } finally {
      setSavingName('');
    }
  };

  // 打开技能目录:desktop bridge 优先;webapp 兼容模式走 REST;都不可用时复制路径。
  const openDir = async (scope, wsHash = '') => {
    if (openingDir) return;
    setOpeningDir(scope + wsHash);
    try {
      const root = await api.getSkillRoot(wsHash);
      const path = scope === 'global' ? (root?.global_path || '') : (root?.path || '');
      if (!path) throw new Error('目录路径为空');
      if (typeof window.aceDesktop_openInExplorer === 'function') {
        const result = parseDesktopBridgeResult(await window.aceDesktop_openInExplorer(path));
        if (!result?.ok) throw new Error(result?.error || 'open failed');
        toast({ kind: 'ok', text: '已打开技能目录' });
        return;
      }
      try {
        const result = await api.openInExplorer(path);
        if (!result?.ok) throw new Error(result?.error || 'open failed');
        toast({ kind: 'ok', text: '已打开技能目录' });
      } catch {
        // REST 不可用(非 desktop 环境)→ 复制路径;复制也失败(如页面失焦)
        // 时直接把路径显示出来,不要报"失败"把路径吞掉。
        try {
          await navigator.clipboard.writeText(path);
          toast({ kind: 'ok', text: '技能目录路径已复制:' + path });
        } catch {
          toast({ kind: 'info', text: path });
        }
      }
    } catch (e) {
      toast({ kind: 'err', text: '打开技能目录失败:' + (e?.message || '') });
    } finally {
      setOpeningDir('');
    }
  };

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">技能</h2>
          <p className="text-[12px] text-fg-mute">
            管理 ACECode 可调用的技能模块。启用后 Agent 在任务中可自动使用。
          </p>
        </div>
        <div className="flex items-center gap-3 shrink-0">
          <span className="text-[12px] text-fg-mute tabular-nums">{summary.label}</span>
          <button
            type="button"
            onClick={() => load(true)}
            disabled={loading || refreshing}
            title="刷新技能列表"
            className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
          >
            <RefreshIcon size={15} className={clsx(refreshing && 'animate-spin')} />
          </button>
        </div>
      </div>

      <div className="relative mb-5">
        <VsIcon
          name="search"
          size={14}
          className="absolute left-3 top-1/2 -translate-y-1/2 text-fg-mute pointer-events-none"
        />
        <input
          type="text"
          value={search}
          onChange={(e) => setSearch(e.target.value)}
          placeholder="搜索技能名称或描述..."
          className="w-full h-9 pl-9 pr-3 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition"
        />
      </div>

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : (
        <>
          {filteredGlobal.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mb-2">
              {hasQuery ? '无匹配的全局技能' : '暂无全局技能'}
            </div>
          )}
          {filteredGlobal.map((s) => (
            <SkillRow key={s.name} skill={s} busy={!!savingName} onToggle={toggle} />
          ))}
          <button
            type="button"
            onClick={() => openDir('global')}
            disabled={!!openingDir}
            className="w-full h-10 mt-2 inline-flex items-center justify-center gap-2 rounded-md border border-border bg-surface text-[13px] text-fg hover:bg-surface-hi transition disabled:opacity-60"
          >
            <VsIcon name="folder" size={15} />
            打开全局 Skill 目录
          </button>

          <div className="h-px bg-border my-5" />

          <div className="text-[14px] font-semibold mb-1">工作区 Skill 目录</div>
          <p className="text-[12px] text-fg-mute mb-3">
            每个工作区可拥有独立的 Skill,仅在该工作区的会话中生效。
          </p>
          {workspaces.length === 0 && (
            <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center">
              暂无已注册的工作区
            </div>
          )}
          {workspaces.map((ws) => {
            const skills = wsSkills[ws.hash];
            const isExpanded = hasQuery
              ? workspaceAutoExpand(skills, search)
              : !!expanded[ws.hash];
            return (
              <WorkspaceSkillGroup
                key={ws.hash}
                ws={ws}
                skills={skills}
                expanded={isExpanded}
                onToggleExpand={() =>
                  setExpanded((prev) => ({ ...prev, [ws.hash]: !prev[ws.hash] }))
                }
                query={search}
                busy={!!savingName}
                onToggleSkill={(name, v) => toggle(name, v, ws.hash)}
                onOpenDir={() => openDir('workspace', ws.hash)}
                openingDir={openingDir}
              />
            );
          })}
        </>
      )}
    </>
  );
}

function SectionMCP() {
  const [text, setText] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [togglingName, setTogglingName] = useState('');

  // 从 JSON 编辑器文本派生开关列表:文本合法且是对象时才有内容,否则空数组。
  // 直接读文本(而非独立请求)保证开关与 JSON 编辑器永远同步。
  const { serverList, parsedOk } = useMemo(() => {
    try {
      const parsed = JSON.parse(text);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return { serverList: [], parsedOk: false };
      }
      return { serverList: buildMcpServerList(parsed), parsedOk: true };
    } catch {
      return { serverList: [], parsedOk: false };
    }
  }, [text]);

  const load = async () => {
    setLoading(true);
    setError('');
    try {
      const cfg = await api.getMcp();
      setText(JSON.stringify(cfg || {}, null, 2));
    } catch (e) {
      setError('加载 MCP 失败:' + (e?.message || ''));
      toast({ kind: 'err', text: '加载 MCP 失败:' + (e?.message || '') });
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    api.getMcp()
      .then((cfg) => {
        if (!cancelled) setText(JSON.stringify(cfg || {}, null, 2));
      })
      .catch((e) => {
        if (!cancelled) {
          setError('加载 MCP 失败:' + (e?.message || ''));
          toast({ kind: 'err', text: '加载 MCP 失败:' + (e?.message || '') });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const onChange = (value) => {
    setText(value);
    setSaved(false);
    try {
      JSON.parse(value);
      setError('');
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };

  const format = () => {
    try {
      const parsed = JSON.parse(text);
      setText(JSON.stringify(parsed, null, 2));
      setError('');
    } catch (e) {
      setError('JSON 格式错误:' + e.message);
    }
  };
  const save = async () => {
    try {
      const parsed = JSON.parse(text);
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        setError('JSON 必须是对象');
        return;
      }
      setError('');
      setSaving(true);
      await api.putMcp(parsed);
      setSaved(true);
      toast({ kind: 'ok', text: 'MCP 配置已保存;重启 daemon 后生效' });
      setTimeout(() => setSaved(false), 1500);
    } catch (e) {
      const msg = e instanceof SyntaxError ? 'JSON 格式错误:' + e.message : '保存失败:' + (e?.message || '');
      setError(msg);
      toast({ kind: 'err', text: msg });
    } finally {
      setSaving(false);
    }
  };
  const reload = async () => {
    try {
      const result = await api.reloadMcp();
      toast({ kind: 'ok', text: 'Reload: ' + JSON.stringify(result) });
    } catch {
      toast({ kind: 'err', text: '当前 daemon 需要重启后加载 MCP 配置' });
    }
  };

  // 开关某个 server:先把 disabled 写回 JSON 文本(编辑器同步),再调 toggle
  // 端点落盘 + 运行时热切换。失败回滚文本。applied=false 说明 daemon 未热应用,
  // 提示需重启。
  const toggleServer = async (name, enabled) => {
    if (togglingName) return;
    let nextText = text;
    try {
      const parsed = JSON.parse(text);
      nextText = JSON.stringify(applyMcpToggle(parsed, name, enabled), null, 2);
    } catch {
      toast({ kind: 'err', text: 'JSON 无效,无法切换' });
      return;
    }
    const prevText = text;
    setText(nextText);
    setSaved(false);
    setTogglingName(name);
    try {
      const res = await api.toggleMcpServer(name, enabled);
      if (res && res.applied === false) {
        toast({ kind: 'ok', text: `${name} 已${enabled ? '启用' : '关闭'};重启 daemon 后生效` });
      } else {
        toast({ kind: 'ok', text: `${name} 已${enabled ? '启用' : '关闭'}` });
      }
    } catch (e) {
      setText(prevText);
      toast({ kind: 'err', text: '切换失败:' + (e?.message || '') });
    } finally {
      setTogglingName('');
    }
  };

  const enabledCount = countEnabledMcp(serverList);

  return (
    <>
      <h2 className="text-xl font-bold mb-5">MCP 服务器</h2>

      <div className="text-[14px] font-semibold mb-1">服务器配置</div>
      <p className="text-[12px] text-fg-mute mb-3">
        直接编辑 JSON 配置 MCP 服务器连接(stdio / sse / http)
      </p>

      <textarea
        value={text}
        onChange={(e) => onChange(e.target.value)}
        spellCheck={false}
        disabled={loading}
        rows={18}
        className={clsx(
          'w-full px-4 py-3 text-[12px] rounded-md border bg-code-bg text-code-fg font-mono outline-none transition leading-relaxed resize-y',
          error ? 'border-danger' : 'border-border focus:border-accent',
        )}
        placeholder={loading ? '加载中...' : '{\n  \"filesystem\": { ... }\n}'}
        style={{ minHeight: 380, tabSize: 2 }}
      />
      {error && (
        <div className="mt-2 text-[12px] text-danger">{error}</div>
      )}

      <div className="flex justify-end gap-2 mt-3">
        <button
          type="button"
          onClick={format}
          disabled={loading}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi transition"
        >
          格式化
        </button>
        <button
          type="button"
          onClick={load}
          disabled={loading || saving}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
        >
          重新加载
        </button>
        <button
          type="button"
          onClick={reload}
          disabled={loading || saving}
          className="px-3 py-1.5 rounded-md text-[12px] border border-border text-fg-2 hover:bg-surface-hi disabled:opacity-50 transition"
        >
          Reload
        </button>
        <button
          type="button"
          onClick={save}
          disabled={loading || saving || !!error}
          className={clsx(
            'px-4 py-1.5 rounded-md text-[12px] font-medium transition disabled:opacity-50 disabled:cursor-not-allowed',
            saved ? 'bg-ok text-white' : 'bg-accent text-white hover:opacity-90',
          )}
        >
          {saving ? '保存中...' : (saved ? '✓ 已保存' : '保存')}
        </button>
      </div>

      <div className="mt-8 pt-6 border-t border-border">
        <div className="flex items-center justify-between mb-4">
          <div className="text-[15px] font-bold">启用服务器</div>
          {parsedOk && serverList.length > 0 && (
            <div className="text-[12px] text-fg-mute">
              {enabledCount} / {serverList.length} 已启用
            </div>
          )}
        </div>

        {loading ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            <span className="ace-spinner mr-2" /> 加载中
          </div>
        ) : !parsedOk ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            JSON 无效,修正后可在此逐个开关服务器。
          </div>
        ) : serverList.length === 0 ? (
          <div className="rounded-lg border border-border bg-surface px-4 py-4 text-[12px] text-fg-mute">
            暂无已配置的 MCP 服务器。
          </div>
        ) : (
          <div className="space-y-2 max-w-3xl">
            {serverList.map((server) => (
              <div
                key={server.name}
                className="rounded-lg border border-border bg-surface px-4 py-3 flex items-center gap-3"
              >
                <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
                  <VsIcon name="mcp" size={18} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    <div className="text-[13px] font-semibold text-fg truncate">
                      {server.name}
                    </div>
                    <span className="text-[10px] px-1.5 py-0.5 rounded border border-border text-fg-mute shrink-0">
                      {server.transportLabel}
                    </span>
                  </div>
                  {server.commandLine && (
                    <div className="mt-0.5 text-[11px] text-fg-mute font-mono break-all">
                      {server.commandLine}
                    </div>
                  )}
                </div>
                <Toggle
                  on={server.enabled}
                  onChange={(next) => toggleServer(server.name, next)}
                  disabled={!!togglingName || saving}
                />
              </div>
            ))}
          </div>
        )}
      </div>
    </>
  );
}

function SectionConnectors() {
  const [connectors, setConnectors] = useState([]);
  const [loading, setLoading] = useState(true);
  const [savingId, setSavingId] = useState('');
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    setLoading(true);
    setError('');
    try {
      const data = await api.getConnectors();
      setConnectors(normalizeConnectorList(data));
    } catch (e) {
      const message = e?.message || String(e);
      setError(message);
      toast({ kind: 'err', text: '加载连接器失败:' + message });
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.getConnectors()
      .then((data) => {
        if (!cancelled) setConnectors(normalizeConnectorList(data));
      })
      .catch((e) => {
        if (!cancelled) {
          const message = e?.message || String(e);
          setError(message);
          toast({ kind: 'err', text: '加载连接器失败:' + message });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const toggleConnector = async (connector, enabled) => {
    if (!connector?.id || savingId) return;
    const before = connectors;
    const next = applyConnectorToggle(connectors, connector.id, enabled);
    setConnectors(next);
    setSavingId(connector.id);
    setError('');
    try {
      const result = await api.setConnectors({ connectors: next });
      setConnectors(normalizeConnectorList(result));
      toast({ kind: 'ok', text: enabled ? '连接器已启用' : '连接器已关闭' });
    } catch (e) {
      const message = e?.message || String(e);
      setConnectors(before);
      setError(message);
      toast({ kind: 'err', text: '连接器保存失败:' + message });
    } finally {
      setSavingId('');
    }
  };

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">连接器</h2>
          <p className="text-[12px] text-fg-mute">config.json 中配置的连接器</p>
        </div>
        <button
          type="button"
          onClick={load}
          disabled={loading || !!savingId}
          title="刷新连接器"
          className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
        >
          <RefreshIcon size={15} className={clsx(loading && 'animate-spin')} />
        </button>
      </div>

      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
          {error}
        </div>
      )}

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : connectors.length === 0 ? (
        <div className="rounded-lg border border-border bg-surface px-4 py-4 max-w-3xl">
          <div className="text-[14px] font-semibold text-fg mb-1">暂无已配置连接器</div>
          <div className="text-[12px] text-fg-mute">没有可显示的连接器。</div>
        </div>
      ) : (
        <div className="space-y-3 max-w-5xl">
          {connectors.map((connector) => (
            <div
              key={connector.id}
              className="rounded-lg border border-border bg-surface px-4 py-3"
            >
              <div className="flex items-start gap-3">
                <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
                  <VsIcon name="extension" size={18} />
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    <div className="text-[13px] font-semibold text-fg truncate">
                      {connector.name || '未命名连接器'}
                    </div>
                    <span
                      className={clsx(
                        'text-[10px] px-1.5 py-0.5 rounded border shrink-0',
                        connector.enabled
                          ? 'border-ok-border bg-ok-bg text-ok'
                          : 'border-border text-fg-mute',
                      )}
                    >
                      {connector.enabled ? '已启用' : '已关闭'}
                    </span>
                  </div>
                  <div className="mt-1 text-[11px] text-fg-mute break-words">
                    {connector.description || '无描述'}
                  </div>
                </div>
                <Toggle
                  on={connector.enabled}
                  onChange={(next) => toggleConnector(connector, next)}
                  disabled={loading || !!savingId}
                />
              </div>
            </div>
          ))}
        </div>
      )}
    </>
  );
}

// ─── 工具 ──────────────────────────────────────────────────────────────────

function SectionTools() {
  const [bridgeEnabled, setBridgeEnabled] = useState(false);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.getAceBrowserBridge()
      .then((cfg) => {
        if (!cancelled) setBridgeEnabled(!!cfg?.enabled);
      })
      .catch((e) => {
        if (!cancelled) setError(e.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, []);

  const setBridge = async (next) => {
    const before = bridgeEnabled;
    setBridgeEnabled(next);
    setSaving(true);
    setError('');
    try {
      const saved = await api.setAceBrowserBridge({ enabled: next });
      setBridgeEnabled(!!saved?.enabled);
      toast({ kind: 'ok', text: next ? 'ACE Browser Bridge 已启用' : 'ACE Browser Bridge 已关闭' });
    } catch (e) {
      setBridgeEnabled(before);
      const message = e.message || String(e);
      setError(message);
      toast({ kind: 'err', text: message });
    } finally {
      setSaving(false);
    }
  };

  const tools = [
    {
      key: 'ace_browser_bridge',
      name: 'ACE Browser Bridge',
      desc: '启用 browser_start，并在打开后通过 user prompt 引导模型使用 ace-browser-host CLI。',
      icon: <VsIcon name="globe" size={20} />,
      on: bridgeEnabled,
      toggle: setBridge,
    },
  ];

  return (
    <>
      <h2 className="text-xl font-bold mb-5">工具</h2>

      <div className="text-[14px] font-semibold mb-1">内置工具</div>
      <p className="text-[12px] text-fg-mute mb-3">
        启用后 Agent 可在任务中自动调用；会写入 ace_browser_bridge 配置。
      </p>
      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger/40 bg-danger/10 text-danger text-[12px]">
          {error}
        </div>
      )}

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
          <Toggle on={tool.on} onChange={tool.toggle} disabled={loading || saving} />
        </div>
      ))}

      {/* 占位:更多工具即将加入 */}
      <div className="px-3.5 py-3 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center mt-2">
        更多内置工具即将加入
      </div>
    </>
  );
}

// ─── 钩子 ──────────────────────────────────────────────────────────────────

function SectionHooks() {
  const [snapshot, setSnapshot] = useState(() => normalizeHookSnapshot({ hooks: [] }));
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [busyId, setBusyId] = useState('');
  const [error, setError] = useState('');

  const applySnapshot = useCallback((data) => {
    setSnapshot(normalizeHookSnapshot(data || {}));
  }, []);

  const load = useCallback(async (refresh = false) => {
    if (refresh) setRefreshing(true); else setLoading(true);
    setError('');
    try {
      const data = refresh ? await api.refreshHooks() : await api.listHooks();
      applySnapshot(data);
    } catch (e) {
      const message = hookSettingsErrorMessage(e);
      setError(message);
      toast({ kind: 'err', text: '加载钩子失败:' + message });
    } finally {
      if (refresh) setRefreshing(false); else setLoading(false);
    }
  }, [applySnapshot]);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.listHooks()
      .then((data) => {
        if (!cancelled) applySnapshot(data);
      })
      .catch((e) => {
        if (!cancelled) {
          const message = hookSettingsErrorMessage(e);
          setError(message);
          toast({ kind: 'err', text: '加载钩子失败:' + message });
        }
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, [applySnapshot]);

  const runAction = async (hook, action) => {
    if (!hook?.id) return;
    const token = `${hook.id}:${action}`;
    setBusyId(token);
    setError('');
    try {
      let data;
      if (action === 'trust') data = await api.trustHook(hook.id);
      else if (action === 'disable') data = await api.disableHook(hook.id);
      else data = await api.enableHook(hook.id);
      applySnapshot(data);
      const label = action === 'trust' ? '已信任钩子' : (action === 'disable' ? '已禁用钩子' : '已启用钩子');
      toast({ kind: 'ok', text: label });
    } catch (e) {
      const message = hookSettingsErrorMessage(e);
      setError(message);
      toast({ kind: 'err', text: '钩子操作失败:' + message });
    } finally {
      setBusyId('');
    }
  };

  const empty = hookEmptyState(snapshot);

  return (
    <>
      <div className="flex items-start justify-between gap-4 mb-5">
        <div>
          <h2 className="text-xl font-bold mb-2">钩子</h2>
          <p className="text-[12px] text-fg-mute">
            通过配置和已启用的插件管理生命周期钩子。
            <button
              type="button"
              onClick={() => openExternalUrl('https://developers.openai.com/codex/hooks')}
              className="ml-2 text-accent hover:underline"
            >
              了解更多
            </button>
          </p>
        </div>
        <button
          type="button"
          onClick={() => load(true)}
          disabled={loading || refreshing}
          title="刷新钩子"
          className="h-8 w-8 inline-flex items-center justify-center rounded-md border border-border bg-surface text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
        >
          <RefreshIcon size={15} className={clsx(refreshing && 'animate-spin')} />
        </button>
      </div>

      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px]">
          {error}
        </div>
      )}

      {loading ? (
        <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : snapshot.isEmpty ? (
        <div className="rounded-lg border border-border bg-surface px-4 py-4 max-w-3xl">
          <div className="text-[14px] font-semibold text-fg mb-1">{empty.title}</div>
          <div className="text-[12px] text-fg-mute">{empty.body}</div>
        </div>
      ) : (
        <div className="space-y-3 max-w-5xl">
          {snapshot.hooks.map((hook) => (
            <HookListItem
              key={hook.id}
              hook={hook}
              busyId={busyId}
              onTrust={() => runAction(hook, 'trust')}
              onDisable={() => runAction(hook, 'disable')}
              onEnable={() => runAction(hook, 'enable')}
            />
          ))}
        </div>
      )}

      {!loading && snapshot.diagnostics.length > 0 && (
        <div className="mt-4 rounded-md border border-border bg-surface px-3.5 py-3">
          <div className="text-[12px] font-semibold text-fg-2 mb-2">发现诊断</div>
          <div className="space-y-1">
            {snapshot.diagnostics.slice(0, 8).map((diag, index) => (
              <div key={`${diag.code}-${index}`} className="text-[11px] text-fg-mute">
                <span className="text-fg-2">{diag.code || diag.severity}</span>
                {diag.message ? ` · ${diag.message}` : ''}
              </div>
            ))}
          </div>
        </div>
      )}
    </>
  );
}

function HookListItem({ hook, busyId, onTrust, onDisable, onEnable }) {
  const actions = hookActionState(hook);
  const busyTrust = busyId === `${hook.id}:trust`;
  const busyDisable = busyId === `${hook.id}:disable`;
  const busyEnable = busyId === `${hook.id}:enable`;
  const sourceLabel = hook.sourcePath || hook.sourceId || '未知来源';
  const commandText = hook.commandWindows
    ? `${hook.command} | Windows: ${hook.commandWindows}`
    : hook.command;

  return (
    <div className="rounded-lg border border-border bg-surface px-4 py-3">
      <div className="flex items-start gap-3">
        <div className="h-9 w-9 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg-2 shrink-0">
          <VsIcon name="hook" size={18} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2 min-w-0">
            <div className="text-[13px] font-semibold text-fg truncate">{hook.eventName || 'Hook'}</div>
            <HookBadge hook={hook} />
            {hook.managed && (
              <span className="text-[10px] px-1.5 py-0.5 rounded border border-border text-fg-mute">managed</span>
            )}
          </div>
          <div className="mt-1 flex flex-wrap gap-x-3 gap-y-1 text-[11px] text-fg-mute">
            <span>匹配: <span className="text-fg-2">{hook.matcher}</span></span>
            <span>来源: <span className="text-fg-2 break-all">{sourceLabel}</span></span>
            {hook.timeoutSeconds > 0 && <span>超时: {hook.timeoutSeconds}s</span>}
          </div>
          {commandText && (
            <div className="mt-2 rounded-md border border-border bg-code-bg px-2.5 py-1.5 font-mono text-[11px] text-code-fg break-all">
              {commandText}
            </div>
          )}
          {hook.statusMessage && (
            <div className="mt-1 text-[11px] text-fg-mute">状态消息: {hook.statusMessage}</div>
          )}
          {hook.skipReason && (
            <div className="mt-1 text-[11px] text-warn">跳过原因: {hook.skipReason}</div>
          )}
          {hook.diagnostics.length > 0 && (
            <div className="mt-2 space-y-1">
              {hook.diagnostics.map((diag, index) => (
                <div key={`${diag.code}-${index}`} className="text-[11px] text-fg-mute">
                  <span className="text-warn">{diag.code || diag.severity}</span>
                  {diag.message ? ` · ${diag.message}` : ''}
                </div>
              ))}
            </div>
          )}
        </div>
        <div className="shrink-0 flex items-center gap-2">
          {actions.canTrust && (
            <button
              type="button"
              onClick={onTrust}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 transition disabled:opacity-50"
            >
              {busyTrust ? <span className="ace-spinner" /> : <VsIcon name="check" size={12} />}
              信任
            </button>
          )}
          {actions.canEnable && (
            <button
              type="button"
              onClick={onEnable}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border border-border text-fg-2 bg-surface-alt text-[12px] hover:bg-surface-hi transition disabled:opacity-50"
            >
              {busyEnable ? <span className="ace-spinner" /> : <VsIcon name="run" size={12} />}
              启用
            </button>
          )}
          {actions.canDisable && (
            <button
              type="button"
              onClick={onDisable}
              disabled={busyTrust || busyDisable || busyEnable}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border border-border text-fg-2 bg-surface-alt text-[12px] hover:bg-surface-hi transition disabled:opacity-50"
            >
              {busyDisable ? <span className="ace-spinner" /> : <VsIcon name="stop" size={12} />}
              禁用
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

function HookBadge({ hook }) {
  const label = hookStatusLabel(hook);
  const cls = hook.disabled
    ? 'border-danger text-danger'
    : hook.pendingReview
      ? 'border-warn text-warn'
      : hook.trusted || hook.managed
        ? 'border-ok text-ok'
        : 'border-border text-fg-mute';
  return (
    <span className={clsx('text-[10px] px-1.5 py-0.5 rounded border shrink-0', cls)}>
      {label}
    </span>
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

const USAGE_COLORS = [
  'var(--ace-accent)',
  'var(--ace-ok)',
  'var(--ace-warn)',
  'var(--ace-danger)',
  '#8b5cf6',
  '#06b6d4',
];

function shortUsageDate(date) {
  const text = String(date || '');
  const m = text.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  return m ? `${Number(m[2])}/${Number(m[3])}` : text;
}

function UsageEmptyState({ text }) {
  return (
    <div className="px-3.5 py-8 rounded-md bg-surface border border-border text-[12px] text-fg-mute text-center">
      {text}
    </div>
  );
}

function SectionUsage() {
  const [raw, setRaw] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [reloadKey, setReloadKey] = useState(0);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError('');
    api.getUsageStats({ days: 30, timezoneOffsetMinutes: new Date().getTimezoneOffset() })
      .then((data) => {
        if (!cancelled) setRaw(data || {});
      })
      .catch((e) => {
        if (!cancelled) setError(e.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => { cancelled = true; };
  }, [reloadKey]);

  const stats = useMemo(() => normalizeUsageStats(raw || {}), [raw]);
  const note = usageDataNote(stats);
  const summary = [
    {
      label: `${stats.metadata.days} 天 Tokens`,
      value: formatUsageTokens(stats.summary.totals.totalTokens),
      sub: `${formatUsageTokens(stats.summary.totals.promptTokens)} 输入 / ${formatUsageTokens(stats.summary.totals.completionTokens)} 输出`,
    },
    {
      label: '用量记录',
      value: String(stats.summary.records),
      sub: stats.hasEstimates ? `${stats.summary.estimatedRecords} 条估算` : 'provider usage',
    },
    {
      label: '会话',
      value: String(stats.summary.sessionCount),
      sub: `${stats.models.length} 个模型`,
    },
  ];
  const tokenDetails = [
    ['输入', stats.summary.totals.promptTokens],
    ['输出', stats.summary.totals.completionTokens],
    ['缓存读', stats.summary.totals.cacheReadTokens],
    ['缓存写', stats.summary.totals.cacheWriteTokens],
    ['推理', stats.summary.totals.reasoningTokens],
    ['总计', stats.summary.totals.totalTokens],
  ];

  return (
    <>
      <div className="flex items-center justify-between mb-5">
        <h2 className="text-xl font-bold">使用情况</h2>
        <button
          type="button"
          onClick={() => setReloadKey((v) => v + 1)}
          disabled={loading}
          className="px-2.5 h-7 rounded-md text-[12px] border border-border bg-surface hover:bg-surface-hi disabled:opacity-60 transition flex items-center gap-1.5"
        >
          {loading ? <span className="ace-spinner" /> : <RefreshIcon size={13} />}
          刷新
        </button>
      </div>

      {loading && !raw ? (
        <UsageEmptyState text="加载中" />
      ) : error ? (
        <UsageEmptyState text={`加载失败:${error}`} />
      ) : !stats.hasData ? (
        <>
          <div className="grid grid-cols-1 xl:grid-cols-3 gap-3 mb-6">
            {summary.map((c) => (
              <div key={c.label} className="px-4 py-3.5 rounded-md bg-surface border border-border">
                <div className="text-[10px] text-fg-mute uppercase tracking-wider mb-1.5">{c.label}</div>
                <div className="text-[24px] font-bold text-fg leading-none mb-1">{c.value}</div>
                <div className="text-[11px] text-fg-mute">{c.sub}</div>
              </div>
            ))}
          </div>
          <UsageEmptyState text={note} />
        </>
      ) : (
        <>
          <div className="grid grid-cols-1 xl:grid-cols-3 gap-3 mb-6">
            {summary.map((c) => (
              <div key={c.label} className="px-4 py-3.5 rounded-md bg-surface border border-border">
                <div className="text-[10px] text-fg-mute uppercase tracking-wider mb-1.5">{c.label}</div>
                <div className="text-[24px] font-bold text-fg leading-none mb-1">{c.value}</div>
                <div className="text-[11px] text-fg-mute truncate">{c.sub}</div>
              </div>
            ))}
          </div>

          <div className="text-[14px] font-semibold mb-1">每日用量趋势</div>
          <p className="text-[12px] text-fg-mute mb-3">近 {stats.metadata.days} 天 token 消耗</p>
          <div className="px-4 pt-4 pb-2 rounded-md bg-surface border border-border mb-6">
            <div className="flex items-stretch gap-1.5 h-[150px]">
              {stats.daily.map((d) => {
                const h = stats.maxDailyTokens > 0 ? (d.tokens / stats.maxDailyTokens) * 100 : 0;
                return (
                  <div key={d.date} className="flex-1 min-w-[24px] h-full flex flex-col items-center gap-1.5">
                    <div className="text-[9px] text-fg-mute opacity-80 whitespace-nowrap">
                      {d.tokens > 0 ? formatUsageTokens(d.tokens) : ''}
                    </div>
                    <div className="w-full flex-1 flex items-end">
                      <div
                        className="w-full rounded-sm bg-accent transition-all"
                        style={{ height: `${h}%`, minHeight: d.tokens > 0 ? 6 : 2, opacity: d.tokens > 0 ? 0.9 : 0.18 }}
                      />
                    </div>
                    <div className="text-[10px] text-fg-mute whitespace-nowrap">{shortUsageDate(d.date)}</div>
                  </div>
                );
              })}
            </div>
          </div>

          <div className="grid grid-cols-2 xl:grid-cols-6 gap-2 mb-6">
            {tokenDetails.map(([label, value]) => (
              <div key={label} className="px-3 py-2.5 rounded-md bg-surface border border-border">
                <div className="text-[11px] text-fg-mute mb-1">{label}</div>
                <div className="text-[14px] font-semibold">{formatUsageTokens(value)}</div>
              </div>
            ))}
          </div>

          <div className="text-[14px] font-semibold mb-1">模型用量明细</div>
          <p className="text-[12px] text-fg-mute mb-3">每个模型的输入 / 输出 token 与用量记录</p>
          <div className="rounded-md bg-surface border border-border overflow-hidden mb-6">
            {stats.models.length === 0 ? (
              <div className="px-3.5 py-6 text-[12px] text-fg-mute text-center">暂无模型用量</div>
            ) : stats.models.map((m, i) => {
              const total = m.totals.totalTokens;
              const barWidth = stats.maxModelTokens > 0 ? (total / stats.maxModelTokens) * 100 : 0;
              const inputPct = total > 0 ? (m.totals.promptTokens / total) * 100 : 0;
              const color = USAGE_COLORS[i % USAGE_COLORS.length];
              return (
                <div
                  key={`${m.provider}:${m.model}:${m.modelPreset}:${i}`}
                  className={clsx(
                    'px-4 py-3.5',
                    i < stats.models.length - 1 && 'border-b border-border',
                  )}
                >
                  <div className="flex items-center justify-between mb-2 gap-3">
                    <div className="flex items-center gap-2 min-w-0">
                      <span className="w-2.5 h-2.5 rounded-sm shrink-0" style={{ background: color }} />
                      <span className="text-[13px] font-semibold truncate">{m.label}</span>
                    </div>
                    <div className="flex items-center gap-4 shrink-0">
                      <span className="text-[12px] text-fg-mute">{m.records} 条</span>
                      <span className="text-[13px] font-semibold">{formatUsageTokens(total)}</span>
                    </div>
                  </div>
                  <div className="h-1.5 rounded-sm bg-surface-hi overflow-hidden mb-1.5">
                    <div className="h-full flex" style={{ width: `${barWidth}%`, minWidth: total > 0 ? 6 : 0 }}>
                      <div className="h-full" style={{ width: `${inputPct}%`, background: color }} />
                      <div className="h-full flex-1" style={{ background: color, opacity: 0.38 }} />
                    </div>
                  </div>
                  <div className="flex flex-wrap gap-x-4 gap-y-1 text-[11px] text-fg-mute">
                    <span>输入 {formatUsageTokens(m.totals.promptTokens)}</span>
                    <span>输出 {formatUsageTokens(m.totals.completionTokens)}</span>
                    <span>{m.sessionCount} 会话</span>
                    {m.estimatedRecords > 0 && <span>{m.estimatedRecords} 估算</span>}
                  </div>
                </div>
              );
            })}
          </div>

          <div className="text-[14px] font-semibold mb-1">工作区用量</div>
          <p className="text-[12px] text-fg-mute mb-3">按工作区汇总 token 消耗</p>
          <div className="rounded-md bg-surface border border-border overflow-hidden">
            {stats.workspaces.length === 0 ? (
              <div className="px-3.5 py-6 text-[12px] text-fg-mute text-center">暂无工作区用量</div>
            ) : stats.workspaces.map((w, i) => {
              const total = w.totals.totalTokens;
              const width = stats.maxWorkspaceTokens > 0 ? (total / stats.maxWorkspaceTokens) * 100 : 0;
              return (
                <div
                  key={`${w.workspaceHash}:${i}`}
                  className={clsx('px-4 py-3', i < stats.workspaces.length - 1 && 'border-b border-border')}
                >
                  <div className="flex items-center justify-between gap-3 mb-2">
                    <div className="min-w-0">
                      <div className="text-[13px] font-medium truncate">{w.workspaceName || 'workspace'}</div>
                      <div className="text-[11px] text-fg-mute truncate">{w.cwd}</div>
                    </div>
                    <div className="text-[13px] font-semibold shrink-0">{formatUsageTokens(total)}</div>
                  </div>
                  <div className="h-1.5 rounded-sm bg-surface-hi overflow-hidden">
                    <div className="h-full bg-accent" style={{ width: `${width}%`, minWidth: total > 0 ? 6 : 0, opacity: 0.85 }} />
                  </div>
                </div>
              );
            })}
          </div>

          <div className="mt-4 px-3.5 py-2.5 rounded-md border border-dashed border-border text-[12px] text-fg-mute text-center">
            {note}
          </div>
        </>
      )}
    </>
  );
}

// ─── 问题反馈 ──────────────────────────────────────────────────────────────

function feedbackSessionOptionLabel(item) {
  const title = sessionDisplayTitle(item, item?.title || item?.summary || item?.id || '');
  const when = relativeTime(item?.updated_at || item?.created_at);
  const workspace = item?.workspaceName || item?.cwd || item?.workspace_hash || '';
  return [title, when, workspace].filter(Boolean).join(' · ');
}

function SectionFeedback() {
  const [feedbackText, setFeedbackText] = useState('');
  const [sessionsRaw, setSessionsRaw] = useState(null);
  const [selectedKey, setSelectedKey] = useState(NO_FEEDBACK_SESSION_KEY);
  const [loadingSessions, setLoadingSessions] = useState(true);
  const [sessionsError, setSessionsError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [status, setStatus] = useState(null);

  const sessions = useMemo(
    () => normalizeDesktopFeedbackSessions(sessionsRaw || {}),
    [sessionsRaw],
  );
  const selectedSession = useMemo(
    () => selectedFeedbackSessionFromKey(sessions, selectedKey),
    [sessions, selectedKey],
  );

  const loadSessions = useCallback(() => {
    let cancelled = false;
    setLoadingSessions(true);
    setSessionsError('');
    api.listDesktopFeedbackSessions(20)
      .then((data) => {
        if (!cancelled) setSessionsRaw(data || {});
      })
      .catch((e) => {
        if (!cancelled) setSessionsError(e?.message || String(e));
      })
      .finally(() => {
        if (!cancelled) setLoadingSessions(false);
      });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => loadSessions(), [loadSessions]);

  const submit = async () => {
    if (submitting) return;
    setSubmitting(true);
    setStatus(null);
    try {
      const payload = buildDesktopFeedbackPayload({
        feedbackText,
        selectedSession,
      });
      const result = await api.submitDesktopFeedback(payload);
      const filename = result?.package_filename || 'feedback package';
      setStatus({
        kind: 'ok',
        text: `反馈已上传:${filename}`,
      });
      toast({ kind: 'ok', text: '问题反馈已上传' });
      setFeedbackText('');
      setSelectedKey(NO_FEEDBACK_SESSION_KEY);
    } catch (e) {
      const body = e?.body && typeof e.body === 'object' ? e.body : {};
      const message = lookupErrorMessage(e?.code, body.message || e?.message || String(e));
      setStatus({
        kind: 'err',
        text: `上传失败:${message}`,
        packagePath: body.package_path || '',
      });
      toast({ kind: 'err', text: '问题反馈上传失败' });
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-5">问题反馈</h2>

      <div className="rounded-md bg-surface border border-border overflow-hidden">
        <div className="px-4 py-3.5 border-b border-border">
          <div className="text-[14px] font-semibold mb-1">提交反馈</div>
          <p className="text-[12px] text-fg-mute">
            默认只附带最近 desktop 日志。关联某个具体的会话记录将更有助于我们帮您排查问题。
          </p>
        </div>

        <div className="px-4 py-4 space-y-4">
          <label className="block">
            <span className="block text-[12px] font-medium text-fg-2 mb-1.5">反馈内容</span>
            <textarea
              value={feedbackText}
              onChange={(e) => setFeedbackText(e.target.value)}
              rows={5}
              placeholder="描述你遇到的问题"
              className="w-full resize-y min-h-[112px] rounded-md bg-bg border border-border px-3 py-2 text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent"
            />
          </label>

          <div>
            <div className="flex items-center justify-between mb-1.5">
              <span className="text-[12px] font-medium text-fg-2">最近会话记录</span>
              <button
                type="button"
                onClick={loadSessions}
                disabled={loadingSessions || submitting}
                className="h-6 px-2 rounded-md text-[11px] text-fg-2 bg-surface-hi hover:bg-surface-alt border border-border disabled:opacity-60 transition flex items-center gap-1"
              >
                {loadingSessions ? <span className="ace-spinner" /> : <RefreshIcon size={12} />}
                刷新
              </button>
            </div>
            <div className="flex gap-2">
              <select
                value={selectedKey}
                onChange={(e) => setSelectedKey(e.target.value)}
                disabled={loadingSessions || submitting}
                className="min-w-0 flex-1 h-8 rounded-md bg-bg border border-border px-2 text-[13px] text-fg outline-none focus:border-accent"
              >
                <option value={NO_FEEDBACK_SESSION_KEY}>不附带会话</option>
                {sessions.map((item) => (
                  <option key={feedbackSessionKey(item)} value={feedbackSessionKey(item)}>
                    {feedbackSessionOptionLabel(item)}
                  </option>
                ))}
              </select>
              {selectedKey !== NO_FEEDBACK_SESSION_KEY && (
                <button
                  type="button"
                  onClick={() => setSelectedKey(NO_FEEDBACK_SESSION_KEY)}
                  disabled={submitting}
                  className="shrink-0 h-8 px-2.5 rounded-md text-[12px] border border-border bg-surface hover:bg-surface-hi disabled:opacity-60 transition"
                >
                  清除
                </button>
              )}
            </div>
            {sessionsError ? (
              <div className="mt-2 text-[12px] text-danger">加载会话失败:{sessionsError}</div>
            ) : (
              <div className="mt-2 text-[12px] text-fg-mute">
                {selectedSession
                  ? `将附带:${feedbackSessionOptionLabel(selectedSession)}`
                  : '不会附带会话数据库或会话记录。'}
              </div>
            )}
          </div>

          {status && (
            <div
              className={clsx(
                'rounded-md border px-3 py-2 text-[12px]',
                status.kind === 'ok'
                  ? 'border-ok-border bg-ok-bg text-ok'
                  : 'border-danger bg-surface text-danger',
              )}
            >
              <div>{status.text}</div>
              {status.packagePath && (
                <div className="mt-1 text-[11px] break-all">{status.packagePath}</div>
              )}
            </div>
          )}

          <div className="flex items-center justify-end">
            <button
              type="button"
              onClick={submit}
              disabled={submitting}
              className="h-8 px-3 rounded-md text-[13px] font-medium bg-accent text-white hover:opacity-95 disabled:opacity-60 transition flex items-center gap-1.5"
            >
              {submitting ? <span className="ace-spinner" /> : <VsIcon name="send" size={13} />}
              提交反馈
            </button>
          </div>
        </div>
      </div>
    </>
  );
}

// ─── 模型 ────────────────────────────────────────────────────────────────
// Claude Design 高保真原型 (panels.jsx::renderModelContent) 的真实接入版本。
// saved_models 仍是唯一持久化来源;多选模型提交时拆成多个 saved model 条目。

const MODEL_NEW_PROVIDER_PILL = {
  openai:    'text-ok bg-ok-bg border border-ok-border',
  anthropic: 'text-warn bg-warn-bg border border-warn',
  copilot:   'text-accent bg-accent-bg border border-accent-soft',
};

const MODEL_NEW_DRAFT_DEFAULT = {
  name: '',
  provider: 'openai',
  model: '',
  base_url: OPENAI_DEFAULT_BASE_URL,
  api_key: '',
  request_headers_json: '',
  context_window_k: '',
  capabilities: DEFAULT_MODEL_CAPABILITIES,
};

function draftFromModelProfile(m) {
  return {
    name: m?.name || '',
    provider: m?.provider || 'openai',
    model: m?.model || '',
    base_url: m?.base_url || '',
    api_key: (m?.provider === 'openai' || m?.provider === 'anthropic') ? (m?.api_key || '') : '',
    request_headers_json: formatRequestHeadersJson(m?.request_headers),
    context_window_k: formatContextWindowK(m?.context_window),
    capabilities: normalizeModelCapabilities(m?.capabilities),
  };
}

function payloadForModelDraft(draft, { omitApiKey = false, requestHeaders } = {}) {
  const payload = {
    name: String(draft.name || '').trim(),
    provider: draft.provider || 'openai',
    model: String(draft.model || '').trim(),
  };
  if (payload.provider === 'openai' || payload.provider === 'anthropic') {
    payload.base_url = String(draft.base_url || '').trim();
    if (!omitApiKey) payload.api_key = String(draft.api_key || '');
    if (requestHeaders !== undefined) payload.request_headers = requestHeaders;
  }
  const parsedContext = parseContextWindowK(draft.context_window_k);
  payload.context_window = parsedContext.ok && parsedContext.tokens ? parsedContext.tokens : 0;
  payload.capabilities = normalizeModelCapabilities(draft.capabilities);
  return payload;
}

function providerLabel(provider) {
  if (provider === 'copilot') return 'Copilot';
  if (provider === 'anthropic') return 'Anthropic';
  return 'OpenAI';
}

function capabilityLabel(id) {
  return MODEL_CAPABILITY_OPTIONS.find((item) => item.id === id)?.label || id;
}

function CapabilityIcon({ id, size = 14, className = '' }) {
  const icon = {
    vision: 'eye',
    web_search: 'globe',
    reasoning: 'brain',
    tool_use: 'tool',
    rerank: 'list',
    embedding: 'embedding',
  }[id] || 'settings';
  return <VsIcon name={icon} size={size} className={className} />;
}

function CapabilityBadges({ capabilities = [], compact = false }) {
  const tags = normalizeModelCapabilities(capabilities);
  if (tags.length === 0) return null;
  return (
    <div className="flex items-center gap-1">
      {tags.map((id) => (
        <span
          key={id}
          title={capabilityLabel(id)}
          className={clsx(
            'inline-flex items-center justify-center rounded-md border border-border bg-surface-hi text-fg-2',
            compact ? 'w-6 h-6' : 'w-7 h-7',
            id === 'tool_use' && 'border-warn bg-warn-bg text-warn',
            id === 'vision' && 'border-accent-soft bg-accent-bg text-accent',
          )}
        >
          <CapabilityIcon id={id} size={compact ? 12 : 14} />
        </span>
      ))}
    </div>
  );
}

function SectionModel() {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState('');
  const [copilotAuth, setCopilotAuth] = useState({
    loading: true,
    authenticated: false,
    has_token: false,
  });
  const [copilotBusy, setCopilotBusy] = useState('');
  const [copilotFlow, setCopilotFlow] = useState(null);
  const [showAdd, setShowAdd] = useState(false);
  const [addDraft, setAddDraft] = useState(MODEL_NEW_DRAFT_DEFAULT);
  const [expandedModelNames, setExpandedModelNames] = useState(() => new Set());
  const [editDrafts, setEditDrafts] = useState({});
  const [savedModelFilter, setSavedModelFilter] = useState('');
  const addModelTitleRef = useRef(null);
  const pendingAddScrollRef = useRef(false);
  const filteredModels = useMemo(
    () => filterSavedModels(models, savedModelFilter),
    [models, savedModelFilter],
  );
  const visibleModels = useMemo(
    () => {
      if (expandedModelNames.size === 0) return filteredModels;
      const filteredNames = new Set(filteredModels.map((m) => m?.name).filter(Boolean));
      return models.filter((m) => filteredNames.has(m?.name) || expandedModelNames.has(m?.name));
    },
    [expandedModelNames, filteredModels, models],
  );

  const refreshCopilotAuth = useCallback(async () => {
    setCopilotAuth((s) => ({ ...s, loading: true }));
    try {
      const state = await api.getCopilotAuth();
      setCopilotAuth({
        loading: false,
        authenticated: !!state?.authenticated,
        has_token: !!state?.has_token,
      });
    } catch {
      setCopilotAuth({ loading: false, authenticated: false, has_token: false });
    }
  }, []);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const [list, d] = await Promise.all([
        api.listModels(),
        api.getDefaultModel().catch(() => ({ name: '' })),
      ]);
      setModels(Array.isArray(list) ? list : []);
      setDefaultName(d?.name || d?.default_model_name || '');
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setLoading(false);
    }
  }, []);

  const refreshAndCollapseModels = useCallback(() => {
    setExpandedModelNames((prev) => (prev.size > 0 ? new Set() : prev));
    setEditDrafts((prev) => (Object.keys(prev).length > 0 ? {} : prev));
    void refresh();
  }, [refresh]);

  useEffect(() => {
    refresh();
  }, [refresh]);
  useEffect(() => {
    refreshCopilotAuth();
  }, [refreshCopilotAuth]);

  const pollCopilotFlow = useCallback(async () => {
    if (!copilotFlow?.device_code || copilotBusy) return;
    setCopilotBusy('poll');
    try {
      const state = await api.pollCopilotAuth(copilotFlow.device_code);
      if (state?.status === 'authenticated') {
        setCopilotFlow(null);
        setCopilotAuth({ loading: false, authenticated: true, has_token: true });
        toast({ kind: 'ok', text: 'Copilot 已登录' });
        return;
      }
      const intervalDelta = Number(state?.interval_delta_seconds || 0);
      setCopilotFlow((flow) => flow ? {
        ...flow,
        status: state?.status || 'pending',
        message: state?.message || (state?.status === 'slow_down' ? '轮询间隔已调整' : '等待 GitHub 授权'),
        interval: Math.max(1, Number(flow.interval || 5) + (intervalDelta > 0 ? intervalDelta : 0)),
      } : flow);
      if (state?.status === 'expired' || state?.status === 'failed') {
        toast({ kind: 'err', text: state?.message || 'Copilot 登录失败' });
      }
    } catch (err) {
      setCopilotFlow((flow) => flow ? {
        ...flow,
        status: 'failed',
        message: lookupErrorMessage(err?.code, err?.message),
      } : flow);
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setCopilotBusy('');
    }
  }, [copilotBusy, copilotFlow]);

  useEffect(() => {
    if (!copilotFlow?.device_code) return undefined;
    if (copilotBusy) return undefined;
    if (copilotFlow.status === 'authenticated' ||
        copilotFlow.status === 'expired' ||
        copilotFlow.status === 'failed') {
      return undefined;
    }
    const delay = Math.max(1, Number(copilotFlow.interval || 5)) * 1000;
    const id = window.setTimeout(() => { pollCopilotFlow(); }, delay);
    return () => window.clearTimeout(id);
  }, [copilotBusy, copilotFlow, pollCopilotFlow]);

  const copyCopilotUserCode = useCallback(async (userCode, { silent = false } = {}) => {
    const result = await copyTextToSystemClipboard(userCode);
    if (result.ok) {
      if (!silent) toast({ kind: 'ok', text: '验证码已复制' });
      return result;
    }
    if (!silent) {
      toast({ kind: 'err', text: '复制验证码失败:' + (result.error || '') });
    }
    return result;
  }, []);

  const startCopilotLogin = async () => {
    if (copilotBusy) return;
    setCopilotBusy('start');
    try {
      const flow = await api.startCopilotAuth();
      setCopilotFlow({
        ...flow,
        status: 'pending',
        interval: Math.max(1, Number(flow?.interval || 5)),
        message: '等待 GitHub 授权',
      });
      const copyResult = flow?.user_code
        ? await copyCopilotUserCode(flow.user_code, { silent: true })
        : null;
      if (copyResult && !copyResult.ok) {
        toast({ kind: 'err', text: '验证码自动复制失败:' + (copyResult.error || '') });
      }
      if (flow?.verification_uri) {
        const opened = await openExternalUrl(flow.verification_uri);
        if (!opened.ok) {
          toast({ kind: 'err', text: '无法打开系统浏览器:' + (opened.error || '') });
        }
      }
      toast({ kind: 'ok', text: copyResult?.ok ? 'Copilot 登录已开始,验证码已复制' : 'Copilot 登录已开始' });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setCopilotBusy('');
    }
  };

  const logoutCopilot = async () => {
    if (copilotBusy) return;
    setCopilotBusy('logout');
    try {
      await api.logoutCopilot();
      setCopilotFlow(null);
      setCopilotAuth({ loading: false, authenticated: false, has_token: false });
      toast({ kind: 'ok', text: 'Copilot 已退出' });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setCopilotBusy('');
    }
  };

  useEffect(() => {
    const knownNames = new Set(models.map((m) => m?.name).filter(Boolean));
    setExpandedModelNames((prev) => {
      const next = new Set([...prev].filter((name) => knownNames.has(name)));
      return next.size === prev.size ? prev : next;
    });
    setEditDrafts((prev) => {
      let changed = false;
      const next = {};
      for (const [name, draft] of Object.entries(prev)) {
        if (knownNames.has(name)) {
          next[name] = draft;
        } else {
          changed = true;
        }
      }
      return changed ? next : prev;
    });
  }, [models]);

  const resetAddForm = () => {
    setShowAdd(false);
    setAddDraft(MODEL_NEW_DRAFT_DEFAULT);
  };

  const collapseModel = (name, { discardDraft = false } = {}) => {
    if (!name) return;
    setExpandedModelNames((prev) => {
      if (!prev.has(name)) return prev;
      const next = new Set(prev);
      next.delete(name);
      return next;
    });
    if (discardDraft) {
      setEditDrafts((prev) => {
        if (!Object.prototype.hasOwnProperty.call(prev, name)) return prev;
        const next = { ...prev };
        delete next[name];
        return next;
      });
    }
  };

  const expandModel = (m) => {
    if (!m?.name) return;
    setExpandedModelNames((prev) => {
      if (prev.has(m.name)) return prev;
      const next = new Set(prev);
      next.add(m.name);
      return next;
    });
    setEditDrafts((prev) => (
      prev[m.name] ? prev : { ...prev, [m.name]: draftFromModelProfile(m) }
    ));
  };

  const toggleModelExpansion = (m) => {
    if (!m?.name) return;
    if (expandedModelNames.has(m.name)) {
      collapseModel(m.name);
    } else {
      expandModel(m);
    }
  };

  const scrollAddModelTitleIntoView = useCallback(() => {
    window.requestAnimationFrame(() => {
      addModelTitleRef.current?.scrollIntoView({
        behavior: 'smooth',
        block: 'start',
        inline: 'nearest',
      });
    });
  }, []);

  useEffect(() => {
    if (!showAdd || !pendingAddScrollRef.current) return;
    pendingAddScrollRef.current = false;
    scrollAddModelTitleIntoView();
  }, [scrollAddModelTitleIntoView, showAdd]);

  const startAdd = () => {
    if (showAdd) {
      scrollAddModelTitleIntoView();
      return;
    }
    setAddDraft(MODEL_NEW_DRAFT_DEFAULT);
    pendingAddScrollRef.current = true;
    setShowAdd(true);
  };

  const saveDefault = async (name) => {
    if (!name || busy) return;
    setBusy(`default:${name}`);
    try {
      const saved = await api.setDefaultModel(name);
      setDefaultName(saved?.default_model_name || name);
      toast({ kind: 'ok', text: '默认模型已更新' });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setBusy('');
    }
  };

  const removeOne = async (name) => {
    if (!name || busy) return;
    if (!canDeleteSavedModel({ models, defaultName, name, busy: false })) {
      toast({ kind: 'err', text: '模型暂时不能删除' });
      return;
    }
    setBusy(`delete:${name}`);
    try {
      await api.removeModel(name);
      collapseModel(name, { discardDraft: true });
      await refresh();
      toast({ kind: 'ok', text: '模型已删除' });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setBusy('');
    }
  };

  const buildPayloads = (drafts) => {
    const payloads = [];
    for (const item of drafts) {
      const contextWindow = parseContextWindowK(item.context_window_k);
      if (!contextWindow.ok) {
        toast({ kind: 'err', text: lookupErrorMessage(contextWindow.code) });
        return null;
      }
      const requestHeaders = parseRequestHeadersJson(item.request_headers_json, item.provider);
      if (!requestHeaders.ok) {
        toast({ kind: 'err', text: lookupErrorMessage(requestHeaders.code) });
        return null;
      }
      const payload = payloadForModelDraft(item, {
        requestHeaders: requestHeaders.headers,
      });
      const valid = validateModelDraft(payload);
      if (!valid.ok) {
        toast({ kind: 'err', text: lookupErrorMessage(valid.code) });
        return null;
      }
      payloads.push(payload);
    }
    return payloads;
  };

  const submitAddDraft = async () => {
    if (busy) return;
    const drafts = buildModelDraftsFromSelection(addDraft);
    if (drafts.length === 0) {
      toast({ kind: 'err', text: lookupErrorMessage('MISSING_MODEL') });
      return;
    }
    const payloads = buildPayloads(drafts);
    if (!payloads) return;

    setBusy('submit:add');
    try {
      for (const payload of payloads) {
        await api.addModel(payload);
      }
      toast({ kind: 'ok', text: drafts.length > 1 ? `已新增 ${drafts.length} 个模型` : '模型已新增' });
      resetAddForm();
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setBusy('');
    }
  };

  const submitEditDraft = async (name, fallbackModel) => {
    if (!name || busy) return;
    const draft = editDrafts[name] || draftFromModelProfile(fallbackModel);
    const selected = splitModelIds(draft.model);
    if (selected.length !== 1) {
      toast({ kind: 'err', text: '编辑模式只能保存一个 Model ID' });
      return;
    }
    const payloads = buildPayloads([draft]);
    if (!payloads) return;

    setBusy(`submit:${name}`);
    try {
      await api.updateModel(name, payloads[0]);
      toast({ kind: 'ok', text: '模型已更新' });
      collapseModel(name, { discardDraft: true });
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err?.code, err?.message) });
    } finally {
      setBusy('');
    }
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-1">模型</h2>
      <p className="text-[13px] text-fg-mute leading-relaxed mb-6">
        管理已保存的模型预设。聊天界面顶栏切换的模型列表来自这里。
      </p>

      <CopilotAuthPanel
        auth={copilotAuth}
        flow={copilotFlow}
        busy={copilotBusy}
        onRefresh={refreshCopilotAuth}
        onStart={startCopilotLogin}
        onPoll={pollCopilotFlow}
        onLogout={logoutCopilot}
        onOpenExternalUrl={openExternalUrl}
        onCopyCode={(code) => copyCopilotUserCode(code)}
      />

      <div className="flex items-center justify-between mb-3">
        <div className="text-[12px] text-fg-mute">
          {loading ? '正在加载...' : (
            savedModelFilter.trim()
              ? `${models.length} 个已保存模型 · 匹配 ${filteredModels.length} 个`
              : `${models.length} 个已保存模型`
          )}
        </div>
        <div className="flex items-center gap-2 shrink-0">
          <button
            type="button"
            onClick={startAdd}
            className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border border-accent-soft bg-accent-bg text-[12px] font-medium text-accent hover:border-accent hover:bg-surface-hi transition"
          >
            <VsIcon name="add" size={13} />
            新增模型
          </button>
          <button
            type="button"
            onClick={refreshAndCollapseModels}
            disabled={loading || !!busy}
            className="px-3 py-1.5 rounded-md border border-border text-[12px] text-fg-2 hover:bg-surface-hi transition disabled:opacity-50 disabled:cursor-wait"
          >
            刷新
          </button>
        </div>
      </div>

      {models.length > 0 && (
        <div className="relative mb-3">
          <VsIcon name="search" size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-fg-mute" />
          <input
            type="text"
            value={savedModelFilter}
            onChange={(e) => setSavedModelFilter(e.target.value)}
            placeholder="按名称、模型 ID 或能力标签过滤,如 vision / websearch / 联网"
            className="w-full h-9 pl-9 pr-3 rounded-md border border-border bg-surface text-[12px] text-fg outline-none focus:border-accent transition placeholder:text-fg-mute"
          />
        </div>
      )}

      <div className="rounded-lg border border-border bg-surface overflow-hidden mb-3">
        {!loading && models.length === 0 && (
          <div className="px-5 py-8 text-center text-[13px] text-fg-mute">
            还没有保存的模型
          </div>
        )}
        {!loading && models.length > 0 && visibleModels.length === 0 && (
          <div className="px-5 py-8 text-center text-[13px] text-fg-mute">
            没有匹配的模型
          </div>
        )}
        {visibleModels.map((m, i) => {
          const isLast = i === visibleModels.length - 1;
          const isExpanded = expandedModelNames.has(m.name);
          const editDraft = editDrafts[m.name] || draftFromModelProfile(m);
          const isDefault = defaultName === m.name;
          const canDelete = canDeleteSavedModel({
            models,
            defaultName,
            name: m.name,
            busy: !!busy,
          });

          return (
            <div
              key={m.name}
              className={clsx(!isLast && 'border-b border-border')}
            >
              <div
                role="button"
                tabIndex={0}
                aria-expanded={isExpanded}
                onClick={() => toggleModelExpansion(m)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    toggleModelExpansion(m);
                  }
                }}
                className={clsx(
                  'group/model-row flex items-center gap-3.5 px-5 py-4 cursor-pointer transition hover:bg-surface-hi',
                  isExpanded && 'bg-surface-alt',
                )}
              >
                {isDefault ? (
                  <span
                    className="px-2 py-1 rounded-md text-[11px] font-medium shrink-0 text-warn bg-warn-bg border border-warn select-none"
                  >
                    当前默认
                  </span>
                ) : (
                  <button
                    type="button"
                    onClick={(e) => { e.stopPropagation(); saveDefault(m.name); }}
                    disabled={!!busy}
                    className="px-2 py-1 rounded-md text-[11px] font-medium shrink-0 transition text-fg-mute opacity-0 group-hover/model-row:opacity-100 hover:bg-surface-hi hover:text-fg"
                  >
                    设为默认
                  </button>
                )}

                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 mb-1">
                    <span className="text-[14px] font-semibold truncate">{m.name}</span>
                    <span
                      className={clsx(
                        'px-2 py-[2px] rounded text-[10px] font-medium uppercase tracking-wide',
                        MODEL_NEW_PROVIDER_PILL[m.provider] || 'text-fg-2 bg-surface-hi border border-border',
                      )}
                    >
                      {providerLabel(m.provider)}
                    </span>
                    <CapabilityBadges capabilities={m.capabilities} compact />
                  </div>
                  <div className="text-[12px] text-fg-mute truncate">
                    {m.model}
                    {m.context_window ? (
                      <span> · 上下文 {formatContextWindowK(m.context_window)}k</span>
                    ) : null}
                  </div>
                </div>

                <div className="flex gap-1 shrink-0" onClick={(e) => e.stopPropagation()}>
                  <button
                    type="button"
                    onClick={() => expandModel(m)}
                    title="编辑"
                    className="w-8 h-8 rounded-md flex items-center justify-center text-fg-mute hover:bg-surface-hi transition"
                  >
                    <VsIcon name="edit" size={14} />
                  </button>
                  <button
                    type="button"
                    onClick={() => removeOne(m.name)}
                    disabled={!canDelete}
                    title="删除"
                    className={clsx(
                      'w-8 h-8 rounded-md flex items-center justify-center transition',
                      canDelete
                        ? 'text-fg-mute hover:bg-danger-bg hover:text-danger'
                        : 'text-fg-mute opacity-30 cursor-not-allowed',
                    )}
                  >
                    <VsIcon name="delete" size={14} />
                  </button>
                  <button
                    type="button"
                    onClick={() => toggleModelExpansion(m)}
                    title={isExpanded ? '折叠' : '展开'}
                    aria-expanded={isExpanded}
                    className="w-8 h-8 rounded-md flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg transition"
                  >
                    <VsIcon name={isExpanded ? 'expandDown' : 'expandRight'} size={15} />
                  </button>
                </div>
              </div>
              {isExpanded && (
                <div className="px-5 pb-5 pt-1 bg-surface">
                  <ModelFormPreview
                    data={editDraft}
                    setData={(patch) => setEditDrafts((drafts) => ({
                      ...drafts,
                      [m.name]: { ...(drafts[m.name] || draftFromModelProfile(m)), ...patch },
                    }))}
                    editingName={m.name}
                    onCancel={() => collapseModel(m.name, { discardDraft: true })}
                    onSubmit={() => submitEditDraft(m.name, m)}
                    submitLabel="保存"
                    busy={!!busy}
                    allowMultiple={false}
                    onProbeModels={api.probeModels}
                    copilotAuthenticated={copilotAuth.authenticated}
                  />
                </div>
              )}
            </div>
          );
        })}
      </div>

      {showAdd ? (
        <div className="mt-4">
          <div ref={addModelTitleRef} className="text-[14px] font-semibold mb-3 scroll-mt-6">新增模型</div>
          <ModelFormPreview
            data={addDraft}
            setData={(patch) => setAddDraft((d) => ({ ...d, ...patch }))}
            onCancel={() => {
              setShowAdd(false);
              setAddDraft(MODEL_NEW_DRAFT_DEFAULT);
            }}
            onSubmit={submitAddDraft}
            submitLabel="新增"
            busy={!!busy}
            allowMultiple
            onProbeModels={api.probeModels}
            copilotAuthenticated={copilotAuth.authenticated}
          />
        </div>
      ) : (
        <button
          type="button"
          onClick={startAdd}
          className="w-full mt-3 py-3.5 rounded-lg border border-dashed border-border bg-transparent text-fg-mute text-[13px] font-medium inline-flex items-center justify-center gap-1.5 transition hover:border-accent hover:text-accent"
        >
          <span className="text-[16px] leading-none">+</span> 新增模型
        </button>
      )}
    </>
  );
}

function CopilotAuthPanel({
  auth,
  flow,
  busy = '',
  onRefresh,
  onStart,
  onPoll,
  onLogout,
  onOpenExternalUrl = openExternalUrl,
  onCopyCode = () => {},
}) {
  const pending = busy === 'start' || busy === 'poll' || busy === 'logout';
  const loggedIn = !!auth?.authenticated;
  const statusText = auth?.loading ? '检查中' : loggedIn ? '已登录' : '未登录';
  const statusClass = auth?.loading
    ? 'text-fg-mute bg-surface-hi border border-border'
    : loggedIn
      ? 'text-ok bg-ok-bg border border-ok-border'
      : 'text-warn bg-warn-bg border border-warn';

  return (
    <div className="rounded-lg border border-border bg-surface mb-6 overflow-hidden">
      <div className="px-5 py-3.5 flex items-center gap-3 border-b border-border">
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2 mb-1">
            <span className="text-[14px] font-semibold">Copilot</span>
            <span className={clsx('px-2 py-[2px] rounded text-[10px] font-medium', statusClass)}>
              {statusText}
            </span>
          </div>
          <div className="text-[12px] text-fg-mute">
            GitHub Copilot 认证用于获取 Copilot 模型列表和运行 Copilot saved model。
          </div>
        </div>
        <div className="flex items-center gap-2 shrink-0">
          <button
            type="button"
            onClick={onRefresh}
            disabled={pending || auth?.loading}
            className="w-8 h-8 rounded-md flex items-center justify-center text-fg-mute hover:bg-surface-hi transition disabled:opacity-40"
            title="刷新状态"
          >
            <RefreshIcon size={14} />
          </button>
          {loggedIn ? (
            <button
              type="button"
              onClick={onLogout}
              disabled={pending}
              className="px-3 py-1.5 rounded-md border border-border text-[12px] text-fg-2 hover:bg-surface-hi transition disabled:opacity-50"
            >
              退出
            </button>
          ) : (
            <button
              type="button"
              onClick={onStart}
              disabled={pending}
              className="px-3.5 py-1.5 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 transition disabled:opacity-50"
            >
              登录
            </button>
          )}
        </div>
      </div>

      {flow?.device_code && (
        <div className="px-5 py-4 bg-surface-alt">
          <div className="grid grid-cols-[minmax(0,1fr)_auto] gap-3 items-center">
            <div className="min-w-0">
              <div className="text-[12px] text-fg-mute mb-1">GitHub 验证码</div>
              <div className="flex items-center gap-2">
                <div className="text-[20px] font-semibold tracking-[0.12em] text-fg">
                  {flow.user_code}
                </div>
                <button
                  type="button"
                  onClick={() => onCopyCode(flow.user_code)}
                  disabled={!flow.user_code}
                  className="h-8 px-2.5 rounded-md border border-border bg-surface text-[12px] text-fg-2 inline-flex items-center gap-1.5 hover:bg-surface-hi transition disabled:opacity-40"
                  title="复制验证码"
                >
                  <VsIcon name="copy" size={13} />
                  复制
                </button>
              </div>
              <a
                href={flow.verification_uri}
                onClick={async (event) => {
                  event.preventDefault();
                  const opened = await onOpenExternalUrl(flow.verification_uri);
                  if (!opened.ok) {
                    toast({ kind: 'err', text: '无法打开系统浏览器:' + (opened.error || '') });
                  }
                }}
                className="mt-1 inline-block text-[12px] text-accent hover:underline break-all"
              >
                {flow.verification_uri}
              </a>
            </div>
            <button
              type="button"
              onClick={onPoll}
              disabled={pending || flow.status === 'expired' || flow.status === 'failed'}
              className="px-3 py-1.5 rounded-md border border-border text-[12px] text-fg-2 hover:bg-surface-hi transition disabled:opacity-40"
            >
              检查
            </button>
          </div>
          <div className="mt-3 text-[12px] text-fg-mute">
            {busy === 'poll' ? '正在检查...' : flow.message || '等待 GitHub 授权'}
          </div>
        </div>
      )}
    </div>
  );
}

function ModelFormPreview({
  data,
  setData,
  onCancel,
  onSubmit,
  submitLabel,
  busy = false,
  allowMultiple = true,
  editingName = '',
  onProbeModels,
  copilotAuthenticated = false,
}) {
  // 每个 form 实例独立维护 picker 状态 — 不同 baseUrl 的 fetch 结果不混淆。
  // fetchStatus 状态机:'idle' → 'fetching' → ('success' | 'failed')
  // - 点「添加模型」:idle/未尝试时自动 fetch;success/failed 状态都不再自动重试。
  // - 点「刷新」:无条件 fetch (用户显式意志覆盖 no-retry 规则)。
  const [pickerOpen, setPickerOpen] = useState(false);
  const [fetchStatus, setFetchStatus] = useState('idle');
  const [available, setAvailable] = useState(null);
  const [customInput, setCustomInput] = useState('');
  const [fetchError, setFetchError] = useState('');
  const [apiKeyVisible, setApiKeyVisible] = useState(false);
  const providerUsesHttpApi = data.provider === 'openai' || data.provider === 'anthropic';

  useEffect(() => {
    if (!providerUsesHttpApi) setApiKeyVisible(false);
  }, [providerUsesHttpApi]);

  useEffect(() => {
    if (data.provider === 'anthropic') {
      setFetchStatus('idle');
      setAvailable(null);
    }
  }, [data.provider]);

  const selectedModels = splitModelIds(data.model);
  const modelFilter = customInput.trim();
  const filteredAvailable = useMemo(
    () => filterModelIds(available || [], modelFilter),
    [available, modelFilter],
  );
  const canProbeModels = data.provider === 'copilot'
    ? copilotAuthenticated
    : data.provider === 'openai' && !!data.base_url;

  const doFetch = async () => {
    if (!onProbeModels || !canProbeModels) return;
    setFetchStatus('fetching');
    setFetchError('');
    try {
      const requestHeaders = providerUsesHttpApi
        ? parseRequestHeadersJson(data.request_headers_json, data.provider)
        : { ok: true, headers: undefined };
      if (!requestHeaders.ok) {
        setAvailable([]);
        setFetchError(lookupErrorMessage(requestHeaders.code));
        setFetchStatus('failed');
        return;
      }
      const result = await onProbeModels({
        provider: data.provider,
        base_url: data.provider === 'openai' ? data.base_url : '',
        api_key: data.provider === 'openai' ? (data.api_key || '') : '',
        request_headers: requestHeaders.headers,
      });
      const ids = Array.isArray(result?.models) ? result.models : [];
      setAvailable(ids);
      setFetchStatus('success');
    } catch (err) {
      setAvailable([]);
      setFetchError(lookupErrorMessage(err?.code, err?.message));
      setFetchStatus('failed');
    }
  };

  const togglePicker = () => {
    if (pickerOpen) {
      setPickerOpen(false);
      return;
    }
    setPickerOpen(true);
    // 仅在 idle (从未尝试) 时自动 fetch;上次 failed 不自动重试 — 走刷新按钮。
    if (fetchStatus === 'idle' && canProbeModels) {
      doFetch();
    }
  };

  const refresh = () => {
    if (!canProbeModels) return;
    doFetch();
  };

  const toggleModelPick = (mid) => {
    if (!allowMultiple) {
      setData({ model: selectedModels.includes(mid) ? '' : mid });
      return;
    }
    const cur = new Set(selectedModels);
    if (cur.has(mid)) cur.delete(mid); else cur.add(mid);
    setData({ model: [...cur].join(', ') });
  };

  const addCustomModel = () => {
    const trimmed = customInput.trim();
    if (!trimmed) return;
    if (!allowMultiple) {
      setData({ model: trimmed });
      setCustomInput('');
      return;
    }
    if (!selectedModels.includes(trimmed)) {
      setData({ model: [...selectedModels, trimmed].join(', ') });
    }
    setCustomInput('');
  };

  const onApiKeyChange = (e) => {
    setData({ api_key: e.target.value });
  };

  const fieldClass =
    'w-full px-3.5 py-2.5 rounded-md border border-border bg-surface text-fg text-[13px] outline-none focus:border-accent transition';

  const selectedCount = selectedModels.length;
  const canSubmit = selectedCount > 0 && (!editingName || selectedCount === 1);
  const selectedCapabilities = normalizeModelCapabilities(data.capabilities);
  const toggleCapability = (id) => {
    const cur = new Set(selectedCapabilities);
    if (cur.has(id)) cur.delete(id); else cur.add(id);
    const knownIds = new Set(MODEL_CAPABILITY_OPTIONS.map((item) => item.id));
    const unknown = selectedCapabilities.filter((tag) => !knownIds.has(tag));
    setData({ capabilities: [...unknown, ...MODEL_CAPABILITY_OPTIONS.map((item) => item.id).filter((tag) => cur.has(tag))] });
  };

  return (
    <div className="p-5 rounded-lg border border-accent-soft bg-surface">
      {/* Row 1: 名称 + Provider */}
      <div className="grid grid-cols-2 gap-4 mb-4">
        <div>
          <div className="text-[12px] font-medium text-fg-2 mb-1.5">名称</div>
          <input
            type="text"
            className={fieldClass}
            placeholder="例如: gpt-4o-fast"
            value={data.name}
            onChange={(e) => setData({ name: e.target.value })}
          />
        </div>
        <div>
          <div className="text-[12px] font-medium text-fg-2 mb-1.5">Provider</div>
          <div className="relative">
            <select
              className={clsx(fieldClass, 'cursor-pointer appearance-none pr-9')}
              value={data.provider}
              onChange={(e) => {
                const provider = e.target.value;
                setData({
                  provider,
                  base_url: baseUrlForProviderSwitch(provider, data.base_url),
                  api_key: (provider === 'openai' || provider === 'anthropic') ? data.api_key : '',
                  request_headers_json: (provider === 'openai' || provider === 'anthropic')
                    ? (data.request_headers_json || '')
                    : '',
                });
                setFetchStatus('idle');
                setAvailable(null);
              }}
            >
              <option value="openai">OpenAI</option>
              <option value="anthropic">Anthropic</option>
              <option value="copilot">Copilot</option>
            </select>
            <VsIcon
              name="expandDown"
              size={14}
              className="pointer-events-none absolute right-3 top-1/2 -translate-y-1/2 text-fg-mute"
            />
          </div>
        </div>
      </div>

      {/* Base URL */}
      {providerUsesHttpApi && (
        <div className="mb-4">
          <div className="text-[12px] font-medium text-fg-2 mb-1.5">Base URL</div>
          <input
            type="text"
            className={clsx(fieldClass, 'text-[12px]')}
            placeholder={data.provider === 'anthropic' ? ANTHROPIC_DEFAULT_BASE_URL : OPENAI_DEFAULT_BASE_URL}
            value={data.base_url}
            onChange={(e) => {
              setData({ base_url: e.target.value });
              setFetchStatus('idle');
              setAvailable(null);
            }}
          />
        </div>
      )}

      {/* API Key */}
      {providerUsesHttpApi && (
        <div className="mb-4">
          <div className="text-[12px] font-medium text-fg-2 mb-1.5">API Key</div>
          <div className="relative">
            <input
              type={apiKeyVisible ? 'text' : 'password'}
              className={clsx(fieldClass, 'pr-10 text-[12px]')}
              placeholder={data.provider === 'anthropic' ? 'sk-ant-...' : 'sk-...'}
              value={data.api_key || ''}
              onChange={onApiKeyChange}
              spellCheck={false}
              autoComplete="off"
            />
            <button
              type="button"
              aria-label={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'}
              aria-pressed={apiKeyVisible}
              title={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'}
              onClick={() => setApiKeyVisible((v) => !v)}
              className="absolute right-2 top-1/2 flex h-7 w-7 -translate-y-1/2 items-center justify-center rounded-md text-fg-mute transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-accent"
            >
              <span className="relative flex h-4 w-4 items-center justify-center">
                <VsIcon name="eye" size={16} />
                {!apiKeyVisible && (
                  <span
                    aria-hidden="true"
                    data-api-key-eye-slash="true"
                    className="pointer-events-none absolute left-[1px] right-[1px] top-1/2 h-[1.5px] -rotate-45 rounded-full bg-current"
                  />
                )}
              </span>
            </button>
          </div>
        </div>
      )}

      {/* 自定义请求头 */}
      {providerUsesHttpApi && (
        <div className="mb-4">
          <div className="text-[12px] font-medium text-fg-2 mb-1.5">自定义请求头(JSON,可选)</div>
          <textarea
            className={clsx(fieldClass, 'min-h-[86px] resize-y font-mono text-[12px] leading-relaxed')}
            placeholder={'{"X-Team":"acecode","X-Token":"{env:ACE_TOKEN}"}'}
            value={data.request_headers_json || ''}
            onChange={(e) => {
              setData({ request_headers_json: e.target.value });
              setFetchStatus('idle');
              setAvailable(null);
            }}
            spellCheck={false}
          />
        </div>
      )}

      {/* Context Window */}
      <div className="mb-4">
        <div className="text-[12px] font-medium text-fg-2 mb-1.5">上下文窗口(K,可选)</div>
        <input
          type="number"
          min="0"
          step="0.001"
          className={clsx(fieldClass, 'text-[12px]')}
          placeholder="例如: 128"
          value={data.context_window_k || ''}
          onChange={(e) => setData({ context_window_k: e.target.value })}
        />
      </div>

      <div className="mb-4">
        <div className="text-[12px] font-medium text-fg-2 mb-1.5">模型能力</div>
        <div className="flex flex-wrap gap-1.5">
          {MODEL_CAPABILITY_OPTIONS.map((item) => {
            const active = selectedCapabilities.includes(item.id);
            return (
              <button
                key={item.id}
                type="button"
                onClick={() => toggleCapability(item.id)}
                aria-pressed={active}
                className={clsx(
                  'inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-md border text-[12px] transition',
                  active
                    ? 'border-accent-soft bg-accent-bg text-accent'
                    : 'border-border bg-surface text-fg-mute hover:bg-surface-hi hover:text-fg',
                  item.id === 'tool_use' && active && 'border-warn bg-warn-bg text-warn',
                )}
              >
                <CapabilityIcon id={item.id} size={13} />
                <span>{item.label}</span>
              </button>
            );
          })}
        </div>
      </div>

      {/* Model ID 区域 */}
      <div className="mb-5">
        <div className="flex items-center justify-between mb-1.5">
          <div className="text-[12px] font-medium text-fg-2">Model ID</div>
          <button
            type="button"
            onClick={togglePicker}
            className={clsx(
              'inline-flex items-center gap-1.5 px-3.5 py-1 rounded-md border text-[12px] font-medium transition',
              pickerOpen
                ? 'border-accent text-accent bg-accent-bg'
                : 'border-border text-fg bg-surface hover:bg-surface-hi',
            )}
          >
            <VsIcon name="add" size={13} />
            {allowMultiple ? '添加模型' : '选择模型'}
          </button>
        </div>

        {pickerOpen && (
          <div className="rounded-md border border-border bg-surface-alt overflow-hidden mb-2.5">
            {/* 第一行:自定义输入 — 始终显示,即使 fetch 失败也能手动加 */}
            <div className="flex items-center gap-2 px-3.5 py-2 bg-surface border-b border-border">
              <VsIcon name="edit" size={14} className="shrink-0 text-fg-mute" />
              <input
                type="text"
                placeholder="过滤或自定义模型 ID,回车添加"
                className="flex-1 px-1 py-0.5 bg-transparent text-[12px] text-fg outline-none placeholder:text-fg-mute"
                value={customInput}
                onChange={(e) => setCustomInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') {
                    e.preventDefault();
                    addCustomModel();
                  }
                }}
              />
              <button
                type="button"
                onClick={addCustomModel}
                disabled={!customInput.trim()}
                className={clsx(
                  'shrink-0 inline-flex items-center gap-1 px-2.5 py-1 rounded text-[11px] font-medium transition',
                  customInput.trim()
                    ? 'text-accent bg-accent-bg hover:opacity-80'
                    : 'text-fg-mute bg-transparent opacity-50 cursor-not-allowed',
                )}
              >
                <VsIcon name="add" size={11} />
                添加
              </button>
            </div>

            {/* 已查询模型 section header (含状态 + 刷新按钮) */}
            <div className="flex items-center justify-between px-3.5 py-1.5 bg-surface-alt border-b border-border">
              <span className="text-[11px] text-fg-mute">
                {data.provider === 'copilot' && fetchStatus === 'idle' && (copilotAuthenticated ? '尚未获取 Copilot 模型列表' : '请先登录 Copilot,也可手动输入')}
                {data.provider === 'openai' && fetchStatus === 'idle' && (data.base_url ? '尚未获取模型列表' : '请先填写 Base URL')}
                {data.provider === 'anthropic' && fetchStatus === 'idle' && 'Anthropic 暂不查询模型列表,请手动输入'}
                {fetchStatus === 'fetching' && '正在获取...'}
                {fetchStatus === 'success' && (
                  modelFilter && available?.length
                    ? `已查询到 ${available.length} 个模型 · 匹配 ${filteredAvailable.length} 个`
                    : `已查询到 ${available?.length || 0} 个模型`
                )}
                {fetchStatus === 'failed'   && '上次获取失败 · 点「刷新」重试'}
              </span>
              <button
                type="button"
                onClick={refresh}
                disabled={!canProbeModels || fetchStatus === 'fetching'}
                title={canProbeModels ? '刷新模型列表' : (
                  data.provider === 'copilot'
                    ? '请先登录 Copilot'
                    : (data.provider === 'anthropic' ? 'Anthropic 暂不支持模型列表查询' : '请先填写 Base URL')
                )}
                className="inline-flex items-center gap-1 px-2 py-0.5 text-[11px] text-fg-2 rounded hover:bg-surface-hi transition disabled:opacity-40 disabled:cursor-not-allowed"
              >
                <RefreshIcon size={11} className={clsx(fetchStatus === 'fetching' && 'animate-spin')} />
                刷新
              </button>
            </div>

            {/* fetch 结果 body */}
            {fetchStatus === 'fetching' && (
              <div className="px-3.5 py-4 text-center text-[12px] text-fg-mute">
                <span
                  className="inline-block w-3 h-3 mr-2 rounded-full border-[1.5px] border-fg-mute border-t-transparent align-middle"
                  style={{ animation: 'ace-spin 0.8s linear infinite' }}
                  aria-hidden="true"
                />
                获取中...
              </div>
            )}
            {fetchStatus === 'failed' && (
              <div className="px-3.5 py-3 text-[12px]">
                <div className="text-danger mb-1">未能获取模型列表</div>
                <div className="text-[11px] text-fg-mute">
                  {fetchError || '请检查 Base URL 与 API Key 是否正确,然后点击右上角「刷新」重试。'}
                </div>
              </div>
            )}
            {fetchStatus === 'success' && available && available.length > 0 && (
              filteredAvailable.length > 0 ? (
                <div className="max-h-[200px] overflow-y-auto">
                  {filteredAvailable.map((mid, idx) => {
                    const checked = selectedModels.includes(mid);
                    const isLast = idx === filteredAvailable.length - 1;
                    return (
                      <button
                        key={mid}
                        type="button"
                        onClick={() => toggleModelPick(mid)}
                        className={clsx(
                          'w-full flex items-center gap-2.5 px-3.5 py-2 text-left transition',
                          !isLast && 'border-b border-border',
                          checked ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                        )}
                      >
                        <span
                          className={clsx(
                            'w-[18px] h-[18px] rounded flex items-center justify-center text-white text-[11px] font-bold leading-none transition shrink-0',
                            checked
                              ? 'bg-accent border-2 border-accent'
                              : 'border-2 border-border bg-transparent',
                          )}
                        >
                          {checked && <span>✓</span>}
                        </span>
                        <span className="text-[12px] text-fg">{mid}</span>
                      </button>
                    );
                  })}
                </div>
              ) : (
                <div className="px-3.5 py-3 text-[12px] text-fg-mute text-center">
                  没有匹配的模型 · 可回车作为自定义模型添加
                </div>
              )
            )}
            {fetchStatus === 'success' && available && available.length === 0 && (
              <div className="px-3.5 py-3 text-[12px] text-fg-mute text-center">
                远端未返回任何模型 · 你仍可通过上方输入框自定义
              </div>
            )}
          </div>
        )}

        {/* 已选模型 tag 区 — 多选 / 自定义结果统一渲染 */}
        {selectedModels.length > 0 ? (
          <div className="flex flex-wrap gap-1.5">
            {selectedModels.map((mid) => (
              <span
                key={mid}
                className="inline-flex items-center gap-1 pl-2.5 pr-1.5 py-1 rounded-md bg-accent-bg border border-accent-soft text-[12px] text-accent"
              >
                {mid}
                <button
                  type="button"
                  onClick={() => toggleModelPick(mid)}
                  aria-label={`移除 ${mid}`}
                  className="w-4 h-4 inline-flex items-center justify-center text-accent opacity-60 hover:opacity-100 transition text-[14px] leading-none"
                >×</button>
              </span>
            ))}
          </div>
        ) : !pickerOpen && (
          <div className="px-3.5 py-3 rounded-md border border-dashed border-border bg-transparent text-[12px] text-fg-mute text-center">
            尚未选择任何模型 · 点击右上角「添加模型」开始
          </div>
        )}
      </div>

      <div className="flex gap-2.5 justify-end">
        <button
          type="button"
          onClick={onCancel}
          disabled={busy}
          className="px-5 py-2 rounded-md border border-border bg-transparent text-fg-2 text-[13px] font-medium hover:bg-surface-hi transition"
        >
          取消
        </button>
        <button
          type="button"
          onClick={onSubmit}
          disabled={!canSubmit || busy}
          className={clsx(
            'px-6 py-2 rounded-md text-[13px] font-medium text-white transition',
            canSubmit && !busy ? 'bg-accent hover:opacity-90' : 'bg-accent opacity-50 cursor-not-allowed',
          )}
        >
          {submitLabel}
        </button>
      </div>
    </div>
  );
}
