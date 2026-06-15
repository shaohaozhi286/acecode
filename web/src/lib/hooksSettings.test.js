import assert from 'node:assert/strict';
import {
  hookActionState,
  hookEmptyState,
  hookSettingsErrorMessage,
  hookStatusLabel,
  normalizeHookForDisplay,
  normalizeHookSnapshot,
} from './hooksSettings.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('empty hooks snapshot uses Codex-style empty state', () => {
  const normalized = normalizeHookSnapshot({ feature_enabled: true, hooks: [] });
  assert.equal(normalized.isEmpty, true);
  assert.equal(hookEmptyState(normalized).title, '未找到钩子');
});

run('disabled feature snapshot gets explicit empty state', () => {
  const normalized = normalizeHookSnapshot({ feature_enabled: false, hooks: [] });
  assert.equal(normalized.featureEnabled, false);
  assert.equal(hookEmptyState(normalized).title, '钩子已关闭');
});

run('pending review hook can be trusted and disabled', () => {
  const hook = normalizeHookForDisplay({
    id: 'h1',
    event_name: 'PreToolUse',
    matcher: '',
    command: 'echo hook',
    trust_status: 'pending_review',
  });
  assert.equal(hook.matcher, '*');
  assert.equal(hook.pendingReview, true);
  assert.equal(hookStatusLabel(hook), '待信任');
  assert.deepEqual(hookActionState(hook), {
    canTrust: true,
    canDisable: true,
    canEnable: false,
    showDisableControl: true,
  });
});

run('trusted hook shows trusted status without trust action', () => {
  const hook = normalizeHookForDisplay({
    id: 'h1',
    trusted: true,
    trust_status: 'trusted',
  });
  assert.equal(hookStatusLabel(hook), '已信任');
  assert.equal(hookActionState(hook).canTrust, false);
  assert.equal(hookActionState(hook).canDisable, true);
});

run('disabled hook can be re-enabled', () => {
  const hook = normalizeHookForDisplay({
    id: 'h1',
    disabled: true,
    trust_status: 'disabled',
  });
  assert.equal(hookStatusLabel(hook), '已禁用');
  assert.equal(hookActionState(hook).canDisable, false);
  assert.equal(hookActionState(hook).canEnable, true);
});

run('managed hook hides disable controls', () => {
  const hook = normalizeHookForDisplay({
    id: 'h1',
    managed: true,
    trust_status: 'managed_trusted',
  });
  assert.equal(hookStatusLabel(hook), '受管理');
  assert.deepEqual(hookActionState(hook), {
    canTrust: false,
    canDisable: false,
    canEnable: false,
    showDisableControl: false,
  });
});

run('diagnostics are normalized for UI rendering', () => {
  const normalized = normalizeHookSnapshot({
    hooks: [{
      id: 'h1',
      diagnostics: [{ severity: 'warning', code: 'SKIPPED_HANDLER', message: 'async unsupported' }],
    }],
    diagnostics: [{ severity: 'info', code: 'HOOK_SOURCE_MISSING', message: 'missing' }],
  });
  assert.equal(normalized.hooks[0].diagnostics[0].code, 'SKIPPED_HANDLER');
  assert.equal(normalized.diagnostics[0].message, 'missing');
});

run('hook settings error message uses Error.message fallback', () => {
  assert.equal(hookSettingsErrorMessage(new Error('refresh failed')), 'refresh failed');
  assert.equal(hookSettingsErrorMessage('plain failure'), 'plain failure');
});
