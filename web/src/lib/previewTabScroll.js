function finiteNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : 0;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

export function scrollLeftForVisibleTab({
  scrollLeft = 0,
  clientWidth = 0,
  scrollWidth = 0,
  tabOffsetLeft = 0,
  tabOffsetWidth = 0,
  gutter = 8,
} = {}) {
  const viewportWidth = Math.max(0, finiteNumber(clientWidth));
  const maxScroll = Math.max(0, finiteNumber(scrollWidth) - viewportWidth);
  const currentLeft = clamp(finiteNumber(scrollLeft), 0, maxScroll);
  if (viewportWidth <= 0 || maxScroll <= 0) return currentLeft;

  const itemStart = Math.max(0, finiteNumber(tabOffsetLeft) - Math.max(0, finiteNumber(gutter)));
  const itemEnd = Math.max(
    itemStart,
    finiteNumber(tabOffsetLeft) + Math.max(0, finiteNumber(tabOffsetWidth)) + Math.max(0, finiteNumber(gutter)),
  );
  const viewStart = currentLeft;
  const viewEnd = currentLeft + viewportWidth;

  if (itemStart < viewStart) return clamp(itemStart, 0, maxScroll);
  if (itemEnd > viewEnd) return clamp(itemEnd - viewportWidth, 0, maxScroll);
  return currentLeft;
}
