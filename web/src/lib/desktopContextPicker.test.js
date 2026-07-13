import assert from 'node:assert/strict';
import {
  folderReferencePath,
  hasNativeContextPicker,
  nativeFolderReferencePath,
  nativePickedFileToFile,
  parseNativeContextPickerResult,
} from './desktopContextPicker.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

test('detects the Desktop unified context picker bridge', () => {
  assert.equal(hasNativeContextPicker({ aceDesktop_pickContextItems() {} }), true);
  assert.equal(hasNativeContextPicker({}), false);
});

test('parses file, folder, and cancelled native results', () => {
  assert.deepEqual(
    parseNativeContextPickerResult(JSON.stringify({
      ok: true,
      items: [{ kind: 'folder', path: 'C:/repo/docs' }],
    })),
    { cancelled: false, files: [], folder: { kind: 'folder', path: 'C:/repo/docs' } },
  );
  assert.equal(parseNativeContextPickerResult({ ok: true, cancelled: true, items: [] }).cancelled, true);
  assert.equal(parseNativeContextPickerResult({
    ok: true,
    items: [{ kind: 'file', name: 'a.txt', data_base64: 'YQ==' }],
  }).files.length, 1);
});

test('rejects mixed native file and folder results', () => {
  assert.throws(() => parseNativeContextPickerResult({
    ok: true,
    items: [
      { kind: 'folder', path: 'C:/repo/docs' },
      { kind: 'file', name: 'a.txt', data_base64: 'YQ==' },
    ],
  }), /冲突/);
});

test('restores native base64 bytes as a File-compatible object', () => {
  class FakeFile {
    constructor(parts, name, options) {
      this.parts = parts;
      this.name = name;
      this.type = options.type;
    }
  }
  const file = nativePickedFileToFile({
    kind: 'file',
    name: '图.png',
    mime_type: 'image/png',
    data_base64: 'AP8Q',
  }, {
    FileCtor: FakeFile,
    decodeBase64: (value) => Buffer.from(value, 'base64').toString('latin1'),
  });
  assert.equal(file.name, '图.png');
  assert.equal(file.type, 'image/png');
  assert.deepEqual([...file.parts[0]], [0, 255, 16]);
});

test('converts an in-cwd native folder to a relative reference path', () => {
  assert.equal(folderReferencePath('C:\\Repo', 'c:/repo/docs/设计 文档'), 'docs/设计 文档');
  assert.equal(folderReferencePath('C:/repo', 'C:/repo'), '');
  assert.equal(folderReferencePath('/home/me/repo', '/home/me/repo/docs'), 'docs');
});

test('keeps cwd-external and cwd-less folders as absolute references', () => {
  assert.equal(folderReferencePath('C:/repo', 'D:/shared/设计 文档'), 'D:/shared/设计 文档');
  assert.equal(folderReferencePath('C:/repo', 'C:/repo-other/docs'), 'C:/repo-other/docs');
  assert.equal(folderReferencePath('', 'C:/shared/docs'), 'C:/shared/docs');
  assert.equal(folderReferencePath('/repo', '/opt/shared/docs'), '/opt/shared/docs');
});

test('prefers the bridge canonical relative folder path', () => {
  assert.equal(nativeFolderReferencePath('N:/alias/repo', {
    path: 'C:/real/repo/docs',
    relative_path: 'docs',
  }), 'docs');
  assert.throws(() => nativeFolderReferencePath('C:/repo', {
    path: 'C:/outside',
    relative_path: '../outside',
  }), /相对路径无效/);
});

test('native folder references allow cwd prefix collisions and outside folders', () => {
  assert.equal(nativeFolderReferencePath('C:/repo', {
    path: 'C:/repo-other/docs',
  }), 'C:/repo-other/docs');
  assert.equal(nativeFolderReferencePath('/repo', {
    path: '/outside',
  }), '/outside');
});
