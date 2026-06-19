export const DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY = 'acecode.desktop.sidebarAcrylicSettings.v2';

export const LIGHT_DESKTOP_ACRYLIC_SETTINGS = Object.freeze({
  tintColor: '#f2f3f8',
  tintOpacity: 1,
  sidebarTintOpacity: 0,
  hoverOpacity: 0.46,
  activeOpacity: 0.12,
});

export const DARK_DESKTOP_ACRYLIC_SETTINGS = Object.freeze({
  tintColor: '#1c1f21',
  tintOpacity: 1,
  sidebarTintOpacity: 0.42,
  hoverOpacity: 0.44,
  activeOpacity: 0.12,
});

export const DEFAULT_DESKTOP_ACRYLIC_SETTINGS = LIGHT_DESKTOP_ACRYLIC_SETTINGS;

function themeKey(theme) {
  return theme === 'dark' ? 'dark' : 'light';
}

export function defaultDesktopAcrylicSettingsForTheme(theme = 'light') {
  return themeKey(theme) === 'dark'
    ? DARK_DESKTOP_ACRYLIC_SETTINGS
    : LIGHT_DESKTOP_ACRYLIC_SETTINGS;
}

function clampOpacity(value, fallback) {
  const number = Number(value);
  if (!Number.isFinite(number)) return fallback;
  if (number < 0) return 0;
  if (number > 1) return 1;
  return number;
}

function normalizeHexColor(value, fallback) {
  if (typeof value !== 'string') return fallback;
  const trimmed = value.trim();
  const match = trimmed.match(/^#?([0-9a-fA-F]{6})$/);
  return match ? `#${match[1].toLowerCase()}` : fallback;
}

export function hexToRgb(value, fallback = DEFAULT_DESKTOP_ACRYLIC_SETTINGS.tintColor) {
  const color = normalizeHexColor(value, fallback);
  const hex = color.slice(1);
  return {
    red: Number.parseInt(hex.slice(0, 2), 16),
    green: Number.parseInt(hex.slice(2, 4), 16),
    blue: Number.parseInt(hex.slice(4, 6), 16),
  };
}

function opacityFromAlpha(value, fallback) {
  const alpha = Number(value);
  if (!Number.isFinite(alpha)) return fallback;
  return clampOpacity(Math.round(alpha) / 255, fallback);
}

function parseBridgeResult(value) {
  if (value == null) return null;
  if (typeof value === 'string') {
    try { return JSON.parse(value); } catch { return null; }
  }
  return typeof value === 'object' ? value : null;
}

export function normalizeDesktopAcrylicSettings(value, fallback = DEFAULT_DESKTOP_ACRYLIC_SETTINGS) {
  const source = value && typeof value === 'object' ? value : {};
  const safeFallback = fallback && typeof fallback === 'object'
    ? fallback
    : DEFAULT_DESKTOP_ACRYLIC_SETTINGS;
  const tintFallback = clampOpacity(safeFallback.tintOpacity, DEFAULT_DESKTOP_ACRYLIC_SETTINGS.tintOpacity);
  const tintColorFallback = normalizeHexColor(
    safeFallback.tintColor,
    DEFAULT_DESKTOP_ACRYLIC_SETTINGS.tintColor,
  );
  const tintOpacity = source.tintAlpha == null
    ? clampOpacity(source.tintOpacity, tintFallback)
    : opacityFromAlpha(source.tintAlpha, tintFallback);
  return {
    tintColor: normalizeHexColor(source.tintColor, tintColorFallback),
    tintOpacity,
    sidebarTintOpacity: clampOpacity(source.sidebarTintOpacity, safeFallback.sidebarTintOpacity),
    hoverOpacity: clampOpacity(source.hoverOpacity, safeFallback.hoverOpacity),
    activeOpacity: clampOpacity(source.activeOpacity, safeFallback.activeOpacity),
  };
}

function parseStoredSettings(raw) {
  if (!raw) return null;
  try {
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === 'object' ? parsed : null;
  } catch {
    return null;
  }
}

export function loadStoredDesktopAcrylicSettings(
  storage = typeof window !== 'undefined' ? window.localStorage : undefined,
  theme = 'light',
) {
  if (!storage) return null;
  let raw = null;
  try {
    raw = storage.getItem(DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY);
  } catch {
    return null;
  }
  const parsed = parseStoredSettings(raw);
  if (!parsed) return null;

  const fallback = defaultDesktopAcrylicSettingsForTheme(theme);
  if (parsed.light || parsed.dark) {
    const themed = parsed[themeKey(theme)];
    return themed ? normalizeDesktopAcrylicSettings(themed, fallback) : null;
  }
  return normalizeDesktopAcrylicSettings(parsed, fallback);
}

export function saveDesktopAcrylicSettings(
  settings,
  storage = typeof window !== 'undefined' ? window.localStorage : undefined,
  theme = 'light',
) {
  const normalized = normalizeDesktopAcrylicSettings(
    settings,
    defaultDesktopAcrylicSettingsForTheme(theme),
  );
  if (!storage) return normalized;
  try {
    const existing = parseStoredSettings(storage.getItem(DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY));
    const next = existing && typeof existing === 'object' && (existing.light || existing.dark)
      ? { ...existing }
      : {};
    next[themeKey(theme)] = normalized;
    storage.setItem(DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY, JSON.stringify(next));
  } catch {
    // Storage can fail in restricted WebView profiles; keep the live settings.
  }
  return normalized;
}

function formatOpacity(value) {
  return clampOpacity(value, 0).toFixed(3).replace(/0+$/u, '').replace(/\.$/u, '');
}

export function applyDesktopAcrylicCssVariables(
  root = typeof document !== 'undefined' ? document.documentElement : undefined,
  settings = DEFAULT_DESKTOP_ACRYLIC_SETTINGS,
) {
  if (!root?.style) return normalizeDesktopAcrylicSettings(settings);
  const normalized = normalizeDesktopAcrylicSettings(settings);
  const { red, green, blue } = hexToRgb(normalized.tintColor);
  root.style.setProperty('--ace-sidebar-acrylic-tint-rgb', `${red}, ${green}, ${blue}`);
  root.style.setProperty('--ace-sidebar-acrylic-tint-alpha', formatOpacity(normalized.sidebarTintOpacity));
  root.style.setProperty('--ace-sidebar-acrylic-hover-alpha', formatOpacity(normalized.hoverOpacity));
  root.style.setProperty('--ace-sidebar-acrylic-active-alpha', formatOpacity(normalized.activeOpacity));
  return normalized;
}

export function settingsToNativeAcrylicPayload(settings) {
  const normalized = normalizeDesktopAcrylicSettings(settings);
  return {
    tintColor: normalized.tintColor,
    tintOpacity: normalized.tintOpacity,
    tintAlpha: Math.round(normalized.tintOpacity * 255),
  };
}

export function normalizeNativeAcrylicResponse(value, fallback = DEFAULT_DESKTOP_ACRYLIC_SETTINGS) {
  const raw = parseBridgeResult(value);
  if (!raw || raw.ok === false) return null;
  return normalizeDesktopAcrylicSettings(raw, fallback);
}

export async function readNativeDesktopAcrylicSettings(
  win = typeof window !== 'undefined' ? window : undefined,
  fallback = DEFAULT_DESKTOP_ACRYLIC_SETTINGS,
) {
  const bridge = win && typeof win.aceDesktop_getSidebarAcrylicSettings === 'function'
    ? win.aceDesktop_getSidebarAcrylicSettings
    : null;
  if (!bridge) return null;
  try {
    return normalizeNativeAcrylicResponse(await bridge(), fallback);
  } catch {
    return null;
  }
}

export async function applyNativeDesktopAcrylicSettings(
  settings,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  const bridge = win && typeof win.aceDesktop_setSidebarAcrylicSettings === 'function'
    ? win.aceDesktop_setSidebarAcrylicSettings
    : null;
  if (!bridge) return null;
  const normalized = normalizeDesktopAcrylicSettings(settings);
  try {
    return normalizeNativeAcrylicResponse(
      await bridge(settingsToNativeAcrylicPayload(normalized)),
      normalized,
    );
  } catch {
    return null;
  }
}

export async function loadInitialDesktopAcrylicSettings(
  win = typeof window !== 'undefined' ? window : undefined,
  storage = win?.localStorage,
  theme = 'light',
) {
  const fallback = defaultDesktopAcrylicSettingsForTheme(theme);
  const stored = loadStoredDesktopAcrylicSettings(storage, theme);
  if (stored) return stored;
  return fallback;
}
