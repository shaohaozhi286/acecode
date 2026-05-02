// ThemeContext + persist + system preference fallback。
//
// 主题切换不是改 inline style 而是改 <html data-theme="dark|light">,所有
// 颜色由 CSS variables 驱动(见 styles/globals.css)。

import { createContext, useCallback, useContext, useEffect, useState } from 'react';

const STORAGE_KEY = 'ace.theme';
const ThemeCtx = createContext({ theme: 'light', toggle: () => {} });

function readInitialTheme() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved === 'light' || saved === 'dark') return saved;
  } catch {}
  if (typeof window !== 'undefined' && window.matchMedia?.('(prefers-color-scheme: dark)').matches) {
    return 'dark';
  }
  return 'light';
}

export function ThemeProvider({ children }) {
  const [theme, setTheme] = useState(() => readInitialTheme());

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem(STORAGE_KEY, theme); } catch {}
  }, [theme]);

  const toggle = useCallback(() => setTheme((t) => (t === 'dark' ? 'light' : 'dark')), []);
  const set    = useCallback((t) => setTheme(t === 'dark' ? 'dark' : 'light'), []);

  return (
    <ThemeCtx.Provider value={{ theme, toggle, set }}>
      {children}
    </ThemeCtx.Provider>
  );
}

export function useTheme() { return useContext(ThemeCtx); }
