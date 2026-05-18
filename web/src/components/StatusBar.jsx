// 底部 22px 状态栏:权限模式下拉 + 模型 tag + 轮次 + 分支(占位)

import { useEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { optionLabel } from '../lib/sessionModel.js';
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
  tokenBudget = null,
  goal = null,
  permissionMode = 'default',
  permissionSwitching = false,
  onPermissionModeChange,
}) {
  const [localMode, setLocalMode] = useState(normalizePermissionMode(permissionMode));
  const [open, setOpen] = useState(false);
  const ref = useRef(null);

  useEffect(() => {
    setLocalMode(normalizePermissionMode(permissionMode));
  }, [permissionMode]);

  useEffect(() => {
    if (!open) return;
    const onDoc = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(false); };
    document.addEventListener('mousedown', onDoc);
    return () => document.removeEventListener('mousedown', onDoc);
  }, [open]);

  const mode = normalizePermissionMode(onPermissionModeChange ? permissionMode : localMode);
  const cur = permissionModeOption(mode);
  const dotCls = cur.color === 'ok' ? 'bg-ok' : cur.color === 'warn' ? 'bg-warn' : 'bg-danger';
  const modelBusy = modelSwitching || modelRefreshing;
  const selectPermissionMode = (nextMode) => {
    const normalized = normalizePermissionMode(nextMode);
    if (onPermissionModeChange) onPermissionModeChange(normalized);
    else setLocalMode(normalized);
    setOpen(false);
  };
  const modelControl = onModelChange && modelOptions.length > 0 ? (
    <select
      value={selectedModelName || ''}
      disabled={modelBusy}
      onChange={(e) => onModelChange(e.target.value)}
      title={model}
      className="h-[18px] max-w-[220px] px-1.5 py-0 rounded bg-surface-hi border border-transparent text-[10px] text-fg-mute outline-none hover:text-fg focus:border-accent disabled:opacity-60"
    >
      {!selectedModelName && <option value="">{model}</option>}
      {modelOptions.map((option) => (
        <option key={option.name} value={option.name}>
          {optionLabel(option)}
        </option>
      ))}
    </select>
  ) : (
    <span className="px-1.5 py-px rounded bg-surface-hi text-[10px] max-w-[220px] truncate" title={model}>{model}</span>
  );
  const goalLabel = goal
    ? `${goal.status || 'goal'}${goal.token_budget ? ` ${goal.tokens_used || 0}/${goal.token_budget}` : ''}`
    : '';

  return (
    <div className="h-[22px] flex items-center px-2.5 gap-4 bg-surface-alt border-t border-border text-[11px] text-fg-mute shrink-0">
      <div className="flex items-center gap-1">
        <div ref={ref} className="relative">
          <button
            type="button"
            onClick={() => setOpen((o) => !o)}
            title={cur.hint}
            disabled={permissionSwitching}
            className="flex items-center gap-1 px-1.5 py-px rounded hover:bg-surface-hi text-fg-mute transition"
          >
            <span className={clsx('w-1.5 h-1.5 rounded-full', dotCls)} />
            <span>{cur.label}</span>
            <VsIcon name="glyphDown" size={9} className="opacity-70" />
          </button>
          {open && (
            <div className="absolute bottom-full left-0 mb-1 bg-surface border border-border rounded-md ace-shadow-lg p-1 min-w-[220px] z-50">
              {PERMISSION_MODES.map((m) => {
                const active = m.id === mode;
                const c = m.color === 'ok' ? 'bg-ok' : m.color === 'warn' ? 'bg-warn' : 'bg-danger';
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
            <RefreshIcon size={12} className={modelRefreshing ? 'animate-spin' : ''} />
          </button>
        )}
      </div>
      <span className="flex items-center gap-1.5 min-w-0">
        {modelControl}
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
