import { useCallback, useEffect, useState } from 'react';
import { SlideOver } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import {
  applyDesktopAcrylicCssVariables,
  applyNativeDesktopAcrylicSettings,
  defaultDesktopAcrylicSettingsForTheme,
  loadInitialDesktopAcrylicSettings,
  normalizeDesktopAcrylicSettings,
  saveDesktopAcrylicSettings,
} from '../lib/desktopAcrylicSettings.js';

function percent(value) {
  return Math.round(Number(value || 0) * 100);
}

function SliderRow({ label, value, onChange }) {
  const pct = percent(value);
  const commitPercent = (raw) => {
    const number = Number(raw);
    if (!Number.isFinite(number)) return;
    onChange(number / 100);
  };
  return (
    <label className="ace-acrylic-settings-row">
      <span className="ace-acrylic-settings-label">{label}</span>
      <span className="ace-acrylic-settings-control">
        <input
          className="ace-acrylic-settings-range"
          type="range"
          min="0"
          max="100"
          step="1"
          value={pct}
          onChange={(event) => commitPercent(event.target.value)}
        />
        <input
          className="ace-acrylic-settings-number"
          type="number"
          min="0"
          max="100"
          step="1"
          value={pct}
          onChange={(event) => commitPercent(event.target.value)}
        />
        <span className="ace-acrylic-settings-percent">%</span>
      </span>
    </label>
  );
}

export function AcrylicSettingsPanel({ onClose, theme = 'light' }) {
  const [settings, setSettings] = useState(() => defaultDesktopAcrylicSettingsForTheme(theme));

  const commit = useCallback((nextSettings) => {
    const normalized = saveDesktopAcrylicSettings(nextSettings, undefined, theme);
    setSettings(normalized);
    applyDesktopAcrylicCssVariables(document.documentElement, normalized);
    void applyNativeDesktopAcrylicSettings(normalized);
  }, [theme]);

  useEffect(() => {
    let cancelled = false;
    loadInitialDesktopAcrylicSettings(window, window.localStorage, theme)
      .then((initial) => {
        if (cancelled) return;
        const normalized = normalizeDesktopAcrylicSettings(
          initial,
          defaultDesktopAcrylicSettingsForTheme(theme),
        );
        setSettings(normalized);
        applyDesktopAcrylicCssVariables(document.documentElement, normalized);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [theme]);

  const update = useCallback((patch) => {
    commit({ ...settings, ...patch });
  }, [commit, settings]);

  const reset = useCallback(() => {
    commit(defaultDesktopAcrylicSettingsForTheme(theme));
  }, [commit, theme]);

  return (
    <SlideOver onClose={onClose} width={360}>
      {({ close }) => (
        <>
          <div className="ace-acrylic-settings-header">
            <div className="min-w-0">
              <div className="ace-acrylic-settings-title">亚克力参数</div>
              <div className="ace-acrylic-settings-subtitle">左侧栏</div>
            </div>
            <button
              type="button"
              className="ace-acrylic-settings-icon-btn"
              onClick={close}
              title="关闭"
              aria-label="关闭"
            >
              <VsIcon name="close" size={15} />
            </button>
          </div>

          <div className="ace-acrylic-settings-body">
            <label className="ace-acrylic-settings-row">
              <span className="ace-acrylic-settings-label">颜色</span>
              <span className="ace-acrylic-settings-color-control">
                <input
                  className="ace-acrylic-settings-color"
                  type="color"
                  value={settings.tintColor}
                  onChange={(event) => update({ tintColor: event.target.value })}
                  aria-label="亚克力颜色"
                />
                <input
                  className="ace-acrylic-settings-hex"
                  type="text"
                  value={settings.tintColor}
                  maxLength={7}
                  onChange={(event) => update({ tintColor: event.target.value })}
                  aria-label="亚克力颜色 Hex"
                />
              </span>
            </label>

            <SliderRow
              label="原生透明度"
              value={settings.tintOpacity}
              onChange={(value) => update({ tintOpacity: value })}
            />
            <SliderRow
              label="侧栏罩色"
              value={settings.sidebarTintOpacity}
              onChange={(value) => update({ sidebarTintOpacity: value })}
            />
            <SliderRow
              label="悬停底色"
              value={settings.hoverOpacity}
              onChange={(value) => update({ hoverOpacity: value })}
            />
            <SliderRow
              label="选中底色"
              value={settings.activeOpacity}
              onChange={(value) => update({ activeOpacity: value })}
            />

            <div
              className="ace-acrylic-settings-preview"
              style={{
                backgroundColor: `rgba(var(--ace-sidebar-acrylic-tint-rgb), ${settings.sidebarTintOpacity})`,
              }}
            >
              <div
                className="ace-acrylic-settings-preview-row"
                style={{ backgroundColor: `rgba(var(--ace-surface-hi-rgb), ${settings.hoverOpacity})` }}
              >
                Hover
              </div>
              <div
                className="ace-acrylic-settings-preview-row text-accent"
                style={{ backgroundColor: `rgba(var(--ace-accent-rgb), ${settings.activeOpacity})` }}
              >
                Active
              </div>
            </div>

            <div className="ace-acrylic-settings-footer">
              <button type="button" className="ace-acrylic-settings-btn" onClick={reset}>
                重置
              </button>
              <button type="button" className="ace-acrylic-settings-btn ace-acrylic-settings-btn-primary" onClick={close}>
                完成
              </button>
            </div>
          </div>
        </>
      )}
    </SlideOver>
  );
}
