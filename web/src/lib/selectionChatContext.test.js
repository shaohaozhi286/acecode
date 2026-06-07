import assert from 'node:assert/strict';
import {
  createSelectionContext,
  formatSelectionContextLabel,
  formatSelectionContextNote,
  normalizeComposerContext,
  selectionContextLocationKey,
  selectionLineCount,
  truncateSelectionText,
} from './selectionChatContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('selection context formats file line ranges and line counts', () => {
  const ctx = createSelectionContext({
    id: 'sel1',
    text: 'first\nsecond',
    path: 'docs/README.md',
    kind: 'markdown',
    startLine: 23,
    endLine: 24,
  });
  assert.equal(formatSelectionContextLabel(ctx), 'README.md:23-24');
  assert.equal(formatSelectionContextNote(ctx), '2 行');
  assert.equal(ctx.source.path, 'docs/README.md');
  assert.equal(ctx.source.line_count, 2);
});

run('selection contexts keep text when normalized for composer payload', () => {
  const payload = normalizeComposerContext({
    type: 'selection',
    local_id: 'local-sel',
    text: 'const a = 1;',
    source: { path: 'src/a.js', start_line: 7, end_line: 7, line_count: 1 },
  });
  assert.equal(payload.type, 'selection');
  assert.equal(payload.id, 'local-sel');
  assert.equal(payload.label, 'a.js:7');
  assert.equal(payload.text, 'const a = 1;');
  assert.deepEqual(payload.source, {
    path: 'src/a.js',
    kind: '',
    line_count: 1,
    start_line: 7,
    end_line: 7,
  });
});

run('selection location keys match the same file range despite text changes', () => {
  const first = createSelectionContext({
    text: 'first selected text',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
  });
  const second = createSelectionContext({
    text: 'different selected text',
    path: 'src/a.js',
    startLine: 7,
    endLine: 7,
  });
  const otherLine = createSelectionContext({
    text: 'first selected text',
    path: 'src/a.js',
    startLine: 8,
    endLine: 8,
  });

  assert.equal(selectionContextLocationKey(first), selectionContextLocationKey(second));
  assert.notEqual(selectionContextLocationKey(first), selectionContextLocationKey(otherLine));
});

run('non-selection contexts keep the existing browser payload shape', () => {
  assert.deepEqual(normalizeComposerContext({ type: 'browser', label: 'Browser', note: 'Context' }), {
    type: 'browser',
    label: 'Browser',
    note: 'Context',
  });
});

run('selection text helpers count and truncate predictably', () => {
  assert.equal(selectionLineCount('a\r\nb\nc'), 3);
  assert.equal(truncateSelectionText('abcdef', 3), 'abc\n[Selection truncated]');
});
