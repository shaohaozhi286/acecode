// ThemeContext + persist + system preference fallback。
//
// 主题切换不是改 inline style 而是改 <html data-theme="dark|light">,所有
// 颜色由 CSS variables 驱动(见 styles/globals.css)。
//
// 持久化走 lib/usePreference.js — 与 view / sidePanelCollapsed / layout widths
// 共享同一份 try/catch 兜底逻辑。useTheme() 对外 API 不变。

import { createContext, useCallback, useContext, useEffect } from 'react';
import { usePreference } from './lib/usePreference.js';

const STORAGE_KEY = 'ace.theme';
const ThemeCtx = createContext({ theme: 'light', toggle: () => {}, set: () => {} });

function isValidTheme(v) { return v === 'light' || v === 'dark'; }

function systemThemeFallback() {
  if (typeof window !== 'undefined' && window.matchMedia?.('(prefers-color-scheme: dark)').matches) {
    return 'dark';
  }
  return 'light';
}

export function ThemeProvider({ children }) {
  // 默认值在用户从未选择时按系统偏好,选过后由 usePreference 的 storage 路径
  // 锁定。readWithFallback 校验通过的字符串原样返回,不会被默认值覆盖。
  const [theme, setTheme] = usePreference(STORAGE_KEY, systemThemeFallback(), isValidTheme);

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  const toggle = useCallback(() => setTheme((t) => (t === 'dark' ? 'light' : 'dark')), [setTheme]);
  const set    = useCallback((t) => setTheme(t === 'dark' ? 'dark' : 'light'), [setTheme]);

  return (
    <ThemeCtx.Provider value={{ theme, toggle, set }}>
      {children}
    </ThemeCtx.Provider>
  );
}

export function useTheme() { return useContext(ThemeCtx); }
