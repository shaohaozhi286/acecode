import assert from 'node:assert/strict';
import { captureFilePreviewScroll, restoredFilePreviewScroll } from './filePreviewScroll.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('file preview scroll preserves a valid middle position', () => {
  const snapshot = captureFilePreviewScroll({ scrollTop: 400, scrollLeft: 30, scrollHeight: 2000, clientHeight: 500, scrollWidth: 900, clientWidth: 600 });
  assert.deepEqual(restoredFilePreviewScroll(snapshot, { scrollHeight: 1800, clientHeight: 500, scrollWidth: 800, clientWidth: 600 }), { top: 400, left: 30 });
});

run('file preview scroll clamps when refreshed content shrinks or empties', () => {
  const snapshot = captureFilePreviewScroll({ scrollTop: 900, scrollLeft: 400, scrollHeight: 1500, clientHeight: 500, scrollWidth: 1000, clientWidth: 500 });
  assert.deepEqual(restoredFilePreviewScroll(snapshot, { scrollHeight: 700, clientHeight: 500, scrollWidth: 600, clientWidth: 500 }), { top: 200, left: 100 });
  assert.deepEqual(restoredFilePreviewScroll(snapshot, { scrollHeight: 0, clientHeight: 500, scrollWidth: 0, clientWidth: 500 }), { top: 0, left: 0 });
});

run('file preview scroll follows a new bottom but does not treat non-scrollable content as bottom-pinned', () => {
  const bottom = captureFilePreviewScroll({ scrollTop: 1000, scrollHeight: 1500, clientHeight: 500 });
  assert.deepEqual(restoredFilePreviewScroll(bottom, { scrollHeight: 900, clientHeight: 500 }), { top: 400, left: 0 });

  const short = captureFilePreviewScroll({ scrollTop: 0, scrollHeight: 300, clientHeight: 500 });
  assert.deepEqual(restoredFilePreviewScroll(short, { scrollHeight: 1500, clientHeight: 500 }), { top: 0, left: 0 });
});

run('file preview scroll sanitizes invalid metrics', () => {
  const snapshot = captureFilePreviewScroll({ scrollTop: Number.NaN, scrollHeight: Number.POSITIVE_INFINITY });
  assert.deepEqual(restoredFilePreviewScroll(snapshot), { top: 0, left: 0 });
});
