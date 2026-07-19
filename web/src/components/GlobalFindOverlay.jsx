import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { VsIcon } from './Icon.jsx';
import {
  applyFindHighlights,
  clearFindHighlights,
  clearFindSelection,
  collectFindMatches,
  isComposingInputEvent,
  isFindShortcut,
  scrollFindMatchIntoView,
  selectFindMatch,
} from '../lib/globalFind.js';

export const CONVERSATION_FIND_ROOT_SELECTOR = '[data-conversation-find-root="true"]';

function selectedTextForFind(root) {
  const selection = window.getSelection?.();
  if (root && selection?.anchorNode && !root.contains(selection.anchorNode)) return '';
  const text = selection?.toString?.().trim() || '';
  if (!text || text.length > 80 || /[\r\n]/.test(text)) return '';
  return text;
}

export function GlobalFindOverlay({
  enabled = false,
  openRequest = 0,
  scopeKey = '',
  rootSelector = CONVERSATION_FIND_ROOT_SELECTOR,
}) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [matches, setMatches] = useState([]);
  const [activeIndex, setActiveIndex] = useState(-1);
  const [focusNonce, setFocusNonce] = useState(0);
  const [navigationNonce, setNavigationNonce] = useState(0);
  const overlayRef = useRef(null);
  const inputRef = useRef(null);
  const composingRef = useRef(false);
  const queryRef = useRef(query);
  const lastOpenRequestRef = useRef(openRequest);
  const previousScopeKeyRef = useRef(scopeKey);
  const selectOnFocusRef = useRef(false);
  const syncValueOnFocusRef = useRef(false);

  queryRef.current = query;

  const focusFindInput = useCallback(() => {
    const input = inputRef.current;
    if (!input) return;
    if (syncValueOnFocusRef.current && !composingRef.current && input.value !== queryRef.current) {
      input.value = queryRef.current;
    }
    input.focus({ preventScroll: true });
    if (selectOnFocusRef.current && !composingRef.current) {
      input.select();
    }
    selectOnFocusRef.current = false;
    syncValueOnFocusRef.current = false;
  }, []);

  const requestInputFocus = useCallback(({ select = false, syncValue = false } = {}) => {
    selectOnFocusRef.current = select;
    syncValueOnFocusRef.current = syncValue;
    setFocusNonce((value) => value + 1);
  }, []);

  const resolveFindRoot = useCallback(
    () => document.querySelector(rootSelector),
    [rootSelector],
  );

  const close = useCallback(() => {
    setOpen(false);
    setNavigationNonce(0);
    clearFindHighlights(document);
    clearFindSelection(document);
  }, []);

  const openFind = useCallback(() => {
    if (!enabled) return;
    const selected = selectedTextForFind(resolveFindRoot());
    if (selected) setQuery(selected);
    setOpen(true);
    setNavigationNonce(0);
    requestInputFocus({ select: true, syncValue: true });
  }, [enabled, requestInputFocus, resolveFindRoot]);

  useLayoutEffect(() => {
    if (!open) return undefined;
    focusFindInput();
    const frame = requestAnimationFrame(focusFindInput);
    const timer = window.setTimeout(focusFindInput, 80);
    return () => {
      cancelAnimationFrame(frame);
      window.clearTimeout(timer);
    };
  }, [focusFindInput, focusNonce, open]);

  useEffect(() => {
    const onKeyDown = (event) => {
      if (!enabled || !isFindShortcut(event)) return;
      event.preventDefault();
      event.stopPropagation();
      openFind();
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => window.removeEventListener('keydown', onKeyDown, true);
  }, [enabled, openFind]);

  useEffect(() => {
    if (!enabled) {
      lastOpenRequestRef.current = openRequest;
      close();
      return;
    }
    if (lastOpenRequestRef.current === openRequest) return;
    lastOpenRequestRef.current = openRequest;
    openFind();
  }, [close, enabled, openFind, openRequest]);

  useEffect(() => {
    if (previousScopeKeyRef.current === scopeKey) return;
    previousScopeKeyRef.current = scopeKey;
    close();
  }, [close, scopeKey]);

  useEffect(() => {
    if (!open) return undefined;
    const onPointerDown = (event) => {
      if (overlayRef.current?.contains(event.target)) return;
      close();
    };
    document.addEventListener('pointerdown', onPointerDown, true);
    return () => document.removeEventListener('pointerdown', onPointerDown, true);
  }, [close, open]);

  useEffect(() => {
    if (!open) return;
    clearFindHighlights(document);
    const nextMatches = collectFindMatches(resolveFindRoot(), query);
    setMatches(nextMatches);
    setActiveIndex(nextMatches.length > 0 ? 0 : -1);
    setNavigationNonce(0);
  }, [open, query, resolveFindRoot]);

  useEffect(() => {
    if (!open) return undefined;
    if (!query || activeIndex < 0 || matches.length === 0) {
      clearFindHighlights(document);
      if (document.activeElement !== inputRef.current) {
        clearFindSelection(document);
      }
      return undefined;
    }
    const highlighted = applyFindHighlights(matches, activeIndex, document);
    const activeMatch = matches[activeIndex];
    if (navigationNonce > 0 && !composingRef.current) {
      scrollFindMatchIntoView(activeMatch, document);
      if (!highlighted) selectFindMatch(activeMatch, document);
    }
    return undefined;
  }, [activeIndex, matches, navigationNonce, open, query]);

  useEffect(() => () => {
    clearFindHighlights(document);
    clearFindSelection(document);
  }, []);

  const move = useCallback((direction) => {
    if (!matches.length) return;
    setNavigationNonce((value) => value + 1);
    setActiveIndex((current) => {
      const base = current >= 0 ? current : 0;
      return (base + direction + matches.length) % matches.length;
    });
  }, [matches.length]);

  const onInputKeyDown = (event) => {
    if (isComposingInputEvent(event, composingRef.current)) return;
    if (event.key === 'Escape') {
      event.preventDefault();
      close();
    } else if (event.key === 'Enter') {
      event.preventDefault();
      move(event.shiftKey ? -1 : 1);
    }
  };

  const keepInputFocus = (event) => {
    event.preventDefault();
  };

  const onInputChange = (event) => {
    if (isComposingInputEvent(event, composingRef.current)) return;
    setQuery(event.currentTarget.value);
  };

  if (!open) return null;

  const current = activeIndex >= 0 ? activeIndex + 1 : 0;
  const disabled = matches.length === 0;

  return (
    <div
      ref={overlayRef}
      className="ace-global-find"
      role="search"
      aria-label="当前对话内容查找"
    >
      <VsIcon name="search" size={15} className="ace-global-find-search" />
      <input
        ref={inputRef}
        defaultValue={query}
        onChange={onInputChange}
        onCompositionStart={() => {
          composingRef.current = true;
        }}
        onCompositionEnd={(event) => {
          composingRef.current = false;
          setQuery(event.currentTarget.value);
        }}
        onKeyDown={onInputKeyDown}
        className="ace-global-find-input"
        aria-label="查找"
        placeholder="搜索当前对话内容"
        spellCheck={false}
      />
      <span className="ace-global-find-count" aria-live="polite">
        {current}/{matches.length}
      </span>
      <span className="ace-global-find-divider" aria-hidden="true" />
      <button
        type="button"
        className="ace-global-find-button"
        onMouseDown={keepInputFocus}
        onClick={() => move(-1)}
        disabled={disabled}
        title="上一个"
        aria-label="上一个"
      >
        <VsIcon name="glyphUp" size={13} />
      </button>
      <button
        type="button"
        className="ace-global-find-button"
        onMouseDown={keepInputFocus}
        onClick={() => move(1)}
        disabled={disabled}
        title="下一个"
        aria-label="下一个"
      >
        <VsIcon name="glyphDown" size={13} />
      </button>
      <button
        type="button"
        className="ace-global-find-close"
        onClick={close}
        title="关闭"
        aria-label="关闭"
      >
        <VsIcon name="close" size={14} />
      </button>
    </div>
  );
}
