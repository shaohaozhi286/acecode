import assert from 'node:assert/strict';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from './permissionMode.js';

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
  assert.equal(normalizePermissionMode('yolo'), 'yolo');
});

run('permission mode exposes the three desktop permission options', () => {
  assert.deepEqual(
    PERMISSION_MODES.map(({ id, label }) => ({ id, label })),
    [
      { id: 'default', label: '默认权限' },
      { id: 'accept-edits', label: '自动接收编辑' },
      { id: 'yolo', label: '完全访问权限' },
    ],
  );
});

run('permission mode accepts legacy camelCase UI value', () => {
  assert.equal(normalizePermissionMode('acceptEdits'), 'accept-edits');
});

run('permission mode falls back to default for invalid values', () => {
  assert.equal(normalizePermissionMode(''), 'default');
  assert.equal(normalizePermissionMode('ask'), 'default');
  assert.equal(normalizePermissionMode('plan'), 'default');
});

run('permission mode option returns display metadata', () => {
  const plan = permissionModeOption('plan');
  assert.equal(plan.label, '默认权限');
  assert.equal(plan.color, 'ok');

  const option = permissionModeOption('yolo');
  assert.equal(option.label, '完全访问权限');
  assert.equal(option.color, 'danger');
});
