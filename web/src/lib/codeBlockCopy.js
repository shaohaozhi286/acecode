export const CODE_COPY_FRAME_SELECTOR = '[data-code-copy-frame="true"]';
export const CODE_COPY_BUTTON_SELECTOR = '[data-code-copy-button="true"]';
export const CODE_COPY_SOURCE_SELECTOR = '[data-code-copy-source="true"]';

export function codeTextFromFrame(frame) {
  if (!frame || typeof frame.querySelector !== 'function') return '';
  const source = frame.querySelector(CODE_COPY_SOURCE_SELECTOR)
    || frame.querySelector('pre code')
    || frame.querySelector('pre');
  return String(source?.textContent || '');
}

export function codeTextFromCopyButtonTarget(target) {
  if (!target || typeof target.closest !== 'function') return null;
  const button = target.closest(CODE_COPY_BUTTON_SELECTOR);
  if (!button) return null;
  const frame = button.closest(CODE_COPY_FRAME_SELECTOR);
  if (!frame) return null;
  return codeTextFromFrame(frame);
}

export function isInlineCodeHtml(html) {
  const text = String(html || '');
  return /<code(\s|>)/i.test(text) && !/data-code-copy-frame="true"/i.test(text);
}

export async function copyTextToClipboard(text, clipboard = globalThis.navigator?.clipboard) {
  const value = String(text ?? '');
  if (!clipboard || typeof clipboard.writeText !== 'function') {
    throw new Error('clipboard unavailable');
  }
  await clipboard.writeText(value);
  return value;
}
