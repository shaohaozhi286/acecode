export function isInputHistoryNavigationMode({ value = '', editedSinceHistory = false } = {}) {
  return String(value ?? '').length === 0 || !editedSinceHistory;
}

export function shouldNavigateInputHistory({
  key,
  value = '',
  editedSinceHistory = false,
  historyLength = 0,
  historyPointer = -1,
  altKey = false,
  ctrlKey = false,
  metaKey = false,
  shiftKey = false,
} = {}) {
  if (altKey || ctrlKey || metaKey || shiftKey) return false;
  if (!isInputHistoryNavigationMode({ value, editedSinceHistory })) return false;
  if (key === 'ArrowUp') return historyLength > 0;
  if (key === 'ArrowDown') return historyPointer >= 0;
  return false;
}

export function getNextInputHistoryPointer({ key, historyLength = 0, historyPointer = -1 } = {}) {
  const count = Math.max(0, Math.trunc(Number(historyLength) || 0));
  const pointer = Number.isInteger(historyPointer) ? historyPointer : -1;

  if (key === 'ArrowUp') {
    if (count === 0) return -1;
    return pointer === -1 ? count - 1 : Math.max(0, pointer - 1);
  }

  if (key === 'ArrowDown') {
    if (pointer < 0) return -1;
    const next = pointer + 1;
    return next >= count ? -1 : next;
  }

  return pointer;
}
