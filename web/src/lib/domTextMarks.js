export function unwrapMarks(root, className) {
  if (!root?.querySelectorAll || !className) return;
  const selector = `mark.${className}`;
  const marks = Array.from(root.querySelectorAll(selector));
  for (const mark of marks) {
    const parent = mark.parentNode;
    if (!parent) continue;
    while (mark.firstChild) {
      parent.insertBefore(mark.firstChild, mark);
    }
    parent.removeChild(mark);
    parent.normalize?.();
  }
}

export function wrapTextNodeRange(node, start, end, className) {
  if (!node || node.nodeType !== 3 || !node.parentNode || !className) return null;
  const text = String(node.nodeValue || '');
  const safeStart = Math.max(0, Math.min(Number(start) || 0, text.length));
  const safeEnd = Math.max(safeStart, Math.min(Number(end) || 0, text.length));
  if (safeEnd <= safeStart) return null;

  let target = node;
  if (safeEnd < text.length) {
    target.splitText(safeEnd);
  }
  if (safeStart > 0) {
    target = target.splitText(safeStart);
  }

  const doc = target.ownerDocument || globalThis.document;
  const mark = doc.createElement('mark');
  mark.className = className;
  mark.appendChild(target.cloneNode(true));
  target.parentNode.replaceChild(mark, target);
  return mark;
}

export function markTextMatches(matches = [], {
  className,
  activeClassName = '',
  activeIndex = -1,
} = {}) {
  const list = Array.isArray(matches) ? matches : [];
  for (const match of list) {
    if (match?.mark?.isConnected && activeClassName) {
      match.mark.classList.toggle(activeClassName, false);
    }
  }

  if (list.some((match) => match?.mark?.isConnected)) {
    const active = activeIndex >= 0 ? list[activeIndex]?.mark : null;
    if (active && activeClassName) active.classList.add(activeClassName);
    return true;
  }

  const indexed = list
    .map((match, index) => ({ match, index }))
    .filter(({ match }) => match?.node && match.start != null && match.end != null);
  indexed.sort((a, b) => {
    if (a.match.node === b.match.node) return b.match.start - a.match.start;
    const pos = a.match.node.compareDocumentPosition?.(b.match.node) || 0;
    if (pos & 4) return 1;
    if (pos & 2) return -1;
    return b.index - a.index;
  });

  for (const { match, index } of indexed) {
    const mark = wrapTextNodeRange(match.node, match.start, match.end, className);
    if (!mark) continue;
    if (activeClassName && index === activeIndex) mark.classList.add(activeClassName);
    match.mark = mark;
  }
  return indexed.length > 0;
}
