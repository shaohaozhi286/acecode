import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { ThemeProvider } from './theme.jsx';
import { App } from './App.jsx';
import { installBrowserDefaultGuards } from './lib/browserDefaults.js';
import { installWebappCompatFlag } from './lib/desktopShellMode.js';
import './styles/globals.css';

const compatTitle =
  'ACECode(正以兼容模式运行，建议安装webview2再运行ACECode以获得更好的体验。)';

installBrowserDefaultGuards();

// 必须在 React mount 前安装:DesktopContextMenu 等组件 mount 时即读取该标志。
// 固化到 sessionStorage 后,后续 replaceState 抹掉 query 也不影响识别。
if (installWebappCompatFlag()) {
  document.title = compatTitle;
}

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <ThemeProvider>
      <App />
    </ThemeProvider>
  </StrictMode>,
);
