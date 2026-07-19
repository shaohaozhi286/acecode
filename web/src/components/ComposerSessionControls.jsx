import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { loadTier, loadTierTextClass } from '../lib/modelLoad.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { buildStatusBarModelMenu } from '../lib/sessionModel.js';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { TokenBudgetRing } from './TokenBudgetRing.jsx';

function permissionTextClass(color) {
  if (color === 'ok') return 'text-ok';
  if (color === 'warn') return 'text-warn';
  return 'text-danger';
}

function PermissionShieldIcon({ className = '' }) {
  return (
    <svg
      width="16"
      height="16"
      viewBox="0 0 16 16"
      fill="none"
      aria-hidden="true"
      className={className}
    >
      <path
        d="M8 1.5 13 3.4v3.8c0 3.1-1.85 5.8-5 7.3-3.15-1.5-5-4.2-5-7.3V3.4L8 1.5Z"
        stroke="currentColor"
        strokeWidth="1.35"
        strokeLinejoin="round"
      />
      <path d="M8 4.5v4" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" />
      <circle cx="8" cy="11.1" r=".75" fill="currentColor" />
    </svg>
  );
}

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

function ModelLoadIndicator({ load }) {
  if (!load) return null;
  const tier = loadTier(load.usageRate);
  if (!tier) return null;
  const percent = Math.round(load.usageRate);
  const effectiveWindow = load.effectiveContextWindow
    ? `，有效上下文 ${Math.round(load.effectiveContextWindow / 1000)}k`
    : '';
  return (
    <span
      data-composer-control="model-load"
      className={clsx('ace-composer-model-load', loadTierTextClass(tier))}
      title={`模型池负载 ${percent}%${effectiveWindow}`}
    >
      <SignalBars />
      <span className="tabular-nums">{percent}%</span>
    </span>
  );
}

export function ComposerSessionControls({
  addControl,
  contexts,
  actions,
  className = '',
  model = '—',
  modelOptions = [],
  selectedModelName = '',
  modelSwitching = false,
  modelRefreshing = false,
  onModelChange,
  onRefreshModels,
  modelLoad = null,
  tokenBudget = null,
  permissionMode = 'default',
  permissionSwitching = false,
  onPermissionModeChange,
}) {
  const [localMode, setLocalMode] = useState(normalizePermissionMode(permissionMode));
  const [openMenu, setOpenMenu] = useState('');
  const rootRef = useRef(null);

  useEffect(() => {
    setLocalMode(normalizePermissionMode(permissionMode));
  }, [permissionMode]);

  useEffect(() => {
    if (!openMenu) return undefined;
    const onPointerDown = (event) => {
      if (!rootRef.current?.contains(event.target)) setOpenMenu('');
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') setOpenMenu('');
    };
    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [openMenu]);

  const mode = normalizePermissionMode(onPermissionModeChange ? permissionMode : localMode);
  const permission = permissionModeOption(mode);
  const modelMenu = useMemo(() => buildStatusBarModelMenu({
    modelOptions,
    selectedModelName,
    fallbackLabel: model,
  }), [model, modelOptions, selectedModelName]);
  const modelDeleted = modelMenu.displayDeleted || String(model || '').includes('(deleted)');
  const compactModelLabel = modelDeleted && selectedModelName
    ? `${selectedModelName} (deleted)`
    : (selectedModelName || modelMenu.displayLabel);
  const canOpenModelMenu = modelOptions.length > 0 || !!onRefreshModels;
  const modelBusy = modelSwitching || modelRefreshing;

  const selectPermission = (nextMode) => {
    const normalized = normalizePermissionMode(nextMode);
    if (onPermissionModeChange) onPermissionModeChange(normalized);
    else setLocalMode(normalized);
    setOpenMenu('');
  };

  const selectModel = (name) => {
    const nextName = String(name || '');
    if (!nextName || nextName === selectedModelName || modelBusy) return;
    setOpenMenu('');
    onModelChange?.(nextName);
  };

  return (
    <div
      ref={rootRef}
      data-composer-session-controls="true"
      className={clsx('ace-composer-session-footer', className)}
    >
      <div className="ace-composer-session-left">
        <div
          data-composer-control="add-context"
          className="shrink-0"
          onPointerDown={() => setOpenMenu('')}
        >
          {addControl}
        </div>

        <div
          data-composer-control="permission"
          className="ace-composer-permission-control relative min-w-0"
        >
          <button
            type="button"
            disabled={permissionSwitching}
            onClick={() => setOpenMenu((current) => (current === 'permission' ? '' : 'permission'))}
            title={permission.hint}
            aria-haspopup="menu"
            aria-expanded={openMenu === 'permission'}
            className={clsx(
              'ace-composer-control-button ace-composer-permission-button',
              permissionTextClass(permission.color),
              permissionSwitching && 'cursor-wait opacity-60',
            )}
          >
            <PermissionShieldIcon className="shrink-0" />
            <span className="ace-composer-permission-label">{permission.label}</span>
            <VsIcon name="glyphDown" size={10} className="shrink-0 opacity-75" />
          </button>

          {openMenu === 'permission' && (
            <div
              role="menu"
              aria-label="选择权限模式"
              className="ace-composer-popup ace-composer-permission-menu"
            >
              {PERMISSION_MODES.map((item) => {
                const active = item.id === mode;
                return (
                  <button
                    key={item.id}
                    type="button"
                    role="menuitemradio"
                    aria-checked={active}
                    disabled={permissionSwitching}
                    onClick={() => selectPermission(item.id)}
                    className={clsx(
                      'ace-composer-permission-option',
                      active ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                    )}
                  >
                    <span className="flex items-center gap-1.5">
                      <PermissionShieldIcon
                        className={clsx('shrink-0', permissionTextClass(item.color))}
                      />
                      <span className={clsx(
                        'text-[12px]',
                        active ? 'font-semibold text-accent' : 'text-fg',
                      )}
                      >
                        {item.label}
                      </span>
                    </span>
                    <span className="pl-[22px] text-[10px] text-fg-mute">{item.hint}</span>
                  </button>
                );
              })}
            </div>
          )}
        </div>

        <div
          data-composer-control="selected-contexts"
          className="ace-composer-context-strip"
          tabIndex={contexts ? 0 : undefined}
          aria-label={contexts ? '已选上下文' : undefined}
        >
          {contexts}
        </div>
      </div>

      <div className="ace-composer-session-right">
        <ModelLoadIndicator load={modelLoad} />
        {tokenBudget && (
          <span data-composer-control="token-budget" className="inline-flex shrink-0">
            <TokenBudgetRing budget={tokenBudget} className="ace-composer-token-budget" />
          </span>
        )}

        <div
          data-composer-control="model"
          className="ace-composer-model-control relative min-w-0"
        >
          <button
            type="button"
            disabled={modelSwitching || !canOpenModelMenu}
            onClick={() => {
              if (!canOpenModelMenu || modelSwitching) return;
              setOpenMenu((current) => (current === 'model' ? '' : 'model'));
            }}
            title={modelMenu.displayLabel}
            aria-haspopup="listbox"
            aria-expanded={openMenu === 'model'}
            className={clsx(
              'ace-composer-control-button ace-composer-model-button',
              modelDeleted ? 'text-danger' : 'text-fg',
              (modelSwitching || !canOpenModelMenu) && 'cursor-wait opacity-60',
            )}
          >
            <span className="ace-composer-model-label">{compactModelLabel}</span>
            <VsIcon name="glyphDown" size={10} className="shrink-0 opacity-75" />
          </button>

          {openMenu === 'model' && (
            <div className="ace-composer-popup ace-composer-model-menu">
              <div className="ace-composer-model-menu-header">
                <span>选择模型</span>
                {onRefreshModels && (
                  <button
                    type="button"
                    disabled={modelRefreshing}
                    onClick={(event) => {
                      event.stopPropagation();
                      onRefreshModels();
                    }}
                    title="刷新模型列表"
                    aria-label="刷新模型列表"
                    className="ace-composer-model-refresh"
                  >
                    <RefreshIcon size={16} className={modelRefreshing ? 'animate-spin' : ''} />
                  </button>
                )}
              </div>
              <div role="listbox" aria-label="选择模型" className="ace-composer-model-options">
                {modelMenu.items.length > 0 ? modelMenu.items.map((item) => (
                  <button
                    key={item.name}
                    type="button"
                    role="option"
                    aria-selected={item.active}
                    disabled={modelBusy}
                    onClick={() => selectModel(item.name)}
                    className={clsx(
                      'ace-composer-model-option',
                      item.deleted
                        ? (item.active ? 'bg-danger-bg text-danger' : 'text-danger hover:bg-danger-bg')
                        : (item.active ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi'),
                      modelBusy && 'cursor-wait opacity-60',
                    )}
                    title={item.label}
                  >
                    <span className="w-3 shrink-0 text-center">
                      {item.active && <VsIcon name="ok" size={11} mono={false} />}
                    </span>
                    <span className="truncate text-[12px]">{item.label}</span>
                  </button>
                )) : (
                  <div className="px-3 py-4 text-center text-[11px] text-fg-mute">
                    暂无可用模型
                  </div>
                )}
              </div>
            </div>
          )}
        </div>

        <div data-composer-control="submit" className="flex shrink-0 items-center gap-1">
          {actions}
        </div>
      </div>
    </div>
  );
}
