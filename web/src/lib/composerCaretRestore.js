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
