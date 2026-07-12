import assert from 'node:assert/strict';
import {
  updateDialogMode,
  updateJobIsActive,
  updateJobPhaseLabel,
  updateJobProgress,
  updateRestartMessage,
} from './updateJob.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

test('active update jobs include pending and running only', () => {
  assert.equal(updateJobIsActive({ state: 'pending' }), true);
  assert.equal(updateJobIsActive({ state: 'running' }), true);
  assert.equal(updateJobIsActive({ state: 'failed' }), false);
});

test('download progress occupies the visible middle of the full lifecycle', () => {
  assert.equal(updateJobProgress({ phase: 'checking', state: 'running' }), 5);
  assert.equal(updateJobProgress({ phase: 'downloading', state: 'running', percent: 50 }), 40);
  assert.equal(updateJobProgress({ phase: 'verifying', state: 'running' }), 78);
  assert.equal(updateJobProgress({ phase: 'installing', state: 'running' }), 95);
  assert.equal(updateJobProgress({ phase: 'complete', state: 'succeeded' }), 100);
});

test('phase labels and terminal dialog modes are stable', () => {
  assert.equal(updateJobPhaseLabel({ phase: 'extracting', state: 'running' }), '正在解压安装包');
  assert.equal(updateJobPhaseLabel({ phase: 'verifying', state: 'failed' }), '升级失败');
  assert.equal(updateDialogMode(null), 'confirm');
  assert.equal(updateDialogMode({ state: 'running' }), 'running');
  assert.equal(updateDialogMode({ state: 'succeeded' }), 'success');
  assert.equal(updateDialogMode({ state: 'failed' }), 'failure');
});

test('successful job explains that the running window is still old', () => {
  const message = updateRestartMessage({ state: 'succeeded', restart_required: true });
  assert.match(message, /完全退出并重新启动/);
  assert.match(message, /当前窗口仍在运行旧版本/);
  assert.equal(updateRestartMessage({ state: 'failed' }), '');
});
