export function isInputHistoryNavigationMode({ value = '', editedSinceHistory = false } = {}) {
  return String(value ?? '').length === 0 || !editedSinceHistory;
}

// Lexical 在程序化设值(历史导航填充 / ValueSyncPlugin 外部同步)和 selection-only
// 变化时也会触发 onChange 回声,此时文本与当前 value 相同。这类回声不是用户编辑,
// 不能把 editedSinceHistory 置 true,否则历史浏览态一填充就被打断。
export function isUserComposerEdit({ nextValue = '', currentValue = '' } = {}) {
  return String(nextValue ?? '') !== String(currentValue ?? '');
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
