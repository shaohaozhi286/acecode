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

// 读系统剪贴板纯文本(控制台右键粘贴用)。右键是用户手势,WebView2 / 浏览器
// loopback(secure context)下 navigator.clipboard.readText 可直接读。失败返回
// { ok:false }, 调用方静默降级。
export async function readTextFromSystemClipboard(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  const clipboard = win?.navigator?.clipboard;
  if (clipboard && typeof clipboard.readText === 'function') {
    try {
      const text = await clipboard.readText();
      return { ok: true, text: String(text ?? ''), via: 'navigator' };
    } catch (error) {
      return { ok: false, error: error?.message || String(error) };
    }
  }
  return { ok: false, error: 'clipboard read unavailable' };
}

async function writeClipboardImage(clipboard, ClipboardItemCtor, mimeType, blob) {
  await clipboard.write([new ClipboardItemCtor({ [mimeType]: blob })]);
}

async function imageBlobToPngBlob(blob, win) {
  const createImageBitmapFn = win?.createImageBitmap
    || (typeof createImageBitmap !== 'undefined' ? createImageBitmap : null);
  const doc = win?.document || (typeof document !== 'undefined' ? document : null);
  if (typeof createImageBitmapFn !== 'function' || !doc?.createElement) {
    throw new Error('image/png conversion unavailable');
  }

  const bitmap = await createImageBitmapFn(blob);
  try {
    const canvas = doc.createElement('canvas');
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
    const context = canvas.getContext?.('2d');
    if (!context || typeof canvas.toBlob !== 'function') {
      throw new Error('image/png conversion unavailable');
    }
    context.drawImage(bitmap, 0, 0);
    return await new Promise((resolve, reject) => {
      canvas.toBlob((pngBlob) => {
        if (pngBlob) resolve(pngBlob);
        else reject(new Error('image/png conversion failed'));
      }, 'image/png');
    });
  } finally {
    bitmap?.close?.();
  }
}

export async function copyImageToSystemClipboard(
  imageUrl,
  {
    mimeType = '',
    fetchImpl = typeof fetch !== 'undefined' ? fetch : undefined,
    win = typeof window !== 'undefined' ? window : undefined,
  } = {},
) {
  const url = String(imageUrl || '');
  if (!url) return { ok: false, error: 'image URL required' };
  const clipboard = win?.navigator?.clipboard;
  const ClipboardItemCtor = win?.ClipboardItem || (typeof ClipboardItem !== 'undefined' ? ClipboardItem : null);
  if (!clipboard || typeof clipboard.write !== 'function' || typeof ClipboardItemCtor !== 'function') {
    return { ok: false, error: 'image clipboard unavailable' };
  }
  if (typeof fetchImpl !== 'function') {
    return { ok: false, error: 'fetch unavailable' };
  }

  try {
    const response = await fetchImpl(url, { credentials: 'same-origin' });
    if (!response?.ok) {
      return { ok: false, error: `image fetch failed${response?.status ? `:${response.status}` : ''}` };
    }
    const blob = await response.blob();
    const type = String(mimeType || blob?.type || '').toLowerCase();
    if (!type.startsWith('image/')) {
      return { ok: false, error: 'clipboard item is not an image' };
    }
    const imageBlob = blob.type === type ? blob : new Blob([blob], { type });
    try {
      await writeClipboardImage(clipboard, ClipboardItemCtor, type, imageBlob);
      return { ok: true, via: 'navigator', mimeType: type };
    } catch (writeError) {
      if (type === 'image/png') throw writeError;
      const pngBlob = await imageBlobToPngBlob(imageBlob, win);
      await writeClipboardImage(clipboard, ClipboardItemCtor, 'image/png', pngBlob);
      return { ok: true, via: 'navigator', mimeType: 'image/png', converted: true };
    }
  } catch (error) {
    return { ok: false, error: error?.message || String(error) };
  }
}
