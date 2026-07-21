import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const messageSource = fs.readFileSync(
  path.join(srcRoot, 'components/Message.jsx'),
  'utf8',
);

assert.match(messageSource, /metadata\?\.compact_notice === true/);
assert.match(messageSource, /metadata\?\.compact_notice_complete === true/);
assert.match(
  messageSource,
  /useState\(\s*isCompactNotice && !isCompletedCompactNotice,?\s*\)/,
);
assert.match(messageSource, /'Context compacted'/);
assert.match(messageSource, /onClick=\{\(\) => setExpanded\(\(v\) => !v\)\}/);
assert.match(messageSource, /\{expanded && \(/);

console.log('compactNoticeArchitecture tests passed');
