// 底部 22px 状态栏:权限模式下拉 + 模型下拉 + 轮次 + 分支(占位)

import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { buildStatusBarModelMenu } from '../lib/sessionModel.js';
import { loadTier, loadTierTextClass } from '../lib/modelLoad.js';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { TokenBudgetRing } from './TokenBudgetRing.jsx';

export function StatusBar({
  model = '—',
  turns = 0,
  branch = '',
  modelOptions = [],
  selectedModelName = '',
  modelSwitching = false,
  modelRefreshing = false,
  onModelChange,
  onRefreshModels,
  modelLoad = null,
  tokenBudget = null,
  goal = null,
  permissionMode = 'default',
  permissionSwitching = false,
  onPermissionModeChange,
}) {
  const [localMode, setLocalMode] = useState(normalizePermissionMode(permissionMode));
  const [permissionOpen, setPermissionOpen] = useState(false);
  const [modelOpen, setModelOpen] = useState(false);
  const permissionRef = useRef(null);
  const modelRef = useRef(null);

  useEffect(() => {
    setLocalMode(normalizePermissionMode(permissionMode));
  }, [permissionMode]);

  useEffect(() => {
    if (!permissionOpen && !modelOpen) return;
    const onDoc = (e) => {
      if (permissionRef.current && !permissionRef.current.contains(e.target)) setPermissionOpen(false);
      if (modelRef.current && !modelRef.current.contains(e.target)) setModelOpen(false);
    };
    const onKey = (e) => {
      if (e.key !== 'Escape') return;
      setPermissionOpen(false);
      setModelOpen(false);
    };
    document.addEventListener('mousedown', onDoc);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDoc);
      document.removeEventListener('keydown', onKey);
    };
  }, [permissionOpen, modelOpen]);

  const mode = normalizePermissionMode(onPermissionModeChange ? permissionMode : localMode);
  const cur = permissionModeOption(mode);
  const modeDotClass = (color) => {
    if (color === 'ok') return 'bg-ok';
    if (color === 'warn') return 'bg-warn';
    return 'bg-danger';
  };
  const dotCls = modeDotClass(cur.color);
  const modelBusy = modelSwitching || modelRefreshing;
  const modelMenu = useMemo(() => buildStatusBarModelMenu({
    modelOptions,
    selectedModelName,
    fallbackLabel: model,
  }), [model, modelOptions, selectedModelName]);
  const modelDeleted = modelMenu.displayDeleted || String(model || '').includes('(deleted)');
  const selectPermissionMode = (nextMode) => {
    const normalized = normalizePermissionMode(nextMode);
    if (onPermissionModeChange) onPermissionModeChange(normalized);
    else setLocalMode(normalized);
    setPermissionOpen(false);
  };
  const selectModel = (name) => {
    const nextName = String(name || '');
    setModelOpen(false);
    if (!nextName || nextName === selectedModelName || modelBusy) return;
    onModelChange?.(nextName);
  };
  const modelControl = onModelChange && modelOptions.length > 0 ? (
    <div ref={modelRef} className="relative min-w-0">
      <button
        type="button"
        disabled={modelBusy}
        onClick={() => {
          if (modelBusy) return;
          setPermissionOpen(false);
          setModelOpen((open) => !open);
        }}
        title={modelMenu.displayLabel}
        aria-haspopup="listbox"
        aria-expanded={modelOpen}
        className={clsx(
          'h-[18px] px-1.5 py-0 rounded bg-surface-hi border border-transparent text-[10px] outline-none focus:border-accent disabled:opacity-60 disabled:cursor-wait inline-flex items-center gap-1',
          modelDeleted ? 'text-danger hover:text-danger' : 'text-fg-mute hover:text-fg',
        )}
      >
        <span className="shrink-0 whitespace-nowrap">{modelMenu.displayLabel}</span>
        <VsIcon name="glyphDown" size={9} className="opacity-70 shrink-0" />
      </button>
      {modelOpen && !modelBusy && (
        <div
          role="listbox"
          aria-label="选择模型"
          className="absolute bottom-full left-0 mb-1 min-w-full w-max max-h-[280px] overflow-auto bg-surface border border-border rounded-md ace-shadow-lg p-1 z-50"
        >
          {modelMenu.items.map((item) => (
            <button
              key={item.name}
              type="button"
              role="option"
              aria-selected={item.active}
              onClick={() => selectModel(item.name)}
              className={clsx(
                'w-full text-left px-2.5 py-1.5 rounded transition flex items-center gap-2',
                item.deleted
                  ? (item.active ? 'bg-danger-bg text-danger' : 'text-danger hover:bg-danger-bg')
                  : (item.active ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi'),
              )}
              title={item.label}
            >
              <span className="w-3 shrink-0 text-center">
                {item.active && <VsIcon name="ok" size={11} mono={false} />}
              </span>
              <span className="shrink-0 whitespace-nowrap text-[12px]">{item.label}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  ) : (
    <span
      className={clsx(
        'px-1.5 py-px rounded bg-surface-hi text-[10px] max-w-[220px] truncate',
        modelDeleted && 'text-danger',
      )}
      title={model}
    >
      {model}
    </span>
  );
  const goalLabel = goal
    ? `${goal.status || 'goal'}${goal.token_budget ? ` ${goal.tokens_used || 0}/${goal.token_budget}` : ''}`
    : '';

  return (
    <div data-tour-target="statusbar" className="h-[22px] flex items-center px-2.5 gap-4 bg-surface-alt border-t border-border text-[11px] text-fg-mute shrink-0">
      <div className="flex items-center gap-1">
        <div ref={permissionRef} className="relative">
          <button
            type="button"
            onClick={() => {
              setModelOpen(false);
              setPermissionOpen((o) => !o);
            }}
            title={cur.hint}
            disabled={permissionSwitching}
            className="flex items-center gap-1 px-1.5 py-px rounded hover:bg-surface-hi text-fg-mute transition"
          >
            <span className={clsx('w-1.5 h-1.5 rounded-full', dotCls)} />
            <span>{cur.label}</span>
            <VsIcon name="glyphDown" size={9} className="opacity-70" />
          </button>
          {permissionOpen && (
            <div className="absolute bottom-full left-0 mb-1 bg-surface border border-border rounded-md ace-shadow-lg p-1 min-w-[220px] z-50">
              {PERMISSION_MODES.map((m) => {
                const active = m.id === mode;
                const c = modeDotClass(m.color);
                return (
                  <button
                    key={m.id}
                    type="button"
                    disabled={permissionSwitching}
                    onClick={() => selectPermissionMode(m.id)}
                    className={clsx(
                      'w-full text-left px-2.5 py-1.5 rounded transition flex flex-col gap-0.5',
                      active ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                      permissionSwitching && 'opacity-60 cursor-wait',
                    )}
                  >
                    <span className="flex items-center gap-1.5">
                      <span className={clsx('w-1.5 h-1.5 rounded-full', c)} />
                      <span className={clsx('text-xs', active ? 'text-accent font-semibold' : 'text-fg')}>{m.label}</span>
                    </span>
                    <span className="text-[10px] text-fg-mute pl-3">{m.hint}</span>
                  </button>
                );
              })}
            </div>
          )}
        </div>
        {onRefreshModels && (
          <button
            type="button"
            onClick={onRefreshModels}
            disabled={modelBusy}
            title="刷新模型列表"
            aria-label="刷新模型列表"
            className="h-[18px] w-[18px] inline-flex items-center justify-center rounded hover:bg-surface-hi text-fg-mute hover:text-fg transition disabled:opacity-50 disabled:cursor-wait"
          >
            <RefreshIcon size={16} className={modelRefreshing ? 'animate-spin' : ''} />
          </button>
        )}
      </div>
      <span className="flex items-center gap-1.5 shrink-0">
        {modelControl}
        <ModelLoadChip load={modelLoad} />
        {tokenBudget && <TokenBudgetRing budget={tokenBudget} />}
      </span>
      <span>{turns} 轮次</span>
      {goal && (
        <span
          className="min-w-0 max-w-[28%] truncate px-1.5 py-px rounded bg-surface-hi text-[10px]"
          title={goal.objective || goalLabel}
        >
          Goal: {goalLabel}
        </span>
      )}
      {branch && <span className="ml-auto truncate max-w-[40%]">{branch}</span>}
      {!branch && <span className="ml-auto" />}
    </div>
  );
}

// 递增信号格图标(4 根),整体按当前负载档染色(currentColor 继承父级文本色)。
function SignalBars() {
  return (
    <svg width="12" height="11" viewBox="0 0 12 11" aria-hidden="true" className="shrink-0">
      <rect x="0" y="8" width="2.2" height="3" rx="0.5" fill="currentColor" />
      <rect x="3.2" y="5.5" width="2.2" height="5.5" rx="0.5" fill="currentColor" />
      <rect x="6.4" y="3" width="2.2" height="8" rx="0.5" fill="currentColor" />
      <rect x="9.6" y="0.5" width="2.2" height="10.5" rx="0.5" fill="currentColor" />
    </svg>
  );
}

// 模型池负载指示:仅当当前模型是 PUB 池且有负载数据时显示。颜色:<70 绿 / 70..90 黄
// / >90 红(见 lib/modelLoad.js)。tooltip 附有效上下文窗口(0.8×maxWindowTokens)。
function ModelLoadChip({ load }) {
  if (!load) return null;
  const tier = loadTier(load.usageRate);
  if (!tier) return null;
  const pct = Math.round(load.usageRate);
  const effK = load.effectiveContextWindow
    ? `,有效上下文 ${Math.round(load.effectiveContextWindow / 1000)}k`
    : '';
  return (
    <span
      className={clsx('inline-flex items-center gap-1 shrink-0', loadTierTextClass(tier))}
      title={`模型池负载 ${pct}%${effK}`}
    >
      <SignalBars />
      <span className="text-[10px] font-medium tabular-nums">{pct}%</span>
    </span>
  );
}
