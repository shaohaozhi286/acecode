export function isInputHistoryNavigationMode({ value = '', editedSinceHistory = false } = {}) {
  return String(value ?? '').length === 0 || !editedSinceHistory;
}

// 富文本编辑器在程序化设值(历史导航填充 / 外部 value 同步)和 selection-only
// 变化时也可能触发 onChange 回声,此时文本与当前 value 相同。这类回声不是用户编辑,
// 不能把 editedSinceHistory 置 true,否则历史浏览态一填充就被打断。
export function isUserComposerEdit({ nextValue = '', currentValue = '' } = {}) {
  return String(nextValue ?? '') !== String(currentValue ?? '');
}

// 从 transcript item 提取用户原始输入文本。skill 命令等场景下落库的 content 是
// 展开后的注入提示,原文存在 metadata.display_text,翻历史时必须优先取原文。
export function userInputTextFromTranscriptItem(item) {
  if (item?.kind !== 'msg' || item?.role !== 'user') return '';
  const displayText = item?.metadata?.display_text;
  if (typeof displayText === 'string' && displayText.trim()) return displayText;
  return String(item?.content || '');
}

const MAX_COMPOSER_HISTORY = 500;

// 上下键翻的历史 = per-cwd 输入历史(与 TUI 共享,受 max_entries 截断)+ 当前
// transcript 会话中用户发过的消息。会话消息排在尾部,↑ 优先翻到当前会话的输入,
// 翻完再进入 per-cwd 更早的历史;同文本去重保留最后一次出现。
export function buildComposerHistory({ cwdHistory = [], transcriptItems = [] } = {}) {
  const cwdEntries = Array.isArray(cwdHistory) ? cwdHistory : [];
  const items = Array.isArray(transcriptItems) ? transcriptItems : [];
  const merged = [];
  for (const entry of cwdEntries) {
    const text = String(entry ?? '');
    if (text.trim()) merged.push(text);
  }
  for (const item of items) {
    const text = userInputTextFromTranscriptItem(item);
    if (text.trim()) merged.push(text);
  }
  const seen = new Set();
  const deduped = [];
  for (let index = merged.length - 1; index >= 0; index -= 1) {
    const text = merged[index];
    if (seen.has(text)) continue;
    seen.add(text);
    deduped.push(text);
  }
  deduped.reverse();
  return deduped.length > MAX_COMPOSER_HISTORY ? deduped.slice(-MAX_COMPOSER_HISTORY) : deduped;
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
