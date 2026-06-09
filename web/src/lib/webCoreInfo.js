function cleanString(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function parseBridgeResult(value) {
  if (value == null) return null;
  if (typeof value === 'string') {
    try { return JSON.parse(value); } catch { return null; }
  }
  return typeof value === 'object' ? value : null;
}

export function formatProgramVersion(version) {
  const text = cleanString(version);
  if (!text) return '未知';
  return /^v/i.test(text) ? text : `v${text}`;
}

export function normalizeDesktopWebCoreInfo(value) {
  const raw = parseBridgeResult(value);
  if (!raw || raw.ok === false) return null;
  const info = {
    source: 'desktop',
    backend: cleanString(raw.backend),
    name: cleanString(raw.name),
    version: cleanString(raw.version),
    detail: cleanString(raw.detail),
    wrapperName: cleanString(raw.wrapper_name),
    wrapperVersion: cleanString(raw.wrapper_version),
  };
  return info.name || info.version || info.backend ? info : null;
}

export function webCoreInfoFromUserAgent(userAgent = '') {
  const ua = String(userAgent || '');
  const edge = ua.match(/\bEdg(?:A|iOS)?\/([\d.]+)/);
  if (edge) {
    return {
      source: 'browser',
      backend: 'edge',
      name: 'Edge/Chromium',
      version: edge[1],
      detail: '浏览器直连',
    };
  }

  const chrome = ua.match(/\b(?:Chrome|Chromium)\/([\d.]+)/);
  if (chrome) {
    return {
      source: 'browser',
      backend: 'chromium',
      name: 'Chromium',
      version: chrome[1],
      detail: '浏览器直连',
    };
  }

  const firefox = ua.match(/\bFirefox\/([\d.]+)/);
  if (firefox) {
    return {
      source: 'browser',
      backend: 'firefox',
      name: 'Firefox',
      version: firefox[1],
      detail: '浏览器直连',
    };
  }

  const safari = ua.match(/\bVersion\/([\d.]+).*?\bSafari\//);
  if (safari) {
    return {
      source: 'browser',
      backend: 'safari',
      name: 'Safari/WebKit',
      version: safari[1],
      detail: '浏览器直连',
    };
  }

  const webkit = ua.match(/\bAppleWebKit\/([\d.]+)/);
  if (webkit) {
    return {
      source: 'browser',
      backend: 'webkit',
      name: 'AppleWebKit',
      version: webkit[1],
      detail: '浏览器直连',
    };
  }

  return {
    source: 'browser',
    backend: 'browser',
    name: '浏览器 Web 核心',
    version: '',
    detail: '浏览器直连',
  };
}

export async function getCurrentWebCoreInfo(win = typeof window !== 'undefined' ? window : undefined) {
  const fallback = () => webCoreInfoFromUserAgent(win?.navigator?.userAgent || '');
  const bridge = win && typeof win.aceDesktop_getWebCoreInfo === 'function'
    ? win.aceDesktop_getWebCoreInfo
    : null;
  if (bridge) {
    try {
      const info = normalizeDesktopWebCoreInfo(await bridge());
      if (info) return info;
    } catch {
      // Fall through to user-agent detection.
    }
  }
  return fallback();
}

export function formatWebCoreLabel(info) {
  if (!info) return '检测中...';
  const name = cleanString(info.name) || cleanString(info.backend) || 'Web 核心';
  const version = cleanString(info.version);
  return version ? `${name} ${version}` : name;
}

export function formatWebCoreDetail(info) {
  if (!info) return '读取 Web 核心信息';
  const parts = [];
  const detail = cleanString(info.detail);
  if (detail) parts.push(detail);
  const wrapperVersion = cleanString(info.wrapperVersion);
  if (wrapperVersion) {
    const wrapperName = cleanString(info.wrapperName) || 'webview';
    parts.push(`${wrapperName} ${wrapperVersion}`);
  }
  return parts.join(' · ');
}
