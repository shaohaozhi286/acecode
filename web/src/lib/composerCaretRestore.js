function containsElement(root, target) {
  if (!root || !target) return false;
  if (root === target) return true;
  if (typeof root.contains !== 'function') return false;
  try {
    return root.contains(target);
  } catch {
    return false;
  }
}

function clampOffset(value, max) {
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return Math.max(0, Math.min(max, number));
}

const DESKTOP_WINDOW_FOCUS_EVENT = 'acecode:desktop-window-focus';
const TERMINAL_FOCUS_REGION_SELECTOR = '[data-ace-focus-region="terminal"]';

function meaningfulFocusElement(element, documentRef) {
  if (!element) return null;
  if (element === documentRef?.body || element === documentRef?.documentElement) return null;
  return element;
}

function liveTerminalFocusRegion(element) {
  if (!element || typeof element.closest !== 'function') return null;
  let region = null;
  try {
    region = element.closest(TERMINAL_FOCUS_REGION_SELECTOR);
  } catch {
    return null;
  }
  if (!region || region.isConnected === false) return null;
  if (region.getAttribute?.('data-collapsed') === 'true') return null;
  return region;
}

function readTextareaSelection(textareaElement) {
  if (!textareaElement) return null;
  const max = String(textareaElement.value ?? '').length;
  const start = clampOffset(textareaElement.selectionStart, max);
  const end = clampOffset(textareaElement.selectionEnd, max);
  if (start == null || end == null) return null;
  return {
    start,
    end,
    direction: textareaElement.selectionDirection || 'none',
  };
}

export function shouldRestoreComposerTextareaFocus({
  activeElement,
  rootElement,
  textareaElement,
  bodyElement,
  documentElement,
} = {}) {
  if (!textareaElement) return false;
  if (!activeElement) return true;
  if (activeElement === textareaElement) return true;
  if (containsElement(rootElement, activeElement)) return true;
  return activeElement === bodyElement || activeElement === documentElement;
}

export function restoreComposerTextareaCaret({
  textareaElement,
  rootElement,
  selection,
  documentRef = typeof document === 'undefined' ? null : document,
  allowExternalFocus = false,
} = {}) {
  if (!textareaElement) return false;
  const activeElement = documentRef?.activeElement || null;
  if (!allowExternalFocus && !shouldRestoreComposerTextareaFocus({
    activeElement,
    rootElement,
    textareaElement,
    bodyElement: documentRef?.body,
    documentElement: documentRef?.documentElement,
  })) {
    return false;
  }

  const liveSelection = activeElement === textareaElement ? readTextareaSelection(textareaElement) : null;
  const fallbackSelection = readTextareaSelection(textareaElement);
  const targetSelection = liveSelection || selection || fallbackSelection;

  try {
    textareaElement.focus({ preventScroll: true });
  } catch {
    try {
      textareaElement.focus();
    } catch {
      return false;
    }
  }

  if (targetSelection && typeof textareaElement.setSelectionRange === 'function') {
    try {
      const max = String(textareaElement.value ?? '').length;
      const start = clampOffset(targetSelection.start, max);
      const end = clampOffset(targetSelection.end, max);
      if (start != null && end != null) {
        textareaElement.setSelectionRange(start, end, targetSelection.direction || 'none');
      }
    } catch {
      // Some old WebViews can reject selection updates during teardown; focus is still useful.
    }
  }

  return true;
}

export function captureComposerTextareaSelection(textareaElement) {
  return readTextareaSelection(textareaElement);
}

export function shouldAutoFocusDesktopComposer({
  desktopMode,
  chatVisible = false,
  blockingSurfaceOpen = false,
} = {}) {
  return desktopMode === 'shell' && !!chatVisible && !blockingSurfaceOpen;
}

export function bindDesktopComposerAutoFocus({
  enabled = false,
  onFocus,
  win = typeof window === 'undefined' ? null : window,
  documentRef = typeof document === 'undefined' ? null : document,
} = {}) {
  if (!enabled || typeof onFocus !== 'function' || !win || !documentRef) return () => {};

  let lastFocusedElement = meaningfulFocusElement(documentRef.activeElement, documentRef);
  const rememberFocusOwner = (event) => {
    const element = meaningfulFocusElement(event?.target || documentRef.activeElement, documentRef);
    if (element) lastFocusedElement = element;
  };
  const currentFocusOwner = () => (
    meaningfulFocusElement(documentRef.activeElement, documentRef) || lastFocusedElement
  );
  const focusIfCurrentWindow = () => {
    if (documentRef.visibilityState === 'hidden') return;
    const blockingSurface = documentRef.querySelector?.('[role="dialog"], .ace-global-find');
    if (blockingSurface) return;
    if (liveTerminalFocusRegion(currentFocusOwner())) return;
    onFocus();
  };
  const onVisibilityChange = () => {
    if (documentRef.visibilityState === 'visible') focusIfCurrentWindow();
  };

  documentRef.addEventListener('focusin', rememberFocusOwner);
  win.addEventListener('focus', focusIfCurrentWindow);
  win.addEventListener('pageshow', focusIfCurrentWindow);
  win.addEventListener(DESKTOP_WINDOW_FOCUS_EVENT, focusIfCurrentWindow);
  documentRef.addEventListener('visibilitychange', onVisibilityChange);
  return () => {
    documentRef.removeEventListener('focusin', rememberFocusOwner);
    win.removeEventListener('focus', focusIfCurrentWindow);
    win.removeEventListener('pageshow', focusIfCurrentWindow);
    win.removeEventListener(DESKTOP_WINDOW_FOCUS_EVENT, focusIfCurrentWindow);
    documentRef.removeEventListener('visibilitychange', onVisibilityChange);
  };
}

export function requestDesktopWindowFocus(win = typeof window === 'undefined' ? null : window) {
  const focusWindow = win?.aceDesktop_focusWindow;
  if (typeof focusWindow !== 'function') return false;
  try {
    Promise.resolve(focusWindow()).catch(() => {});
    return true;
  } catch {
    return false;
  }
}
