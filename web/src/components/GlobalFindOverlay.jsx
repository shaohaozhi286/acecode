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

function selectedTextForFind() {
  const text = window.getSelection?.()?.toString?.().trim() || '';
  if (!text || text.length > 80 || /[\r\n]/.test(text)) return '';
  return text;
}

export function GlobalFindOverlay() {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [matches, setMatches] = useState([]);
  const [activeIndex, setActiveIndex] = useState(-1);
  const [focusNonce, setFocusNonce] = useState(0);
  const [navigationNonce, setNavigationNonce] = useState(0);
  const inputRef = useRef(null);
  const composingRef = useRef(false);
  const queryRef = useRef(query);
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

  const close = useCallback(() => {
    setOpen(false);
    setNavigationNonce(0);
    clearFindHighlights(document);
    clearFindSelection(document);
  }, []);

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
      if (!isFindShortcut(event)) return;
      event.preventDefault();
      event.stopPropagation();
      const selected = selectedTextForFind();
      if (selected) setQuery(selected);
      setOpen(true);
      setNavigationNonce(0);
      requestInputFocus({ select: true, syncValue: true });
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => window.removeEventListener('keydown', onKeyDown, true);
  }, [requestInputFocus]);

  useEffect(() => {
    if (!open) return;
    const nextMatches = collectFindMatches(document.body, query);
    setMatches(nextMatches);
    setActiveIndex(nextMatches.length > 0 ? 0 : -1);
    setNavigationNonce(0);
  }, [open, query]);

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
    <div className="ace-global-find" role="search" aria-label="页面查找">
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
        placeholder="搜索对话..."
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
