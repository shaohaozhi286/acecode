import assert from 'node:assert/strict';
import {
  PATH_REFERENCE_LIMIT,
  formatPathReference,
  insertPathReferenceAtCaret,
  normalizePathReferenceCandidates,
  pathReferenceTokenAtCursor,
  replacePathReferenceToken,
  splitPathReferenceQuery,
  unsafeReferencePath,
} from './pathReference.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

test('extracts a complete token around a middle caret', () => {
  const text = 'please @src/ma continue';
  const token = pathReferenceTokenAtCursor(text, 12);
  assert.deepEqual(token, { begin: 7, end: 14, path: 'src/ma', quoted: false });
  assert.deepEqual(
    replacePathReferenceToken(text, token, 'src/main.cpp'),
    { text: 'please @src/main.cpp  continue', cursor: 21 },
  );
});

test('quoted Unicode path keeps whitespace', () => {
  const text = '看 @"文档/设计 草案" 后继续';
  const token = pathReferenceTokenAtCursor(text, text.indexOf('草案') + 2);
  assert.equal(token.path, '文档/设计 草案');
  assert.equal(token.quoted, true);
  assert.equal(formatPathReference('文档/设计 草案.md'), '@"文档/设计 草案.md" ');
});

test('email and non-boundary at signs do not trigger', () => {
  assert.equal(pathReferenceTokenAtCursor('user@example.com', 10), null);
  assert.equal(pathReferenceTokenAtCursor('x@src', 5), null);
});

test('splits Windows and forward slash queries', () => {
  assert.deepEqual(splitPathReferenceQuery('src\\deep\\ma'), { directory: 'src/deep', filter: 'ma' });
  assert.deepEqual(splitPathReferenceQuery('src/deep/'), { directory: 'src/deep', filter: '' });
});

test('directory entry and direct reference use distinct completion', () => {
  const token = pathReferenceTokenAtCursor('@sr', 3);
  assert.deepEqual(
    replacePathReferenceToken('@sr', token, 'src', { directory: true, enterDirectory: true }),
    { text: '@src/', cursor: 5 },
  );
  assert.equal(formatPathReference('src', { directory: true }), '@src/ ');
});

test('folder picker inserts at the saved caret', () => {
  assert.deepEqual(
    insertPathReferenceAtCaret('请处理  后继续', 4, '设计 文档'),
    { text: '请处理 @"设计 文档/"  后继续', cursor: 14 },
  );
});

test('explicit cwd-external folders keep absolute paths', () => {
  assert.deepEqual(
    insertPathReferenceAtCaret('处理', 2, 'D:/共享 目录'),
    { text: '处理 @"D:/共享 目录/" ', cursor: 16 },
  );
  assert.equal(formatPathReference('/opt/shared', { directory: true }), '@/opt/shared/ ');
  assert.equal(formatPathReference('/', { directory: true }), '@/ ');
});

test('candidate normalization is directory first, filtered, and capped', () => {
  const entries = [
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'file' },
    { name: 'Models', path: 'src/Models', kind: 'dir' },
    ...Array.from({ length: 70 }, (_, i) => ({ name: `more-${i}`, path: `more-${i}`, kind: 'file' })),
  ];
  const all = normalizePathReferenceCandidates(entries, '');
  assert.equal(all[0].kind, 'dir');
  assert.equal(all.length, PATH_REFERENCE_LIMIT);
  assert.deepEqual(normalizePathReferenceCandidates(entries, 'MAIN').map((x) => x.name), ['main.cpp']);
  assert.deepEqual(normalizePathReferenceCandidates(entries, '', { foldersOnly: true }).map((x) => x.name), ['Models']);
});

test('rejects traversal and absolute paths', () => {
  assert.equal(unsafeReferencePath('../secret'), true);
  assert.equal(unsafeReferencePath('/etc'), true);
  assert.equal(unsafeReferencePath('C:\\Windows'), true);
  assert.equal(unsafeReferencePath('src/main.cpp'), false);
});
