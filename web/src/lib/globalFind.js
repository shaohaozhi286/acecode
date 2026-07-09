import { markTextMatches, unwrapMarks } from './domTextMarks.js';

const FIND_HIGHLIGHT = 'ace-global-find-match';
const FIND_ACTIVE_HIGHLIGHT = 'ace-global-find-active';

const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE']);

function hasFindModifier(event) {
  return !!(event && (event.ctrlKey || event.metaKey));
}

export function isFindShortcut(event) {
  if (!event || event.altKey) return false;
  const key = typeof event.key === 'string' ? event.key.toLowerCase() : '';
  const code = typeof event.code === 'string' ? event.code.toLowerCase() : '';
  if ((key === 'f3' || code === 'f3') && !event.ctrlKey && !event.metaKey) {
    return true;
  }
  if (!hasFindModifier(event)) return false;
  return key === 'f';
}

export function isComposingInputEvent(event, composing = false) {
  return !!(
    composing ||
    event?.isComposing ||
    event?.nativeEvent?.isComposing
  );
}

export function findMatchesInText(text, query) {
  const source = String(text || '');
  const needle = String(query || '');
  if (!source || !needle) return [];
  const haystack = source.toLocaleLowerCase();
  const normalizedNeedle = needle.toLocaleLowerCase();
  const matches = [];
  let index = 0;
  while (index <= haystack.length - normalizedNeedle.length) {
    const found = haystack.indexOf(normalizedNeedle, index);
    if (found < 0) break;
    matches.push({ start: found, end: found + normalizedNeedle.length });
    index = found + Math.max(1, normalizedNeedle.length);
  }
  return matches;
}

function nodeWindow(node) {
  return node?.ownerDocument?.defaultView || globalThis.window || null;
}

function isHiddenElement(element, root) {
  const view = nodeWindow(element);
  for (let el = element; el && el !== root?.parentElement; el = el.parentElement) {
    if (el.nodeType !== 1) continue;
    if (el.classList?.contains('ace-global-find')) return true;
    if (el.classList?.contains(FIND_HIGHLIGHT)) return true;
    if (SKIP_TAGS.has(el.tagName)) return true;
    if (el.hidden || el.getAttribute?.('aria-hidden') === 'true') return true;
    if (!view?.getComputedStyle) continue;
    const style = view.getComputedStyle(el);
    if (style.display === 'none' || style.visibility === 'hidden') return true;
  }
  return false;
}

function isSearchableTextNode(node, root) {
  if (!node || node.nodeType !== 3) return false;
  if (!String(node.nodeValue || '').trim()) return false;
  const parent = node.parentElement;
  if (!parent) return false;
  return !isHiddenElement(parent, root);
}

export function collectFindMatches(root, query) {
  if (!root || !query) return [];
  const doc = root.ownerDocument || globalThis.document;
  if (!doc?.createTreeWalker) return [];
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(
    root,
    nodeFilter.SHOW_TEXT || 4,
    {
      acceptNode(node) {
        return isSearchableTextNode(node, root)
          ? (nodeFilter.FILTER_ACCEPT || 1)
          : (nodeFilter.FILTER_REJECT || 2);
      },
    },
  );
  const matches = [];
  let node = walker.nextNode();
  while (node) {
    for (const match of findMatchesInText(node.nodeValue, query)) {
      matches.push({ node, start: match.start, end: match.end });
    }
    node = walker.nextNode();
  }
  return matches;
}

function rangeForMatch(match, doc = globalThis.document) {
  if (!match?.node || !doc?.createRange) return null;
  const range = doc.createRange();
  range.setStart(match.node, match.start);
  range.setEnd(match.node, match.end);
  return range;
}

export function clearFindHighlights(doc = globalThis.document) {
  unwrapMarks(doc?.body || doc, FIND_HIGHLIGHT);
}

export function applyFindHighlights(matches, activeIndex, doc = globalThis.document) {
  return markTextMatches(matches, {
    className: FIND_HIGHLIGHT,
    activeClassName: FIND_ACTIVE_HIGHLIGHT,
    activeIndex,
  });
}

export function scrollFindMatchIntoView(match, doc = globalThis.document) {
  if (match?.mark?.isConnected) {
    match.mark.scrollIntoView?.({ block: 'center', inline: 'nearest' });
    return;
  }
  const range = rangeForMatch(match, doc);
  if (!range) return;
  const rect = range.getBoundingClientRect();
  const view = doc.defaultView || globalThis.window;
  if (!view || !Number.isFinite(rect.top)) return;
  const margin = 88;
  const outside =
    rect.top < margin ||
    rect.bottom > view.innerHeight - margin ||
    rect.left < 0 ||
    rect.right > view.innerWidth;
  if (!outside) return;
  match.node.parentElement?.scrollIntoView?.({ block: 'center', inline: 'nearest' });
}

export function selectFindMatch(match, doc = globalThis.document) {
  const range = match?.mark?.isConnected && doc?.createRange
    ? doc.createRange()
    : rangeForMatch(match, doc);
  if (match?.mark?.isConnected && range) range.selectNodeContents(match.mark);
  const selection = doc?.defaultView?.getSelection?.();
  if (!range || !selection) return;
  selection.removeAllRanges();
  selection.addRange(range);
}

export function clearFindSelection(doc = globalThis.document) {
  doc?.defaultView?.getSelection?.()?.removeAllRanges?.();
}
