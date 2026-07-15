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

async function invokeDesktopBridge(win, bridgeName) {
  const bridge = win && typeof win[bridgeName] === 'function' ? win[bridgeName] : null;
  if (!bridge) return { ok: false, unavailable: true };
  try {
    const result = parseBridgeResult(await bridge.call(win));
    if (!result) return { ok: false, error: '原生桥返回了无效结果' };
    return result;
  } catch (error) {
    return { ok: false, error: error?.message || String(error) };
  }
}

export function showDesktopAboutDialog(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return invokeDesktopBridge(win, 'aceDesktop_showAboutDialog');
}

export function requestDesktopAppExit(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return invokeDesktopBridge(win, 'aceDesktop_quitApp');
}
