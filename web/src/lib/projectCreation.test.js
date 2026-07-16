import assert from 'node:assert/strict';
import {
  normalizeProjectDirectoryName,
  projectCreationErrorMessage,
  projectPathPreview,
} from './projectCreation.js';

function test(name, fn) {
  try {
    fn();
    console.log(`  ✓ ${name}`);
  } catch (error) {
    console.error(`  ✗ ${name}`);
    throw error;
  }
}

test('normalizes cross-platform incompatible characters like the daemon', () => {
  assert.deepEqual(normalizeProjectDirectoryName('  my/api:test?  '), {
    directoryName: 'my-api-test-',
    changed: true,
  });
});
test('adjusts Windows reserved names including extension variants', () => {
  assert.equal(normalizeProjectDirectoryName('CON').directoryName, 'CON-project');
  assert.equal(normalizeProjectDirectoryName('lpt9.txt').directoryName, 'lpt9.txt-project');
  assert.equal(normalizeProjectDirectoryName('COM0').directoryName, 'COM0');
});

test('trims trailing dots and falls back for dot-only names', () => {
  assert.equal(normalizeProjectDirectoryName('demo...  ').directoryName, 'demo');
  assert.equal(normalizeProjectDirectoryName('...').directoryName, 'project');
});

test('truncates at 60 Unicode code points without splitting characters', () => {
  assert.equal(
    normalizeProjectDirectoryName('项'.repeat(61)).directoryName,
    '项'.repeat(60),
  );
});

test('previews Windows and POSIX project paths', () => {
  assert.equal(projectPathPreview('C:\\Users\\me\\ACECode', 'demo'),
    'C:\\Users\\me\\ACECode\\demo');
  assert.equal(projectPathPreview('/home/me/acecode/', 'demo'), '/home/me/acecode/demo');
});

test('prefers structured API error messages', () => {
  assert.equal(projectCreationErrorMessage({
    message: 'HTTP 409: fallback',
    body: { message: '目标目录已存在' },
  }), '目标目录已存在');
});
