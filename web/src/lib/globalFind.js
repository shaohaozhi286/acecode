const FIND_HIGHLIGHT = 'ace-global-find-match';
const FIND_ACTIVE_HIGHLIGHT = 'ace-global-find-active';
const FIND_HIGHLIGHT_STYLE_ID = 'ace-global-find-highlight-style';

const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE']);

function hasFindModifier(event) {
  return !!(event && (event.ctrlKey || event.metaKey));
}

export function isFindShortcut(event) {
  if (!hasFindModifier(event)) return false;
  if (event.altKey) return false;
  return typeof event.key === 'string' && event.key.toLowerCase() === 'f';
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
  const registry = doc?.defaultView?.CSS?.highlights;
  if (!registry) return;
  registry.delete(FIND_HIGHLIGHT);
  registry.delete(FIND_ACTIVE_HIGHLIGHT);
}

function ensureFindHighlightStyle(doc) {
  if (!doc?.head || doc.getElementById(FIND_HIGHLIGHT_STYLE_ID)) return;
  const style = doc.createElement('style');
  style.id = FIND_HIGHLIGHT_STYLE_ID;
  style.textContent = `
::highlight(${FIND_HIGHLIGHT}) {
  background: rgba(37, 99, 235, 0.22);
  color: inherit;
}
::highlight(${FIND_ACTIVE_HIGHLIGHT}) {
  background: rgba(37, 99, 235, 0.82);
  color: #fff;
}
`;
  doc.head.appendChild(style);
}

export function applyFindHighlights(matches, activeIndex, doc = globalThis.document) {
  const view = doc?.defaultView;
  const registry = view?.CSS?.highlights;
  const Highlight = view?.Highlight;
  if (!registry || !Highlight) return false;

  ensureFindHighlightStyle(doc);
  clearFindHighlights(doc);
  const ranges = matches
    .map((match) => rangeForMatch(match, doc))
    .filter(Boolean);
  if (ranges.length > 0) {
    registry.set(FIND_HIGHLIGHT, new Highlight(...ranges));
  }
  const activeRange = activeIndex >= 0 ? rangeForMatch(matches[activeIndex], doc) : null;
  if (activeRange) {
    registry.set(FIND_ACTIVE_HIGHLIGHT, new Highlight(activeRange));
  }
  return true;
}

export function scrollFindMatchIntoView(match, doc = globalThis.document) {
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
  const range = rangeForMatch(match, doc);
  const selection = doc?.defaultView?.getSelection?.();
  if (!range || !selection) return;
  selection.removeAllRanges();
  selection.addRange(range);
}

export function clearFindSelection(doc = globalThis.document) {
  doc?.defaultView?.getSelection?.()?.removeAllRanges?.();
}
