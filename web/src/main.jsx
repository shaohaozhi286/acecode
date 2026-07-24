import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { ThemeProvider } from './theme.jsx';
import { App } from './App.jsx';
import { MermaidPreviewHost } from './components/MermaidPreviewHost.jsx';
import { installBrowserDefaultGuards } from './lib/browserDefaults.js';
import { installMermaidRenderer } from './lib/mermaidRenderer.js';
import { installWebappCompatFlag } from './lib/desktopShellMode.js';
import { i18n, tr } from './i18n/index.js';
import './styles/globals.css';

installBrowserDefaultGuards();
installMermaidRenderer();

// 必须在 React mount 前安装:DesktopContextMenu 等组件 mount 时即读取该标志。
// 固化到 sessionStorage 后,后续 replaceState 抹掉 query 也不影响识别。
if (installWebappCompatFlag()) {
  const updateCompatibilityTitle = () => {
    document.title = tr('app.compatibilityTitle');
  };
  updateCompatibilityTitle();
  i18n.on('languageChanged', updateCompatibilityTitle);
}

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <ThemeProvider>
      <App />
      <MermaidPreviewHost />
    </ThemeProvider>
  </StrictMode>,
);
