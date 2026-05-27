export function getInputBarActionState({ value = '', disabled = false, busy = false, hasExtras = false } = {}) {
  const hasText = String(value || '').trim().length > 0;
  const hasSubmittableContent = hasText || !!hasExtras;
  const isDisabled = !!disabled;
  const isBusy = !!busy;
  return {
    hasText,
    hasExtras: !!hasExtras,
    mode: isBusy ? 'queue' : 'send',
    canSubmit: hasSubmittableContent && !isDisabled,
    canAbort: isBusy,
    submitLabel: isBusy ? '排队' : '发送',
    submitTitle: isBusy ? '排队下一条 (Enter)' : '发送 (Enter)',
    helperText: isBusy
      ? 'Enter 排队 · Shift+Enter 换行 · 空输入上下键历史'
      : 'Enter 发送 · Shift+Enter 换行 · 空输入上下键历史',
  };
}
