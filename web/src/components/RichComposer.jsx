import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useMemo,
  useRef,
} from 'react';
import {
  createEditor,
  Editor,
  Range,
  Transforms,
} from 'slate';
import {
  Editable,
  ReactEditor,
  Slate,
  withReact,
} from 'slate-react';
import {
  HistoryEditor,
  withHistory,
} from 'slate-history';
import { clsx } from '../lib/format.js';
import {
  clipboardHasRichText,
  composerAdjacentTagDeletionRange,
  composerDocumentFromText,
  composerDocumentWithSynchronizedLeadingCommand,
  composerLeadingCommandSignature,
  composerPlainTextRangeFromSelection,
  composerSelectionFromPlainTextRange,
  composerTextFromDocument,
  isComposerCommandTag,
  isComposerInlineTag,
  isComposerPathTag,
  normalizeComposerPlainText,
  plainTextFromClipboardData,
} from '../lib/richComposerModel.js';
import { filesFromClipboardEvent, filesFromTransfer } from '../lib/composerFileTransfer.js';
import { slashCommandKindPresentation } from '../lib/slashCommands.js';
import { CommandGlyph, FileTypeIcon, VsIcon } from './Icon.jsx';

function withComposerInlineTags(editor) {
  const { isInline, isVoid, markableVoid } = editor;
  editor.isInline = (element) => (
    isComposerInlineTag(element) ? true : isInline(element)
  );
  editor.isVoid = (element) => (
    isComposerInlineTag(element) ? true : isVoid(element)
  );
  editor.markableVoid = (element) => (
    isComposerInlineTag(element) ? false : markableVoid?.(element) || false
  );
  return editor;
}

function commandTagTitle(command) {
  const presentation = slashCommandKindPresentation(command);
  return command?.description || presentation.label || command?.name || command?.token || '';
}

function CommandTagElement({ attributes, children, element }) {
  const displayName = String(element?.name || element?.token || '').replace(/^\/+/, '');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="command"
      data-slash-chip-kind={element?.kind || 'skill'}
      className="ace-cmd-token ace-slate-inline-tag"
      title={commandTagTitle(element)}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      <CommandGlyph kind={element?.kind || 'skill'} size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name">{displayName}</span>
    </span>
  );
}

function PathTagElement({ attributes, children, element }) {
  const path = String(element?.path || element?.token || '').replace(/^@(?:"(.*)"|(.*))$/, '$1$2');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="path"
      className="ace-cmd-token ace-slate-inline-tag ace-slate-path-tag"
      title={element?.token || path}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      {element?.directory
        ? <VsIcon name="folder" size={12} className="ace-cmd-token-glyph" />
        : <FileTypeIcon path={path} size={12} className="ace-cmd-token-glyph" />}
      <span className="ace-cmd-token-name">{path}</span>
    </span>
  );
}

function ComposerElement(props) {
  if (isComposerCommandTag(props.element)) return <CommandTagElement {...props} />;
  if (isComposerPathTag(props.element)) return <PathTagElement {...props} />;
  return (
    <div {...props.attributes} className="ace-slate-composer-paragraph">
      {props.children}
    </div>
  );
}

function currentPlainSelection(document, selection) {
  if (!selection) {
    const end = composerTextFromDocument(document).length;
    return { start: end, end, direction: 'none' };
  }
  return composerPlainTextRangeFromSelection(document, selection);
}

function replaceEditorDocument(editor, nextDocument, {
  selection = null,
  selectEnd = true,
  clearHistory = false,
} = {}) {
  const nextText = composerTextFromDocument(nextDocument);
  const plainSelection = selection || {
    start: selectEnd ? nextText.length : 0,
    end: selectEnd ? nextText.length : 0,
    direction: 'none',
  };
  const slateSelection = composerSelectionFromPlainTextRange(
    nextDocument,
    plainSelection.start,
    plainSelection.end,
    plainSelection.direction,
  );

  HistoryEditor.withoutSaving(editor, () => {
    Editor.withoutNormalizing(editor, () => {
      if (editor.selection) Transforms.deselect(editor);
      for (let index = editor.children.length - 1; index >= 0; index -= 1) {
        Transforms.removeNodes(editor, { at: [index] });
      }
      Transforms.insertNodes(editor, nextDocument, { at: [0] });
      Transforms.select(editor, slateSelection);
    });
  });

  if (clearHistory && HistoryEditor.isHistoryEditor(editor)) {
    editor.history.undos.splice(0);
    editor.history.redos.splice(0);
  }
}

function deleteAdjacentTag(editor, direction) {
  const range = composerAdjacentTagDeletionRange(editor.children, editor.selection, direction);
  if (!range) return false;
  Transforms.select(editor, composerSelectionFromPlainTextRange(
    editor.children,
    range.start,
    range.end,
  ));
  Transforms.delete(editor);
  return true;
}

function insertPlainText(editor, text) {
  const parts = normalizeComposerPlainText(text).split('\n');
  HistoryEditor.withNewBatch(editor, () => {
    parts.forEach((part, index) => {
      if (index > 0) editor.insertBreak();
      if (part) Transforms.insertText(editor, part);
    });
  });
}

function writeSelectedPlainText(event, editor) {
  if (!editor.selection || Range.isCollapsed(editor.selection)) return false;
  const text = composerTextFromDocument(editor.children);
  const selection = composerPlainTextRangeFromSelection(editor.children, editor.selection);
  try {
    event.clipboardData?.setData('text/plain', text.slice(selection.start, selection.end));
  } catch {
    return false;
  }
  event.preventDefault();
  return true;
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
  const commandsRef = useRef(commands);
  commandsRef.current = commands;
  const initialValueRef = useRef(null);
  if (!initialValueRef.current) {
    initialValueRef.current = composerDocumentFromText(value, commands);
  }
  const editor = useMemo(
    () => withComposerInlineTags(withHistory(withReact(createEditor()))),
    [],
  );
  const latestTextRef = useRef(composerTextFromDocument(initialValueRef.current));
  const selectionRef = useRef({
    start: latestTextRef.current.length,
    end: latestTextRef.current.length,
    direction: 'none',
  });

  const commandSignature = useMemo(
    () => (Array.isArray(commands) ? commands : [])
      .map((command) => [
        command?.token || '',
        command?.name || '',
        command?.kind || '',
        command?.description || '',
      ].join(':'))
      .join('\n'),
    [commands],
  );

  const publishSelection = useCallback((selection = editor.selection) => {
    const next = currentPlainSelection(editor.children, selection);
    selectionRef.current = next;
    onSelectionChange?.(next);
  }, [editor, onSelectionChange]);

  useEffect(() => {
    const nextText = normalizeComposerPlainText(value);
    const currentDocument = editor.children;
    const currentText = composerTextFromDocument(currentDocument);
    const textChanged = currentText !== nextText;
    let nextDocument = null;

    if (textChanged) {
      nextDocument = composerDocumentFromText(nextText, commands);
    } else {
      const synchronized = composerDocumentWithSynchronizedLeadingCommand(
        currentDocument,
        nextText,
        commands,
      );
      if (
        composerLeadingCommandSignature(synchronized)
        !== composerLeadingCommandSignature(currentDocument)
      ) {
        nextDocument = synchronized;
      }
    }

    if (!nextDocument) return;
    const preservedSelection = textChanged
      ? null
      : currentPlainSelection(currentDocument, editor.selection);
    replaceEditorDocument(editor, nextDocument, {
      selection: preservedSelection,
      selectEnd: true,
      clearHistory: textChanged,
    });
    latestTextRef.current = nextText;
    publishSelection(editor.selection);
  }, [commandSignature, commands, editor, publishSelection, value]);

  const handleValueChange = useCallback((nextDocument) => {
    const text = composerTextFromDocument(nextDocument);
    latestTextRef.current = text;
    publishSelection(editor.selection);
    onChange?.(text);
  }, [editor, onChange, publishSelection]);

  const handleSlateSelectionChange = useCallback((selection) => {
    publishSelection(selection);
  }, [publishSelection]);

  useImperativeHandle(ref, () => ({
    focus() {
      if (!editor.selection) {
        const end = latestTextRef.current.length;
        Transforms.select(editor, composerSelectionFromPlainTextRange(editor.children, end, end));
      }
      ReactEditor.focus(editor);
    },
    setSelectionRange(start, end, direction) {
      const selection = composerSelectionFromPlainTextRange(
        editor.children,
        start,
        Number.isFinite(end) ? end : start,
        direction,
      );
      Transforms.select(editor, selection);
      publishSelection(selection);
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
      const nextDocument = composerDocumentFromText(next, commandsRef.current);
      replaceEditorDocument(editor, nextDocument, {
        selectEnd,
        clearHistory: true,
      });
      latestTextRef.current = composerTextFromDocument(editor.children);
      publishSelection(editor.selection);
    },
  }), [editor, publishSelection]);

  const renderElement = useCallback((props) => <ComposerElement {...props} />, []);
  const renderPlaceholder = useCallback(({ attributes, children }) => (
    <span
      {...attributes}
      className={clsx(
        'pointer-events-none absolute inset-0 leading-[20px] font-sans text-fg-mute',
        placeholderClassName,
      )}
    >
      {children}
    </span>
  ), [placeholderClassName]);

  const handleKeyDown = useCallback((event) => {
    onKeyDown?.(event);
    if (event.defaultPrevented) return;
    if (disabled) {
      event.preventDefault();
      return;
    }

    if (event.key === 'Enter' && !event.shiftKey) {
      if (
        isComposingKeyEvent?.(event)
        || event.isComposing
        || event.nativeEvent?.isComposing
        || event.keyCode === 229
        || ReactEditor.isComposing(editor)
      ) {
        return;
      }
      event.preventDefault();
      onSubmit?.();
      return;
    }

    if (event.key === 'Backspace' && deleteAdjacentTag(editor, 'backward')) {
      event.preventDefault();
      return;
    }
    if (event.key === 'Delete' && deleteAdjacentTag(editor, 'forward')) {
      event.preventDefault();
    }
  }, [disabled, editor, isComposingKeyEvent, onKeyDown, onSubmit]);

  const handlePaste = useCallback((event) => {
    if (disabled) {
      event.preventDefault();
      return;
    }
    const clipboardData = event.clipboardData || event.nativeEvent?.clipboardData;
    const files = filesFromClipboardEvent(event);
    const text = plainTextFromClipboardData(clipboardData);
    const hasRichText = clipboardHasRichText(clipboardData);
    if (files.length === 0 && !text && !hasRichText) return;
    event.preventDefault();
    event.stopPropagation();
    if (files.length > 0) onPasteFiles?.(files);
    if (text) insertPlainText(editor, text);
  }, [disabled, editor, onPasteFiles]);

  const handleCopy = useCallback((event) => {
    writeSelectedPlainText(event, editor);
  }, [editor]);

  const handleCut = useCallback((event) => {
    if (disabled || !writeSelectedPlainText(event, editor)) return;
    Transforms.delete(editor);
  }, [disabled, editor]);

  const handleDrop = useCallback((event) => {
    const files = filesFromTransfer(event.dataTransfer);
    const types = Array.from(event.dataTransfer?.types || []);
    if (files.length > 0 || types.includes('application/x-slate-fragment')) {
      event.preventDefault();
    }
  }, []);

  return (
    <Slate
      editor={editor}
      initialValue={initialValueRef.current}
      onValueChange={handleValueChange}
      onSelectionChange={handleSlateSelectionChange}
    >
      <Editable
        aria-label={placeholder}
        aria-disabled={disabled ? 'true' : undefined}
        readOnly={disabled}
        placeholder={placeholder}
        className={className}
        style={style}
        renderElement={renderElement}
        renderPlaceholder={renderPlaceholder}
        onKeyDown={handleKeyDown}
        onPaste={handlePaste}
        onCopy={handleCopy}
        onCut={handleCut}
        onDrop={handleDrop}
        onCompositionStart={onCompositionStart}
        onCompositionEnd={onCompositionEnd}
        spellCheck
      />
    </Slate>
  );
}

export const RichComposer = forwardRef(RichComposerShell);
