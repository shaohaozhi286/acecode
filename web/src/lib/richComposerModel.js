import { resolveLeadingSlashCommand } from './slashCommands.js';
import { normalizeReferencePath } from './pathReference.js';

export const COMPOSER_PARAGRAPH = 'paragraph';
export const COMPOSER_COMMAND_TAG = 'command-tag';
export const COMPOSER_PATH_TAG = 'path-tag';

export function normalizeComposerPlainText(value = '') {
  return String(value ?? '').replace(/\r\n?/g, '\n');
}

export function isComposerCommandTag(value) {
  return !!value && value.type === COMPOSER_COMMAND_TAG;
}

export function isComposerPathTag(value) {
  return !!value && value.type === COMPOSER_PATH_TAG;
}

export function isComposerInlineTag(value) {
  return isComposerCommandTag(value) || isComposerPathTag(value);
}

function composerCommandTag(command) {
  return {
    type: COMPOSER_COMMAND_TAG,
    token: command?.token || (command?.name ? `/${command.name}` : ''),
    name: command?.name || '',
    kind: command?.kind || 'skill',
    description: command?.description || '',
    children: [{ text: '' }],
  };
}

function composerPathTag(token, path) {
  const normalizedPath = normalizeReferencePath(path);
  return {
    type: COMPOSER_PATH_TAG,
    token,
    path: normalizedPath,
    directory: normalizedPath.endsWith('/'),
    children: [{ text: '' }],
  };
}

function isTextNode(value) {
  return !!value && typeof value.text === 'string';
}

function appendText(children, text) {
  const value = String(text ?? '');
  const previous = children[children.length - 1];
  if (isTextNode(previous)) {
    previous.text += value;
    return;
  }
  children.push({ text: value });
}

function appendTag(children, tag) {
  if (!isTextNode(children[children.length - 1])) appendText(children, '');
  children.push(tag);
  appendText(children, '');
}

function horizontalSpace(ch) {
  return ch === ' ' || ch === '\t' || ch === '\r';
}

function pathTagAt(text, at, { lineTerminated = false } = {}) {
  if (text[at] !== '@') return null;
  if (at > 0 && !horizontalSpace(text[at - 1])) return null;

  if (text[at + 1] === '"') {
    const quoteEnd = text.indexOf('"', at + 2);
    if (quoteEnd < 0) return null;
    const end = quoteEnd + 1;
    if (end < text.length && !horizontalSpace(text[end])) return null;
    const path = text.slice(at + 2, quoteEnd);
    if (!path) return null;
    return {
      begin: at,
      end,
      tag: composerPathTag(text.slice(at, end), path),
    };
  }

  let end = at + 1;
  while (end < text.length && !horizontalSpace(text[end])) end += 1;
  const path = text.slice(at + 1, end);
  if (!path) return null;
  if (path.endsWith('/') && end === text.length && !lineTerminated) return null;
  return {
    begin: at,
    end,
    tag: composerPathTag(text.slice(at, end), path),
  };
}

function appendTokenizedLine(children, text, options = {}) {
  let cursor = 0;
  let plainStart = 0;
  while (cursor < text.length) {
    const parsed = text[cursor] === '@' ? pathTagAt(text, cursor, options) : null;
    if (!parsed) {
      cursor += 1;
      continue;
    }
    appendText(children, text.slice(plainStart, parsed.begin));
    appendTag(children, parsed.tag);
    cursor = parsed.end;
    plainStart = parsed.end;
  }
  appendText(children, text.slice(plainStart));
}

function composerParagraphFromLine(line, {
  command = null,
  lineTerminated = false,
} = {}) {
  const children = [];
  let rest = line;
  if (command && line.startsWith(command.token)) {
    appendTag(children, composerCommandTag(command));
    rest = line.slice(command.token.length);
  }
  appendTokenizedLine(children, rest, { lineTerminated });
  if (children.length === 0) children.push({ text: '' });
  return {
    type: COMPOSER_PARAGRAPH,
    children,
  };
}

export function composerDocumentFromText(value = '', commands = []) {
  const text = normalizeComposerPlainText(value);
  const command = resolveLeadingSlashCommand(text, commands);
  const lines = text.split('\n');
  return lines.map((line, index) => composerParagraphFromLine(line, {
    command: index === 0 ? command : null,
    lineTerminated: index < lines.length - 1,
  }));
}

function cloneComposerNode(node) {
  if (isTextNode(node)) return { ...node };
  if (!node || typeof node !== 'object') return node;
  return {
    ...node,
    children: Array.isArray(node.children)
      ? node.children.map((child) => cloneComposerNode(child))
      : [],
  };
}

function leadingCommandTagIndex(children) {
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    if (isComposerCommandTag(child)) return index;
    if (isTextNode(child) && child.text.length === 0) continue;
    return -1;
  }
  return -1;
}

export function composerLeadingCommandSignature(document) {
  const blocks = safeComposerDocument(document);
  const children = Array.isArray(blocks[0]?.children) ? blocks[0].children : [];
  const index = leadingCommandTagIndex(children);
  if (index < 0) return '';
  const tag = children[index];
  return [
    tag.token || '',
    tag.name || '',
    tag.kind || '',
    tag.description || '',
  ].join('\n');
}

function appendExistingChild(children, child) {
  if (isTextNode(child)) {
    appendText(children, child.text);
  } else if (isComposerInlineTag(child)) {
    appendTag(children, cloneComposerNode(child));
  } else {
    children.push(cloneComposerNode(child));
  }
}

export function composerDocumentWithSynchronizedLeadingCommand(document, value = '', commands = []) {
  const text = normalizeComposerPlainText(value);
  const blocks = safeComposerDocument(document).map((block) => cloneComposerNode(block));
  const firstBlock = blocks[0];
  const children = Array.isArray(firstBlock?.children) ? firstBlock.children : [{ text: '' }];
  const currentTagIndex = leadingCommandTagIndex(children);
  const command = resolveLeadingSlashCommand(text, commands);

  if (command && currentTagIndex >= 0) {
    children[currentTagIndex] = composerCommandTag(command);
    return blocks;
  }

  if (command && currentTagIndex < 0) {
    const first = children[0];
    if (!isTextNode(first) || !first.text.startsWith(command.token)) return blocks;
    const nextChildren = [];
    appendTag(nextChildren, composerCommandTag(command));
    appendText(nextChildren, first.text.slice(command.token.length));
    children.slice(1).forEach((child) => appendExistingChild(nextChildren, child));
    firstBlock.children = nextChildren;
    return blocks;
  }

  if (!command && currentTagIndex >= 0) {
    const nextChildren = [];
    children.forEach((child, index) => {
      if (index === currentTagIndex) {
        appendText(nextChildren, child.token || '');
      } else {
        appendExistingChild(nextChildren, child);
      }
    });
    if (nextChildren.length === 0) nextChildren.push({ text: '' });
    firstBlock.children = nextChildren;
  }
  return blocks;
}

function composerInlineText(node) {
  if (isTextNode(node)) return node.text;
  if (isComposerInlineTag(node)) return String(node.token || '');
  return Array.isArray(node?.children)
    ? node.children.map((child) => composerInlineText(child)).join('')
    : '';
}

export function composerTextFromDocument(document = []) {
  const blocks = Array.isArray(document) && document.length > 0
    ? document
    : [{ type: COMPOSER_PARAGRAPH, children: [{ text: '' }] }];
  return normalizeComposerPlainText(blocks.map((block) => (
    Array.isArray(block?.children)
      ? block.children.map((child) => composerInlineText(child)).join('')
      : ''
  )).join('\n'));
}

export function composerInlineSerializedLength(node) {
  return composerInlineText(node).length;
}

export function composerInlineTagRanges(document = []) {
  const ranges = [];
  const blocks = safeComposerDocument(document);
  let offset = 0;
  blocks.forEach((block, blockIndex) => {
    const children = Array.isArray(block?.children) ? block.children : [];
    children.forEach((child, childIndex) => {
      const length = composerInlineSerializedLength(child);
      if (isComposerInlineTag(child)) {
        ranges.push({
          node: child,
          path: [blockIndex, childIndex],
          start: offset,
          end: offset + length,
        });
      }
      offset += length;
    });
    if (blockIndex < blocks.length - 1) offset += 1;
  });
  return ranges;
}

function safeComposerDocument(document) {
  return Array.isArray(document) && document.length > 0
    ? document
    : [{ type: COMPOSER_PARAGRAPH, children: [{ text: '' }] }];
}

function paragraphLength(block) {
  return Array.isArray(block?.children)
    ? block.children.reduce((sum, child) => sum + composerInlineSerializedLength(child), 0)
    : 0;
}

function textPointBeforeChild(block, blockIndex, childIndex) {
  const children = Array.isArray(block?.children) ? block.children : [];
  for (let index = Math.min(childIndex - 1, children.length - 1); index >= 0; index -= 1) {
    if (isTextNode(children[index])) {
      return { path: [blockIndex, index], offset: children[index].text.length };
    }
  }
  return { path: [blockIndex, 0], offset: 0 };
}

function textPointAfterChild(block, blockIndex, childIndex) {
  const children = Array.isArray(block?.children) ? block.children : [];
  for (let index = Math.max(0, childIndex + 1); index < children.length; index += 1) {
    if (isTextNode(children[index])) {
      return { path: [blockIndex, index], offset: 0 };
    }
  }
  return textPointBeforeChild(block, blockIndex, children.length);
}

function pointInParagraph(block, blockIndex, offset, affinity) {
  const children = Array.isArray(block?.children) ? block.children : [{ text: '' }];
  let remaining = Math.max(0, offset);
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    if (isTextNode(child)) {
      if (remaining <= child.text.length) {
        return { path: [blockIndex, index], offset: remaining };
      }
      remaining -= child.text.length;
      continue;
    }
    const length = composerInlineSerializedLength(child);
    if (remaining === 0) return textPointBeforeChild(block, blockIndex, index);
    if (remaining < length) {
      return affinity === 'backward'
        ? textPointBeforeChild(block, blockIndex, index)
        : textPointAfterChild(block, blockIndex, index);
    }
    if (remaining === length) return textPointAfterChild(block, blockIndex, index);
    remaining -= length;
  }
  return textPointBeforeChild(block, blockIndex, children.length);
}

export function composerPointForPlainTextOffset(document, offset, affinity = 'forward') {
  const blocks = safeComposerDocument(document);
  const totalLength = composerTextFromDocument(blocks).length;
  const target = Math.max(0, Math.min(totalLength, Number.isFinite(offset) ? offset : totalLength));
  let consumed = 0;

  for (let blockIndex = 0; blockIndex < blocks.length; blockIndex += 1) {
    const block = blocks[blockIndex];
    const length = paragraphLength(block);
    if (target <= consumed + length) {
      return pointInParagraph(block, blockIndex, target - consumed, affinity);
    }
    consumed += length;
    if (blockIndex < blocks.length - 1) consumed += 1;
  }

  const lastIndex = blocks.length - 1;
  return pointInParagraph(blocks[lastIndex], lastIndex, paragraphLength(blocks[lastIndex]), 'forward');
}

export function composerPlainTextOffsetForPoint(document, point, affinity = 'forward') {
  const blocks = safeComposerDocument(document);
  const blockIndex = Math.max(0, Math.min(
    blocks.length - 1,
    Number.isFinite(point?.path?.[0]) ? point.path[0] : 0,
  ));
  let offset = 0;
  for (let index = 0; index < blockIndex; index += 1) {
    offset += paragraphLength(blocks[index]) + 1;
  }

  const block = blocks[blockIndex];
  const children = Array.isArray(block?.children) ? block.children : [];
  const childIndex = Math.max(0, Math.min(
    children.length,
    Number.isFinite(point?.path?.[1]) ? point.path[1] : 0,
  ));
  for (let index = 0; index < childIndex; index += 1) {
    offset += composerInlineSerializedLength(children[index]);
  }
  const child = children[childIndex];
  if (isTextNode(child)) {
    return offset + Math.max(0, Math.min(child.text.length, Number(point?.offset) || 0));
  }
  if (isComposerInlineTag(child)) {
    return offset + (affinity === 'backward' ? 0 : composerInlineSerializedLength(child));
  }
  return offset;
}

export function composerPlainTextRangeFromSelection(document, selection) {
  const anchor = composerPlainTextOffsetForPoint(document, selection?.anchor, 'backward');
  const focus = composerPlainTextOffsetForPoint(document, selection?.focus, 'forward');
  return {
    start: Math.min(anchor, focus),
    end: Math.max(anchor, focus),
    direction: anchor > focus ? 'backward' : anchor < focus ? 'forward' : 'none',
  };
}

function collapsedSelection(selection) {
  const anchor = selection?.anchor;
  const focus = selection?.focus;
  if (!anchor || !focus || anchor.offset !== focus.offset) return false;
  if (!Array.isArray(anchor.path) || !Array.isArray(focus.path)) return false;
  return anchor.path.length === focus.path.length
    && anchor.path.every((part, index) => part === focus.path[index]);
}

export function composerAdjacentTagDeletionRange(document, selection, direction) {
  if (!collapsedSelection(selection)) return null;
  const caret = composerPlainTextOffsetForPoint(document, selection.anchor);
  const text = composerTextFromDocument(document);
  const tag = composerInlineTagRanges(document).find((candidate) => {
    if (direction === 'backward') {
      return caret === candidate.end
        || (caret === candidate.end + 1 && /[ \t]/.test(text[candidate.end] || ''));
    }
    return direction === 'forward' && caret === candidate.start;
  });
  if (!tag) return null;
  const separatorEnd = /[ \t]/.test(text[tag.end] || '') ? tag.end + 1 : tag.end;
  return { start: tag.start, end: separatorEnd };
}

export function composerSelectionFromPlainTextRange(
  document,
  start,
  end = start,
  direction = 'none',
) {
  const safeStart = Math.max(0, Number.isFinite(start) ? start : 0);
  const safeEnd = Math.max(safeStart, Number.isFinite(end) ? end : safeStart);
  if (safeStart === safeEnd) {
    const point = composerPointForPlainTextOffset(
      document,
      safeStart,
      direction === 'backward' ? 'backward' : 'forward',
    );
    return { anchor: point, focus: point };
  }
  const startPoint = composerPointForPlainTextOffset(document, safeStart, 'backward');
  const endPoint = composerPointForPlainTextOffset(document, safeEnd, 'forward');
  if (direction === 'backward') {
    return { anchor: endPoint, focus: startPoint };
  }
  return { anchor: startPoint, focus: endPoint };
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
