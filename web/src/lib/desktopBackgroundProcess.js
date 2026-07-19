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

export function desktopBackgroundProcessAvailable(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return !!win
    && win.__ACECODE_DESKTOP_SHELL__ === true
    && typeof win.aceDesktop_getBackgroundProcessPreference === 'function'
    && typeof win.aceDesktop_setBackgroundProcessPreference === 'function';
}

function unavailableState() {
  return { ok: false, enabled: false, unavailable: true };
}

export async function getDesktopBackgroundProcess(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopBackgroundProcessAvailable(win)) return unavailableState();
  try {
    const result = parseBridgeResult(
      await win.aceDesktop_getBackgroundProcessPreference(),
    );
    if (!result) return { ok: false, enabled: false, error: '原生桥返回了无效结果' };
    return {
      ...result,
      ok: result.ok !== false,
      enabled: result.enabled === true,
    };
  } catch (error) {
    return {
      ok: false,
      enabled: false,
      error: error?.message || String(error),
    };
  }
}

export async function setDesktopBackgroundProcess(
  enabled,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopBackgroundProcessAvailable(win)) return unavailableState();
  try {
    const result = parseBridgeResult(
      await win.aceDesktop_setBackgroundProcessPreference(!!enabled),
    );
    if (!result) return { ok: false, enabled: false, error: '原生桥返回了无效结果' };
    return {
      ...result,
      ok: result.ok !== false,
      enabled: result.enabled === true,
    };
  } catch (error) {
    return {
      ok: false,
      enabled: false,
      error: error?.message || String(error),
    };
  }
}
