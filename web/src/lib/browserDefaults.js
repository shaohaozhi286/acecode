// WebView/Chromium defaults that feel like browser chrome rather than app UI.
// Keep these guards at the document edge so individual controls can stay simple.

import { isFindShortcut } from './globalFind.js';

const ZOOM_SHORTCUT_KEYS = new Set(['+', '=', '-', '_', '0']);
const ZOOM_SHORTCUT_CODES = new Set([
  'Equal',
  'Minus',
  'Digit0',
  'NumpadAdd',
  'NumpadSubtract',
  'Numpad0',
]);

function hasZoomModifier(event) {
  return !!(event && (event.ctrlKey || event.metaKey));
}

export function isBrowserZoomShortcut(event) {
  if (!hasZoomModifier(event)) return false;
  const key = typeof event.key === 'string' ? event.key : '';
  const code = typeof event.code === 'string' ? event.code : '';
  return ZOOM_SHORTCUT_KEYS.has(key) || ZOOM_SHORTCUT_CODES.has(code);
}

export function isBlockedBrowserDefaultShortcut(event) {
  return isBrowserZoomShortcut(event) || isFindShortcut(event);
}

export function installBrowserDefaultGuards(target = globalThis.window) {
  if (!target || typeof target.addEventListener !== 'function') {
    return () => {};
  }

  const prevent = (event) => event.preventDefault();
  const onWheel = (event) => {
    if (hasZoomModifier(event)) event.preventDefault();
  };
  const onKeyDown = (event) => {
    if (isBlockedBrowserDefaultShortcut(event)) event.preventDefault();
  };

  const activeCapture = { capture: true, passive: false };
  target.addEventListener('wheel', onWheel, activeCapture);
  target.addEventListener('keydown', onKeyDown, true);
  target.addEventListener('gesturestart', prevent, activeCapture);
  target.addEventListener('gesturechange', prevent, activeCapture);
  target.addEventListener('gestureend', prevent, activeCapture);

  return () => {
    target.removeEventListener('wheel', onWheel, activeCapture);
    target.removeEventListener('keydown', onKeyDown, true);
    target.removeEventListener('gesturestart', prevent, activeCapture);
    target.removeEventListener('gesturechange', prevent, activeCapture);
    target.removeEventListener('gestureend', prevent, activeCapture);
  };
}
