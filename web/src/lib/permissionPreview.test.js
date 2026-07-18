import assert from 'node:assert/strict';
import {
  PERMISSION_PREVIEW_LIMITS,
  buildPermissionPreview,
  compactPermissionValue,
  formatPermissionArgsPreview,
  truncatePermissionText,
} from './permissionPreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('long command preview is bounded and reports truncation', () => {
  const command = 'x'.repeat(PERMISSION_PREVIEW_LIMITS.previewChars + 17);
  const preview = buildPermissionPreview({
    tool: 'bash',
    args: { command },
  });
  assert.equal(preview.tool.toolLabel, 'Bash');
  assert.equal(preview.tool.kind, 'command');
  assert.equal(preview.truncated, 17);
  assert.match(preview.text, /已截断 17 个字符/);
});

run('file preview keeps compact path and line summary without content dump', () => {
  const preview = buildPermissionPreview({
    tool: 'file_write',
    args: {
      file_path: 'C:/repo/secret.txt',
      content: 'private-line-one\nprivate-line-two',
    },
  });
  assert.equal(preview.tool.kind, 'file');
  assert.equal(preview.tool.filePath, 'C:/repo/secret.txt');
  assert.equal(preview.tool.detail, '写入 2 行');
  assert.equal(preview.text, '');
});

run('fallback JSON bounds long strings, arrays, keys, and depth', () => {
  const compact = compactPermissionValue({
    long: 'a'.repeat(PERMISSION_PREVIEW_LIMITS.stringChars + 20),
    deep: { one: { two: { three: { four: 'hidden' } } } },
    items: Array.from({ length: PERMISSION_PREVIEW_LIMITS.arrayItems + 3 }, (_, i) => i),
    ...Object.fromEntries(Array.from(
      { length: PERMISSION_PREVIEW_LIMITS.objectKeys + 3 },
      (_, i) => [`key_${i}`, i],
    )),
  });
  const text = JSON.stringify(compact);
  assert.match(text, /已截断 20 个字符/);
  assert.match(text, /还有 3 项/);
  assert.match(text, /还有 \d+ 个字段/);
  assert.match(text, /\{\.\.\.\}/);
});

run('string fallback and generic args are both safe to format', () => {
  assert.equal(formatPermissionArgsPreview('plain text').text, 'plain text');
  assert.equal(truncatePermissionText(null).text, '');
  assert.doesNotThrow(() => formatPermissionArgsPreview({ circular: null }));
});

console.log('permissionPreview tests passed');
