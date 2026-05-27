function parseBridgeResult(value) {
  if (value == null) return {};
  if (typeof value === 'string') {
    try { return JSON.parse(value); } catch { return { ok: false, error: value }; }
  }
  if (typeof value === 'object') return value;
  return { ok: false, error: String(value) };
}

export async function copyTextToSystemClipboard(
  text,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  const value = String(text ?? '');
  if (!value) return { ok: false, error: 'text required' };

  let bridgeError = '';
  const bridge = win && typeof win.aceDesktop_writeClipboardText === 'function'
    ? win.aceDesktop_writeClipboardText
    : null;
  if (bridge) {
    try {
      const result = parseBridgeResult(await bridge(value));
      if (result?.ok) return { ok: true, via: 'desktop' };
      bridgeError = result?.error || 'desktop clipboard failed';
    } catch (error) {
      bridgeError = error?.message || String(error);
    }
  }

  const clipboard = win?.navigator?.clipboard;
  if (clipboard && typeof clipboard.writeText === 'function') {
    try {
      await clipboard.writeText(value);
      return { ok: true, via: 'navigator' };
    } catch (error) {
      const browserError = error?.message || String(error);
      return {
        ok: false,
        error: bridgeError ? `${bridgeError}; ${browserError}` : browserError,
      };
    }
  }

  return {
    ok: false,
    error: bridgeError || 'clipboard unavailable',
  };
}
