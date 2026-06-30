import { forwardRef, useCallback, useEffect, useImperativeHandle, useMemo, useRef } from 'react';
import { LexicalComposer } from '@lexical/react/LexicalComposer';
import { useLexicalComposerContext } from '@lexical/react/LexicalComposerContext';
import { PlainTextPlugin } from '@lexical/react/LexicalPlainTextPlugin';
import { ContentEditable } from '@lexical/react/LexicalContentEditable';
import { OnChangePlugin } from '@lexical/react/LexicalOnChangePlugin';
import { HistoryPlugin } from '@lexical/react/LexicalHistoryPlugin';
import { LexicalErrorBoundary } from '@lexical/react/LexicalErrorBoundary';
import {
  $createLineBreakNode,
  $createParagraphNode,
  $createRangeSelection,
  $createTextNode,
  $getRoot,
  $getSelection,
  $setSelection,
  $isElementNode,
  $isRangeSelection,
  $isTextNode,
  COMMAND_PRIORITY_HIGH,
  KEY_BACKSPACE_COMMAND,
  KEY_DELETE_COMMAND,
  KEY_ENTER_COMMAND,
  PASTE_COMMAND,
  TextNode,
} from 'lexical';
import { clsx } from '../lib/format.js';
import {
  clipboardHasRichText,
  normalizeComposerPlainText,
  plainTextFromClipboardData,
  richComposerModelFromText,
} from '../lib/richComposerModel.js';
import { filesFromClipboardEvent } from '../lib/composerFileTransfer.js';
import { slashCommandKindPresentation } from '../lib/slashCommands.js';

function commandIconFile(command) {
  const presentation = slashCommandKindPresentation(command);
  return presentation.icon === 'tool' ? 'Tool' : 'IntellisenseLightBulbSparkle';
}

function applyCommandTokenDom(dom, command) {
  const presentation = slashCommandKindPresentation(command);
  dom.classList.add('ace-rich-command-token');
  dom.dataset.slashChipKind = command?.kind || 'skill';
  dom.dataset.slashChipIcon = presentation.icon || '';
  dom.title = presentation.label || '';
  dom.style.setProperty('--ace-rich-command-token-icon-url', `url("/vs-icons/${commandIconFile(command)}.svg")`);
}

export class SlashCommandTokenNode extends TextNode {
  __command;

  static getType() {
    return 'slash-command-token';
  }

  static clone(node) {
    return new SlashCommandTokenNode(node.__command, node.__key);
  }

  static importJSON(serializedNode) {
    const node = $createSlashCommandTokenNode(serializedNode.command || { token: serializedNode.text || '' });
    node.setFormat(serializedNode.format || 0);
    node.setDetail(serializedNode.detail || 0);
    node.setStyle(serializedNode.style || '');
    return node;
  }

  constructor(command, key) {
    super(command?.token || (command?.name ? `/${command.name}` : ''), key);
    this.__mode = 1;
    this.__command = {
      token: command?.token || (command?.name ? `/${command.name}` : ''),
      name: command?.name || '',
      kind: command?.kind || 'skill',
      description: command?.description || '',
      rest: '',
    };
  }

  afterCloneFrom(prevNode) {
    super.afterCloneFrom(prevNode);
    this.__command = { ...prevNode.__command };
  }

  createDOM(config) {
    const dom = super.createDOM(config);
    applyCommandTokenDom(dom, this.__command);
    return dom;
  }

  updateDOM(prevNode, dom, config) {
    const shouldReplace = super.updateDOM(prevNode, dom, config);
    applyCommandTokenDom(dom, this.__command);
    return shouldReplace;
  }

  exportJSON() {
    return {
      ...super.exportJSON(),
      type: 'slash-command-token',
      version: 1,
      command: this.__command,
    };
  }

  isTextEntity() {
    return true;
  }

  getCommand() {
    return this.__command;
  }
}

export function $createSlashCommandTokenNode(command) {
  return new SlashCommandTokenNode(command);
}

export function $isSlashCommandTokenNode(node) {
  return node instanceof SlashCommandTokenNode;
}

function appendPlainText(parent, text) {
  const normalized = normalizeComposerPlainText(text);
  const parts = normalized.split('\n');
  parts.forEach((part, index) => {
    if (index > 0) parent.append($createLineBreakNode());
    if (part) parent.append($createTextNode(part));
  });
}

function setRootFromPlainText(text, commands, { selectEnd = false } = {}) {
  const root = $getRoot();
  root.clear();
  const paragraph = $createParagraphNode();
  const model = richComposerModelFromText(text, commands);
  if (model.kind === 'command') {
    paragraph.append($createSlashCommandTokenNode(model.command));
    appendPlainText(paragraph, model.rest);
  } else {
    appendPlainText(paragraph, model.text);
  }
  root.append(paragraph);
  if (selectEnd) root.selectEnd();
}

function serializeEditorText() {
  return normalizeComposerPlainText($getRoot().getTextContent());
}

function firstContentChild() {
  const root = $getRoot();
  const firstBlock = root.getFirstChild();
  if (!$isElementNode(firstBlock)) return null;
  return firstBlock.getFirstChild();
}

function rootNeedsCommandSync(text, commands) {
  const model = richComposerModelFromText(text, commands);
  const first = firstContentChild();
  if (model.kind !== 'command') return $isSlashCommandTokenNode(first);
  if (!$isSlashCommandTokenNode(first)) return true;
  return first.getCommand()?.token !== model.command.token;
}

function childrenForOffset() {
  const root = $getRoot();
  const firstBlock = root.getFirstChild();
  if (!$isElementNode(firstBlock)) return { parent: firstBlock, children: [] };
  return { parent: firstBlock, children: firstBlock.getChildren() };
}

function nodeTextSize(node) {
  if (!node) return 0;
  if (typeof node.getTextContentSize === 'function') return node.getTextContentSize();
  return String(node.getTextContent?.() || '').length;
}

function pointForPlainTextOffset(offset) {
  const target = Math.max(0, Number.isFinite(offset) ? offset : 0);
  const { parent, children } = childrenForOffset();
  if (!parent) {
    const root = $getRoot();
    return { key: root.getKey(), offset: root.getChildrenSize(), type: 'element' };
  }
  let cursor = 0;
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    const size = nodeTextSize(child);
    if (target <= cursor + size) {
      const local = Math.max(0, Math.min(size, target - cursor));
      if ($isSlashCommandTokenNode(child)) {
        return { key: parent.getKey(), offset: index + (local > 0 ? 1 : 0), type: 'element' };
      }
      if ($isTextNode(child)) {
        return { key: child.getKey(), offset: local, type: 'text' };
      }
      return { key: parent.getKey(), offset: index + (local > 0 ? 1 : 0), type: 'element' };
    }
    cursor += size;
  }
  return { key: parent.getKey(), offset: children.length, type: 'element' };
}

function selectPlainTextRange(start, end, direction = 'none') {
  const safeStart = Math.max(0, Number.isFinite(start) ? start : 0);
  const safeEnd = Math.max(0, Number.isFinite(end) ? end : safeStart);
  const anchorOffset = direction === 'backward' ? safeEnd : safeStart;
  const focusOffset = direction === 'backward' ? safeStart : safeEnd;
  const anchor = pointForPlainTextOffset(anchorOffset);
  const focus = pointForPlainTextOffset(focusOffset);
  const selection = $createRangeSelection();
  selection.anchor.set(anchor.key, anchor.offset, anchor.type);
  selection.focus.set(focus.key, focus.offset, focus.type);
  $setSelection(selection);
}

function plainTextOffsetForPoint(point) {
  const node = point.getNode();
  const { parent, children } = childrenForOffset();
  if (!parent) return 0;
  let offset = 0;
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    const size = nodeTextSize(child);
    if (child.is(node)) {
      if ($isSlashCommandTokenNode(child)) return offset + (point.offset > 0 ? size : 0);
      if ($isTextNode(child)) return offset + Math.max(0, Math.min(size, point.offset));
      return offset + (point.offset > 0 ? size : 0);
    }
    offset += size;
  }
  if (node.is(parent)) {
    const elementOffset = Math.max(0, Math.min(children.length, point.offset));
    return children.slice(0, elementOffset).reduce((sum, child) => sum + nodeTextSize(child), 0);
  }
  return serializeEditorText().length;
}

function readPlainSelection() {
  const selection = $getSelection();
  if (!$isRangeSelection(selection)) {
    const end = serializeEditorText().length;
    return { start: end, end, direction: 'none' };
  }
  const anchor = plainTextOffsetForPoint(selection.anchor);
  const focus = plainTextOffsetForPoint(selection.focus);
  return {
    start: Math.min(anchor, focus),
    end: Math.max(anchor, focus),
    direction: selection.isBackward() ? 'backward' : 'forward',
  };
}

function childAtElementPoint(point, direction) {
  const node = point.getNode();
  if (!$isElementNode(node)) return null;
  const children = node.getChildren();
  const index = direction === 'backward' ? point.offset - 1 : point.offset;
  return index >= 0 && index < children.length ? children[index] : null;
}

function adjacentCommandTokenForPoint(point, direction) {
  const node = point.getNode();
  if ($isSlashCommandTokenNode(node)) {
    const size = nodeTextSize(node);
    if (direction === 'backward' && point.offset > 0) return node;
    if (direction === 'forward' && point.offset < size) return node;
  }
  if ($isTextNode(node)) {
    const size = nodeTextSize(node);
    if (direction === 'backward' && point.offset === 0) {
      const previous = node.getPreviousSibling();
      return $isSlashCommandTokenNode(previous) ? previous : null;
    }
    if (direction === 'backward' && point.offset === 1 && /^\s/.test(node.getTextContent())) {
      const previous = node.getPreviousSibling();
      return $isSlashCommandTokenNode(previous) ? previous : null;
    }
    if (direction === 'forward' && point.offset === size) {
      const next = node.getNextSibling();
      return $isSlashCommandTokenNode(next) ? next : null;
    }
  }
  const adjacent = childAtElementPoint(point, direction);
  return $isSlashCommandTokenNode(adjacent) ? adjacent : null;
}

function removeCommandToken(tokenNode) {
  const next = tokenNode.getNextSibling();
  tokenNode.remove();
  if ($isTextNode(next)) {
    const text = next.getTextContent();
    if (text.length > 0 && /\s/.test(text[0])) {
      next.setTextContent(text.slice(1));
    }
  }
  selectPlainTextRange(0, 0);
}

function deleteAdjacentCommandToken(direction) {
  const selection = $getSelection();
  if (!$isRangeSelection(selection) || !selection.isCollapsed()) return false;
  const token = adjacentCommandTokenForPoint(selection.anchor, direction);
  if (!token) return false;
  removeCommandToken(token);
  return true;
}

function ValueSyncPlugin({ value, commands }) {
  const [editor] = useLexicalComposerContext();
  const commandSignature = useMemo(
    () => commands.map((command) => `${command?.kind || ''}:${command?.name || ''}`).join('\n'),
    [commands],
  );

  useEffect(() => {
    const next = normalizeComposerPlainText(value);
    let needsUpdate = false;
    editor.getEditorState().read(() => {
      const current = serializeEditorText();
      needsUpdate = current !== next || rootNeedsCommandSync(next, commands);
    });
    if (!needsUpdate) return;
    editor.update(() => setRootFromPlainText(next, commands, { selectEnd: true }));
  }, [commandSignature, commands, editor, value]);

  return null;
}

function EditableStatePlugin({ disabled }) {
  const [editor] = useLexicalComposerContext();
  useEffect(() => {
    editor.setEditable(!disabled);
  }, [disabled, editor]);
  return null;
}

function KeyAndPastePlugin({ disabled, onSubmit, onPasteFiles, isComposingKeyEvent }) {
  const [editor] = useLexicalComposerContext();

  useEffect(() => editor.registerCommand(
    KEY_ENTER_COMMAND,
    (event) => {
      if (disabled) {
        event?.preventDefault?.();
        return true;
      }
      if (!event || event.shiftKey || isComposingKeyEvent?.(event) || event.isComposing || event.keyCode === 229 || editor.isComposing()) {
        return false;
      }
      event.preventDefault();
      onSubmit?.();
      return true;
    },
    COMMAND_PRIORITY_HIGH,
  ), [disabled, editor, isComposingKeyEvent, onSubmit]);

  useEffect(() => editor.registerCommand(
    KEY_BACKSPACE_COMMAND,
    (event) => {
      if (disabled) {
        event?.preventDefault?.();
        return true;
      }
      const handled = deleteAdjacentCommandToken('backward');
      if (handled) event?.preventDefault?.();
      return handled;
    },
    COMMAND_PRIORITY_HIGH,
  ), [disabled, editor]);

  useEffect(() => editor.registerCommand(
    KEY_DELETE_COMMAND,
    (event) => {
      if (disabled) {
        event?.preventDefault?.();
        return true;
      }
      const handled = deleteAdjacentCommandToken('forward');
      if (handled) event?.preventDefault?.();
      return handled;
    },
    COMMAND_PRIORITY_HIGH,
  ), [disabled, editor]);

  useEffect(() => editor.registerCommand(
    PASTE_COMMAND,
    (event) => {
      if (disabled || !event) return false;
      const files = filesFromClipboardEvent(event);
      const text = plainTextFromClipboardData(event.clipboardData);
      const hasRichText = clipboardHasRichText(event.clipboardData);
      if (files.length === 0 && !text && !hasRichText) return false;
      event.preventDefault();
      event.stopPropagation();
      if (files.length > 0) onPasteFiles?.(files);
      if (text) {
        editor.update(() => {
          const selection = $getSelection();
          if ($isRangeSelection(selection)) selection.insertRawText(text);
        });
      }
      return true;
    },
    COMMAND_PRIORITY_HIGH,
  ), [disabled, editor, onPasteFiles]);

  return null;
}

function SelectionBridgePlugin({ composerRef, commands, onSelectionChange }) {
  const [editor] = useLexicalComposerContext();
  const latestTextRef = useRef('');
  const selectionRef = useRef({ start: 0, end: 0, direction: 'none' });

  useEffect(() => editor.registerUpdateListener(({ editorState }) => {
    editorState.read(() => {
      latestTextRef.current = serializeEditorText();
      selectionRef.current = readPlainSelection();
      onSelectionChange?.(selectionRef.current);
    });
  }), [editor, onSelectionChange]);

  useImperativeHandle(composerRef, () => ({
    focus() {
      editor.focus();
    },
    setSelectionRange(start, end, direction) {
      editor.update(() => selectPlainTextRange(start, Number.isFinite(end) ? end : start, direction));
    },
    get value() {
      return latestTextRef.current;
    },
    get selectionStart() {
      return selectionRef.current.start;
    },
    get selectionEnd() {
      return selectionRef.current.end;
    },
    get selectionDirection() {
      return selectionRef.current.direction || 'none';
    },
    getEditorStateText() {
      return latestTextRef.current;
    },
    replaceText(next, { selectEnd = true } = {}) {
      editor.update(() => setRootFromPlainText(next, commands, { selectEnd }));
    },
  }), [commands, editor]);

  return null;
}

function RichComposerShell({
  value,
  commands,
  disabled,
  placeholder,
  className,
  placeholderClassName,
  style,
  onChange,
  onKeyDown,
  onCompositionStart,
  onCompositionEnd,
  onSubmit,
  onPasteFiles,
  isComposingKeyEvent,
  onSelectionChange,
}, ref) {
  const initialConfig = useMemo(() => ({
    namespace: 'ACECodeRichComposer',
    nodes: [SlashCommandTokenNode],
    onError(error) {
      throw error;
    },
    editorState: () => {
      setRootFromPlainText(value, commands, { selectEnd: false });
    },
  // Initial config must be stable for LexicalComposer after mount.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }), []);

  const handleChange = useCallback((editorState) => {
    editorState.read(() => {
      const text = serializeEditorText();
      onChange?.(text);
    });
  }, [onChange]);

  return (
    <LexicalComposer initialConfig={initialConfig}>
      <div className="relative">
        <PlainTextPlugin
          contentEditable={(
            <ContentEditable
              aria-label={placeholder}
              className={className}
              style={style}
              onKeyDown={onKeyDown}
              onCompositionStart={onCompositionStart}
              onCompositionEnd={onCompositionEnd}
            />
          )}
          placeholder={(
            <div className={clsx(
              'pointer-events-none absolute inset-0 leading-[20px] font-sans text-fg-mute',
              placeholderClassName,
            )}
            >
              {placeholder}
            </div>
          )}
          ErrorBoundary={LexicalErrorBoundary}
        />
      </div>
      <HistoryPlugin />
      <OnChangePlugin onChange={handleChange} />
      <ValueSyncPlugin value={value} commands={commands} />
      <EditableStatePlugin disabled={disabled} />
      <KeyAndPastePlugin
        disabled={disabled}
        onSubmit={onSubmit}
        onPasteFiles={onPasteFiles}
        isComposingKeyEvent={isComposingKeyEvent}
      />
      <SelectionBridgePlugin
        composerRef={ref}
        commands={commands}
        onSelectionChange={onSelectionChange}
      />
    </LexicalComposer>
  );
}

export const RichComposer = forwardRef(RichComposerShell);
