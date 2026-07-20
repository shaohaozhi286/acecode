import assert from 'node:assert/strict';
import {
  desktopUpdateRestartAvailable,
  formatUpdatePublishedDate,
  normalizeUpdateReleases,
  requestDesktopUpdateRestart,
  updateDialogMode,
  updateJobIsActive,
  updateJobPhaseLabel,
  updateJobProgress,
  updateRestartMessage,
} from './updateJob.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

await test('active update jobs include pending and running only', () => {
  assert.equal(updateJobIsActive({ state: 'pending' }), true);
  assert.equal(updateJobIsActive({ state: 'running' }), true);
  assert.equal(updateJobIsActive({ state: 'failed' }), false);
});

await test('download progress occupies the visible middle of the full lifecycle', () => {
  assert.equal(updateJobProgress({ phase: 'checking', state: 'running' }), 5);
  assert.equal(updateJobProgress({ phase: 'downloading', state: 'running', percent: 50 }), 40);
  assert.equal(updateJobProgress({ phase: 'verifying', state: 'running' }), 78);
  assert.equal(updateJobProgress({ phase: 'installing', state: 'running' }), 95);
  assert.equal(updateJobProgress({ phase: 'complete', state: 'succeeded' }), 100);
});

await test('phase labels and terminal dialog modes are stable', () => {
  assert.equal(updateJobPhaseLabel({ phase: 'extracting', state: 'running' }), '正在解压安装包');
  assert.equal(updateJobPhaseLabel({ phase: 'verifying', state: 'failed' }), '升级失败');
  assert.equal(updateDialogMode(null), 'confirm');
  assert.equal(updateDialogMode(null, { status: 'available' }), 'confirm');
  assert.equal(updateDialogMode(null, { status: 'up_to_date' }), 'up_to_date');
  assert.equal(updateDialogMode({ state: 'running' }), 'running');
  assert.equal(updateDialogMode({ state: 'succeeded' }), 'success');
  assert.equal(updateDialogMode({ state: 'failed' }), 'failure');
});

await test('release history normalization keeps ordered non-empty plain-text notes', () => {
  assert.deepEqual(normalizeUpdateReleases(null), []);
  assert.deepEqual(normalizeUpdateReleases([
    {
      version: ' 0.8.0 ',
      published_at: ' 2026-07-20T08:00:00Z ',
      notes: ' First line\nSecond line ',
      packages: [{ sha256: 'must-not-leak' }],
    },
    { version: '0.7.9', notes: '   ' },
    { version: '', notes: 'missing version' },
    null,
    { version: '0.7.8', notes: 'Earlier tip' },
  ]), [
    {
      version: '0.8.0',
      published_at: '2026-07-20T08:00:00Z',
      notes: 'First line\nSecond line',
    },
    {
      version: '0.7.8',
      published_at: '',
      notes: 'Earlier tip',
    },
  ]);
});

await test('release publish dates use stable ISO dates and preserve unknown text', () => {
  assert.equal(formatUpdatePublishedDate('2026-07-20T08:00:00Z'), '2026-07-20');
  assert.equal(formatUpdatePublishedDate(' 2026-07-19 '), '2026-07-19');
  assert.equal(formatUpdatePublishedDate('legacy date'), 'legacy date');
  assert.equal(formatUpdatePublishedDate(null), '');
});

await test('successful job explains that the running window is still old', () => {
  const message = updateRestartMessage({ state: 'succeeded', restart_required: true });
  assert.match(message, /完全退出并重新启动/);
  assert.match(message, /当前窗口仍在运行旧版本/);
  assert.equal(updateRestartMessage({ state: 'failed' }), '');
});

await test('desktop restart capability requires the native lifecycle bridge', () => {
  assert.equal(desktopUpdateRestartAvailable({}), false);
  assert.equal(desktopUpdateRestartAvailable({ aceDesktop_restartApp: () => {} }), true);
});

await test('desktop restart helper accepts structured native success', async () => {
  const calls = [];
  const result = await requestDesktopUpdateRestart({
    aceDesktop_restartApp: async () => {
      calls.push('restart');
      return JSON.stringify({ ok: true });
    },
  });
  assert.deepEqual(calls, ['restart']);
  assert.deepEqual(result, { ok: true });
});

await test('desktop restart helper rejects unavailable and failed native restarts', async () => {
  await assert.rejects(
    () => requestDesktopUpdateRestart({}),
    /不支持自动重启/,
  );
  await assert.rejects(
    () => requestDesktopUpdateRestart({
      aceDesktop_restartApp: async () => ({ ok: false, error: 'replacement missing' }),
    }),
    /replacement missing/,
  );
});
