import assert from 'node:assert/strict';
import {
  PREVIEW_SELECTOR,
  previewElementFromTarget,
  shouldClearPreviewSelectionOnMouseDown,
} from './inactiveSelection.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function element({ inPreview = false } = {}) {
  return {
    nodeType: 1,
    closest(selector) {
      return inPreview && selector === PREVIEW_SELECTOR ? this : null;
    },
  };
}

run('previewElementFromTarget resolves element and text-node preview targets', () => {
  const preview = element({ inPreview: true });
  assert.equal(previewElementFromTarget(preview), preview);
  assert.equal(previewElementFromTarget({ nodeType: 3, parentElement: preview }), preview);
});

run('preview mousedown clears saved inactive selection state', () => {
  assert.equal(
    shouldClearPreviewSelectionOnMouseDown(
      { button: 0, target: element({ inPreview: true }) },
      { hasSavedRanges: true },
    ),
    true,
  );
});

run('preview mousedown clears a live selection anchored in the preview', () => {
  const preview = element({ inPreview: true });
  assert.equal(
    shouldClearPreviewSelectionOnMouseDown(
      { button: 0, target: preview },
      { selection: { rangeCount: 1, anchorNode: { nodeType: 3, parentElement: preview } } },
    ),
    true,
  );
});

run('outside and non-left preview clicks do not clear selection state', () => {
  assert.equal(
    shouldClearPreviewSelectionOnMouseDown(
      { button: 0, target: element({ inPreview: false }) },
      { hasSavedRanges: true },
    ),
    false,
  );
  assert.equal(
    shouldClearPreviewSelectionOnMouseDown(
      { button: 2, target: element({ inPreview: true }) },
      { hasSavedRanges: true },
    ),
    false,
  );
});
