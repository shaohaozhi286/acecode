// web/src/lib/modelPicker.test.js
import assert from 'node:assert/strict';
import { isCurrentValueOrphaned, buildOptionsWithOrphan } from './modelPicker.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('当前值在列表里 → 不 orphan', () => {
  assert.equal(isCurrentValueOrphaned('a', [{ name: 'a' }, { name: 'b' }]), false);
});

run('当前值不在列表里 → orphan', () => {
  assert.equal(isCurrentValueOrphaned('ghost', [{ name: 'a' }]), true);
});

run('空 currentName → 不算 orphan(没有 selection 状态)', () => {
  assert.equal(isCurrentValueOrphaned('', [{ name: 'a' }]), false);
});

run('list 不是数组 → 不算 orphan(防御性)', () => {
  assert.equal(isCurrentValueOrphaned('x', null), false);
});

run('orphan 时插 disabled 灰条', () => {
  const out = buildOptionsWithOrphan('ghost', [{ name: 'a' }]);
  assert.equal(out.length, 2);
  assert.equal(out[0].orphan, true);
  assert.match(out[0].label, /已被改名/);
});

run('not orphan 时不插灰条,原列表不变', () => {
  const out = buildOptionsWithOrphan('a', [{ name: 'a' }, { name: 'b' }]);
  assert.equal(out.length, 2);
  assert.equal(out[0].name, 'a');
  assert.equal(out[1].name, 'b');
});
