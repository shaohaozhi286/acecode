import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('shared Modal appears and closes without staged transitions or delay', () => {
  const modalFile = source('components/Modal.jsx');
  const modal = between(
    modalFile,
    'export function Modal',
    '// 右侧滑出面板',
  );

  assert.doesNotMatch(modal, /requestAnimationFrame/);
  assert.doesNotMatch(modal, /setTimeout/);
  assert.doesNotMatch(modal, /transition-/);
  assert.doesNotMatch(modal, /duration-/);
  assert.doesNotMatch(modal, /\bscale-/);
  assert.doesNotMatch(modal, /\btranslate-/);
  assert.match(modal, /const handleClose = \(\) => onClose\?\.\(\);/);
  assert.match(modal, /backgroundColor: 'rgba\(0, 0, 0, 0\.35\)'/);
});

run('archived deletion and close-preview confirmations share Modal', () => {
  const settingsFile = source('components/SettingsPage.jsx');
  const archived = between(
    settingsFile,
    'function SectionArchived()',
    '// ─── 使用情况',
  );
  const chatView = source('components/ChatView.jsx');

  assert.match(archived, /<Modal onClose=\{\(\) => setPurgeConfirmation\(null\)\}/);
  assert.match(archived, />\s*彻底删除\s*</);
  assert.doesNotMatch(archived, /window\.confirm|window\.alert/);
  assert.match(chatView, /<Modal onClose=\{\(\) => setPreviewCloseConfirm\(null\)\}/);
  assert.match(chatView, />关闭预览面板</);
});

run('update dialog keeps release history bounded and renders notes as plain text', () => {
  const updateDialog = source('components/UpdateDialog.jsx');

  assert.match(updateDialog, /updateDialogMode\(job, updateStatus\)/);
  assert.match(updateDialog, /aria-label="版本更新记录"/);
  assert.match(updateDialog, /max-h-64 overflow-y-auto/);
  assert.match(updateDialog, /whitespace-pre-wrap break-words/);
  assert.match(updateDialog, /mode === 'up_to_date'/);
  assert.doesNotMatch(updateDialog, /dangerouslySetInnerHTML/);
});
