import assert from 'node:assert/strict';
import {
  extensionForPath,
  filePreviewKind,
  isBlobFilePreview,
} from './filePreviewKind.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

run('extensionForPath handles nested paths and uppercase extensions', () => {
  assert.equal(extensionForPath('docs/Manual.PDF'), 'pdf');
  assert.equal(extensionForPath('C:\\repo\\README.MD'), 'md');
  assert.equal(extensionForPath('Makefile'), '');
});

run('filePreviewKind routes browser-native binary previews before text', () => {
  assert.equal(filePreviewKind('docs/manual.pdf'), 'pdf');
  assert.equal(filePreviewKind('assets/logo.svg'), 'image');
  assert.equal(filePreviewKind('docs/spec.DOCX'), 'word');
  assert.equal(filePreviewKind('docs/report.xlsx'), 'spreadsheet');
  assert.equal(filePreviewKind('docs/macro.xlsm'), 'spreadsheet');
  assert.equal(filePreviewKind('README.markdown'), 'markdown');
  assert.equal(filePreviewKind('src/main.cpp'), 'text');
  assert.equal(filePreviewKind('docs/legacy.doc'), 'unsupported');
  assert.equal(filePreviewKind('docs/legacy.xls'), 'unsupported');
});

run('isBlobFilePreview includes browser-rendered binary previews only', () => {
  assert.equal(isBlobFilePreview('docs/manual.pdf'), true);
  assert.equal(isBlobFilePreview('assets/photo.jpeg'), true);
  assert.equal(isBlobFilePreview('docs/spec.docx'), true);
  assert.equal(isBlobFilePreview('docs/report.xlsx'), true);
  assert.equal(isBlobFilePreview('docs/macro.xlsm'), true);
  assert.equal(isBlobFilePreview('README.md'), false);
  assert.equal(isBlobFilePreview('docs/legacy.doc'), false);
  assert.equal(isBlobFilePreview('docs/legacy.xls'), false);
  assert.equal(isBlobFilePreview('archive.zip'), false);
});
