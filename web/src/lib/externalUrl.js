function parseBridgeResult(value) {
  if (value == null) return {};
  if (typeof value === 'string') {
    try { return JSON.parse(value); } catch { return { ok: false, error: value }; }
  }
  if (typeof value === 'object') return value;
  return { ok: false, error: String(value) };
}

export async function openExternalUrl(url, win = typeof window !== 'undefined' ? window : undefined) {
  const target = String(url || '').trim();
  if (!target) return { ok: false, error: 'url required' };

  const bridge = win && typeof win.aceDesktop_openExternalUrl === 'function'
    ? win.aceDesktop_openExternalUrl
    : null;
  if (bridge) {
    try {
      const result = parseBridgeResult(await bridge(target));
      return result?.ok ? { ok: true, via: 'desktop' } : {
        ok: false,
        via: 'desktop',
        error: result?.error || 'failed to open system browser',
      };
    } catch (error) {
      return {
        ok: false,
        via: 'desktop',
        error: error?.message || String(error),
      };
    }
  }

  if (win && typeof win.open === 'function') {
    win.open(target, '_blank', 'noopener,noreferrer');
    return { ok: true, via: 'window-open' };
  }
  return { ok: false, error: 'no external URL opener available' };
}
