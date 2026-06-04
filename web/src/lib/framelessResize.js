export const FRAMELESS_RESIZE_ACTION = Object.freeze({
  IGNORE: 'ignore',
  RESIZE: 'resize',
  TOGGLE_MAXIMIZE: 'toggle-maximize',
});

export function framelessResizeMouseDownAction({
  direction,
  button = 0,
  detail = 0,
  canToggleMaximize = false,
} = {}) {
  if (button !== 0) return FRAMELESS_RESIZE_ACTION.IGNORE;

  const clickDetail = Number.isFinite(detail) ? detail : 0;
  if (direction === 'top' && clickDetail >= 2 && canToggleMaximize) {
    return FRAMELESS_RESIZE_ACTION.TOGGLE_MAXIMIZE;
  }

  return FRAMELESS_RESIZE_ACTION.RESIZE;
}
