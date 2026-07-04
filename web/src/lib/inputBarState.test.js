import assert from 'node:assert/strict';
import { getInputBarActionState } from './inputBarState.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('idle 输入使用发送模式', () => {
  const state = getInputBarActionState({ value: 'hello', busy: false });
  assert.equal(state.mode, 'send');
  assert.equal(state.submitLabel, '发送');
  assert.equal(state.submitTitle, '发送 (Enter)');
  assert.equal(state.helperText, 'Enter 发送 · Shift+Enter 换行 · 上下键切换历史消息');
  assert.equal(state.canSubmit, true);
  assert.equal(state.canAbort, false);
});

run('busy 输入使用排队模式并保留中断能力', () => {
  const state = getInputBarActionState({ value: 'next step', busy: true });
  assert.equal(state.mode, 'queue');
  assert.equal(state.submitLabel, '排队');
  assert.equal(state.submitTitle, '排队下一条 (Enter)');
  assert.equal(state.helperText, 'Enter 排队 · Shift+Enter 换行 · 上下键切换历史消息');
  assert.equal(state.canSubmit, true);
  assert.equal(state.canAbort, true);
});

run('busy 空输入禁用排队但不禁用中断', () => {
  const state = getInputBarActionState({ value: '   ', busy: true });
  assert.equal(state.mode, 'queue');
  assert.equal(state.canSubmit, false);
  assert.equal(state.canAbort, true);
});

run('附件可在空文本时提交', () => {
  const state = getInputBarActionState({ value: '   ', hasExtras: true });
  assert.equal(state.canSubmit, true);
  assert.equal(state.hasExtras, true);
});

run('blocking disabled 状态禁止发送和排队', () => {
  assert.equal(getInputBarActionState({ value: 'hello', busy: false, disabled: true }).canSubmit, false);
  assert.equal(getInputBarActionState({ value: 'hello', busy: true, disabled: true }).canSubmit, false);
});
