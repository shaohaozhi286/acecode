import assert from 'node:assert/strict';
import {
  FRAMELESS_RESIZE_ACTION,
  framelessResizeMouseDownAction,
} from './framelessResize.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('non-left button ignores frameless resize handle', () => {
  assert.equal(
    framelessResizeMouseDownAction({
      direction: 'top',
      button: 1,
      detail: 2,
      canToggleMaximize: true,
    }),
    FRAMELESS_RESIZE_ACTION.IGNORE,
  );
});

test('second top-edge mousedown toggles maximize instead of resize', () => {
  assert.equal(
    framelessResizeMouseDownAction({
      direction: 'top',
      button: 0,
      detail: 2,
      canToggleMaximize: true,
    }),
    FRAMELESS_RESIZE_ACTION.TOGGLE_MAXIMIZE,
  );
});

test('single top-edge click still starts resize', () => {
  assert.equal(
    framelessResizeMouseDownAction({
      direction: 'top',
      button: 0,
      detail: 1,
      canToggleMaximize: true,
    }),
    FRAMELESS_RESIZE_ACTION.RESIZE,
  );
});

test('only top-edge double click toggles maximize', () => {
  for (const direction of ['bottom', 'left', 'right', 'top-left', 'top-right']) {
    assert.equal(
      framelessResizeMouseDownAction({
        direction,
        button: 0,
        detail: 2,
        canToggleMaximize: true,
      }),
      FRAMELESS_RESIZE_ACTION.RESIZE,
    );
  }
});

test('top-edge double click falls back to resize without maximize bridge', () => {
  assert.equal(
    framelessResizeMouseDownAction({
      direction: 'top',
      button: 0,
      detail: 2,
      canToggleMaximize: false,
    }),
    FRAMELESS_RESIZE_ACTION.RESIZE,
  );
});
