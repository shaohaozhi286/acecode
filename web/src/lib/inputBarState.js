export function getInputBarActionState({ value = '', disabled = false, busy = false } = {}) {
  const hasText = String(value || '').trim().length > 0;
  const isDisabled = !!disabled;
  const isBusy = !!busy;
  return {
    hasText,
    mode: isBusy ? 'queue' : 'send',
    canSubmit: hasText && !isDisabled,
    canAbort: isBusy,
    submitLabel: isBusy ? '排队' : '发送',
    submitTitle: isBusy ? '排队下一条 (Enter)' : '发送 (Enter)',
    helperText: isBusy
      ? 'Enter 排队 · Shift+Enter 换行 · 上下键历史'
      : 'Enter 发送 · Shift+Enter 换行 · 上下键历史',
  };
}
