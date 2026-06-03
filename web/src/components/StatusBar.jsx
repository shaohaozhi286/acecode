// 底部 22px 状态栏:权限模式下拉 + 模型下拉 + 轮次 + 分支(占位)

import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { buildStatusBarModelMenu } from '../lib/sessionModel.js';
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
    if (color === 'plan') return 'bg-accent';
    return 'bg-danger';
  };
  const dotCls = modeDotClass(cur.color);
  const modelBusy = modelSwitching || modelRefreshing;
  const modelMenu = useMemo(() => buildStatusBarModelMenu({
    modelOptions,
    selectedModelName,
    fallbackLabel: model,
  }), [model, modelOptions, selectedModelName]);
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
        className="h-[18px] px-1.5 py-0 rounded bg-surface-hi border border-transparent text-[10px] text-fg-mute outline-none hover:text-fg focus:border-accent disabled:opacity-60 disabled:cursor-wait inline-flex items-center gap-1"
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
                item.active ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi',
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
    <span className="px-1.5 py-px rounded bg-surface-hi text-[10px] max-w-[220px] truncate" title={model}>{model}</span>
  );
  const goalLabel = goal
    ? `${goal.status || 'goal'}${goal.token_budget ? ` ${goal.tokens_used || 0}/${goal.token_budget}` : ''}`
    : '';

  return (
    <div className="h-[22px] flex items-center px-2.5 gap-4 bg-surface-alt border-t border-border text-[11px] text-fg-mute shrink-0">
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
            <RefreshIcon size={12} className={modelRefreshing ? 'animate-spin' : ''} />
          </button>
        )}
      </div>
      <span className="flex items-center gap-1.5 shrink-0">
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
