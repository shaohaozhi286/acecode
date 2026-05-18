import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { ThemeProvider } from './theme.jsx';
import { App } from './App.jsx';
import { installBrowserDefaultGuards } from './lib/browserDefaults.js';
import './styles/globals.css';

const compatTitle =
  'ACECode(正以兼容模式运行，建议安装webview2再运行ACECode以获得更好的体验。)';

installBrowserDefaultGuards();

if (new URLSearchParams(window.location.search).get('ace_webapp') === '1') {
  document.title = compatTitle;
}

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <ThemeProvider>
      <App />
    </ThemeProvider>
  </StrictMode>,
);
