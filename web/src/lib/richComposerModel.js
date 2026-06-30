import { resolveLeadingSlashCommand } from './slashCommands.js';

export function normalizeComposerPlainText(value = '') {
  return String(value ?? '').replace(/\r\n?/g, '\n');
}

export function richComposerModelFromText(value = '', commands = []) {
  const text = normalizeComposerPlainText(value);
  const command = resolveLeadingSlashCommand(text, commands);
  if (!command) {
    return {
      kind: 'plain',
      text,
      command: null,
      rest: text,
    };
  }
  return {
    kind: 'command',
    text,
    command,
    rest: command.rest || '',
  };
}

export function richComposerTextFromModel(model = {}) {
  if (model?.kind === 'command' && model.command?.token) {
    return normalizeComposerPlainText(`${model.command.token}${model.rest || ''}`);
  }
  return normalizeComposerPlainText(model?.text || model?.rest || '');
}

export function normalizePastedPlainText(text = '') {
  return normalizeComposerPlainText(text);
}

export function plainTextFromClipboardData(clipboardData) {
  if (!clipboardData || typeof clipboardData.getData !== 'function') return '';
  try {
    return normalizePastedPlainText(clipboardData.getData('text/plain') || '');
  } catch {
    return '';
  }
}

export function clipboardHasRichText(clipboardData) {
  if (!clipboardData) return false;
  const types = Array.from(clipboardData.types || []);
  return types.some((type) => {
    const normalized = String(type || '').toLowerCase();
    return normalized === 'text/html' || normalized === 'text/rtf';
  });
}

export function commandTokenLabel(command) {
  return command?.name || '';
}
