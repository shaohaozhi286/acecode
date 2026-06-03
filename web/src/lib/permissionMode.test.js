import assert from 'node:assert/strict';
import { normalizePermissionMode, permissionModeOption } from './permissionMode.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('permission mode normalizes daemon canonical names', () => {
  assert.equal(normalizePermissionMode('default'), 'default');
  assert.equal(normalizePermissionMode('accept-edits'), 'accept-edits');
  assert.equal(normalizePermissionMode('plan'), 'plan');
  assert.equal(normalizePermissionMode('yolo'), 'yolo');
});

run('permission mode accepts legacy camelCase UI value', () => {
  assert.equal(normalizePermissionMode('acceptEdits'), 'accept-edits');
});

run('permission mode falls back to default for invalid values', () => {
  assert.equal(normalizePermissionMode(''), 'default');
  assert.equal(normalizePermissionMode('ask'), 'default');
});

run('permission mode option returns display metadata', () => {
  const plan = permissionModeOption('plan');
  assert.equal(plan.label, 'Plan');
  assert.equal(plan.color, 'plan');

  const option = permissionModeOption('yolo');
  assert.equal(option.label, 'Yolo');
  assert.equal(option.color, 'danger');
});
