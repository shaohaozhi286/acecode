export const DESKTOP_CLOSE_REQUEST_EVENT = 'acecode:desktop-close-requested';

export const DESKTOP_CLOSE_BEHAVIORS = Object.freeze({
  ASK: 'ask',
  MINIMIZE_TO_TRAY: 'minimize_to_tray',
  EXIT: 'exit',
});

export const DESKTOP_CLOSE_BEHAVIOR_OPTIONS = Object.freeze([
  Object.freeze({ value: DESKTOP_CLOSE_BEHAVIORS.ASK, label: '每次询问' }),
  Object.freeze({ value: DESKTOP_CLOSE_BEHAVIORS.MINIMIZE_TO_TRAY, label: '最小化到托盘' }),
  Object.freeze({ value: DESKTOP_CLOSE_BEHAVIORS.EXIT, label: '退出应用' }),
]);

function parseBridgeResult(value) {
  if (value && typeof value === 'object') return value;
  if (typeof value !== 'string') return null;
  try {
    const parsed = JSON.parse(value);
    return parsed && typeof parsed === 'object' ? parsed : null;
  } catch {
    return null;
  }
}

export function normalizeDesktopCloseBehavior(value) {
  return Object.values(DESKTOP_CLOSE_BEHAVIORS).includes(value)
    ? value
    : DESKTOP_CLOSE_BEHAVIORS.ASK;
}

function unavailableState() {
  return {
    ok: false,
    unavailable: true,
    behavior: DESKTOP_CLOSE_BEHAVIORS.ASK,
    trayAvailable: false,
  };
}

function normalizedState(result) {
  return {
    ...result,
    ok: result?.ok !== false,
    behavior: normalizeDesktopCloseBehavior(result?.behavior),
    trayAvailable: result?.tray_available !== false,
  };
}

export function desktopCloseBehaviorAvailable(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return !!win
    && win.__ACECODE_DESKTOP_SHELL__ === true
    && typeof win.aceDesktop_getCloseBehaviorPreference === 'function'
    && typeof win.aceDesktop_setCloseBehaviorPreference === 'function'
    && typeof win.aceDesktop_hideToTray === 'function';
}

export async function getDesktopCloseBehavior(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopCloseBehaviorAvailable(win)) return unavailableState();
  try {
    const result = parseBridgeResult(
      await win.aceDesktop_getCloseBehaviorPreference(),
    );
    if (!result) {
      return { ...unavailableState(), unavailable: false, error: '原生桥返回了无效结果' };
    }
    return normalizedState(result);
  } catch (error) {
    return {
      ...unavailableState(),
      unavailable: false,
      error: error?.message || String(error),
    };
  }
}

export async function setDesktopCloseBehavior(
  behavior,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopCloseBehaviorAvailable(win)) return unavailableState();
  const normalized = normalizeDesktopCloseBehavior(behavior);
  if (normalized !== behavior) {
    return {
      ...unavailableState(),
      unavailable: false,
      error: '无效的关闭窗口行为',
    };
  }
  try {
    const result = parseBridgeResult(
      await win.aceDesktop_setCloseBehaviorPreference(normalized),
    );
    if (!result) {
      return { ...unavailableState(), unavailable: false, error: '原生桥返回了无效结果' };
    }
    return normalizedState(result);
  } catch (error) {
    return {
      ...unavailableState(),
      unavailable: false,
      error: error?.message || String(error),
    };
  }
}

export async function hideDesktopToTray(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopCloseBehaviorAvailable(win)) return unavailableState();
  try {
    const result = parseBridgeResult(await win.aceDesktop_hideToTray());
    if (!result) return { ok: false, error: '原生桥返回了无效结果' };
    return result;
  } catch (error) {
    return { ok: false, error: error?.message || String(error) };
  }
}

export function subscribeDesktopCloseRequest(
  handler,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!win?.addEventListener || typeof handler !== 'function') return () => {};
  win.addEventListener(DESKTOP_CLOSE_REQUEST_EVENT, handler);
  return () => win.removeEventListener?.(DESKTOP_CLOSE_REQUEST_EVENT, handler);
}

export async function performDesktopCloseChoice({
  behavior,
  remember = false,
  persist,
  hideToTray,
  exitApp,
}) {
  const normalized = normalizeDesktopCloseBehavior(behavior);
  if (normalized === DESKTOP_CLOSE_BEHAVIORS.ASK) {
    return { ok: false, error: '请选择关闭窗口操作' };
  }
  if (remember) {
    const saved = await persist(normalized);
    if (!saved?.ok) return { ...saved, stage: 'persist' };
  }
  const result = normalized === DESKTOP_CLOSE_BEHAVIORS.MINIMIZE_TO_TRAY
    ? await hideToTray()
    : await exitApp();
  return { ...result, stage: 'action' };
}

