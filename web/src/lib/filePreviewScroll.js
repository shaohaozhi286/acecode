function finite(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : 0;
}

function maxScroll(size, viewport) {
  return Math.max(0, finite(size) - finite(viewport));
}

function clamp(value, maximum) {
  return Math.min(Math.max(0, finite(value)), maximum);
}

export function captureFilePreviewScroll({
  scrollTop = 0,
  scrollLeft = 0,
  scrollHeight = 0,
  scrollWidth = 0,
  clientHeight = 0,
  clientWidth = 0,
} = {}) {
  const maxTop = maxScroll(scrollHeight, clientHeight);
  const maxLeft = maxScroll(scrollWidth, clientWidth);
  const top = clamp(scrollTop, maxTop);
  const left = clamp(scrollLeft, maxLeft);
  return {
    top,
    left,
    atBottom: maxTop > 0 && maxTop - top <= 2,
    atRight: maxLeft > 0 && maxLeft - left <= 2,
  };
}

export function restoredFilePreviewScroll(snapshot, {
  scrollHeight = 0,
  scrollWidth = 0,
  clientHeight = 0,
  clientWidth = 0,
} = {}) {
  const maxTop = maxScroll(scrollHeight, clientHeight);
  const maxLeft = maxScroll(scrollWidth, clientWidth);
  if (!snapshot || typeof snapshot !== 'object') return { top: 0, left: 0 };
  return {
    top: snapshot.atBottom ? maxTop : clamp(snapshot.top, maxTop),
    left: snapshot.atRight ? maxLeft : clamp(snapshot.left, maxLeft),
  };
}
