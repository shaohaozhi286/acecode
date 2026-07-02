import assert from 'node:assert/strict';
import {
  defaultOpencodeImportSelection,
  normalizeOpencodeImportPreview,
  opencodeImportConfirmationText,
  opencodeImportProgress,
  toggleAllOpencodeImportSelection,
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
    sessions: [],
    count: 3,
  });
  assert.equal(normalizeOpencodeImportPreview({ available: true, count: 0 }).available, false);
});

test('preview normalizes archived session rows', () => {
  const preview = normalizeOpencodeImportPreview({
    available: true,
    sessions: [
      { id: 'ses-active', title: 'Active', message_count: '2' },
      { id: 'ses-archived', title: 'Archived', time_archived_ms: 1700000000000 },
    ],
  });

  assert.equal(preview.count, 2);
  assert.equal(preview.sessions[0].archived, false);
  assert.equal(preview.sessions[0].message_count, 2);
  assert.equal(preview.sessions[1].archived, true);
});

test('default selection includes only unarchived sessions', () => {
  const sessions = normalizeOpencodeImportPreview({
    available: true,
    sessions: [
      { id: 'ses-active', title: 'Active' },
      { id: 'ses-archived', title: 'Archived', archived: true },
    ],
  }).sessions;

  assert.deepEqual(defaultOpencodeImportSelection(sessions), ['ses-active']);
});

test('toggle all selects all then clears all', () => {
  const sessions = [
    { id: 'ses-a' },
    { id: 'ses-b' },
  ];

  assert.deepEqual(toggleAllOpencodeImportSelection(sessions, ['ses-a']), ['ses-a', 'ses-b']);
  assert.deepEqual(toggleAllOpencodeImportSelection(sessions, ['ses-a', 'ses-b']), []);
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
