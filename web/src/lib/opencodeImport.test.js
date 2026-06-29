import assert from 'node:assert/strict';
import {
  normalizeOpencodeImportPreview,
  opencodeImportConfirmationText,
  opencodeImportProgress,
} from './opencodeImport.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

test('confirmation copy includes import count', () => {
  assert.equal(opencodeImportConfirmationText(28), '即将导入 28 个会话，请确认');
});

test('preview normalizes availability by count', () => {
  assert.deepEqual(normalizeOpencodeImportPreview({ available: true, count: '3' }), {
    available: true,
    count: 3,
  });
  assert.equal(normalizeOpencodeImportPreview({ available: true, count: 0 }).available, false);
});

test('progress uses processed count over total', () => {
  assert.deepEqual(opencodeImportProgress({
    total: 10,
    imported: 3,
    failed: 1,
    skipped: 1,
  }), {
    total: 10,
    imported: 3,
    failed: 1,
    skipped: 1,
    processed: 5,
    percent: 50,
  });
});
