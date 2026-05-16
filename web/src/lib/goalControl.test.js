import assert from 'node:assert/strict';
import { getGoalStopControlState, isActiveGoal } from './goalControl.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('active goal is stoppable even when not busy', () => {
  const state = getGoalStopControlState({ goal: { status: 'active' }, busy: false });
  assert.equal(isActiveGoal({ status: 'active' }), true);
  assert.equal(state.visible, true);
  assert.equal(state.action, 'pause_goal');
  assert.equal(state.label, '停止 Goal');
});

run('busy non-goal turn still shows abort control', () => {
  const state = getGoalStopControlState({ goal: null, busy: true });
  assert.equal(state.visible, true);
  assert.equal(state.action, 'abort');
  assert.equal(state.label, '中断');
});

run('busy active goal makes the stop action an abort', () => {
  const state = getGoalStopControlState({ goal: { status: 'active' }, busy: true });
  assert.equal(state.visible, true);
  assert.equal(state.action, 'abort');
  assert.equal(state.label, '中断 Goal');
  assert.match(state.title, /暂停 Goal/);
});

run('paused goal without a running turn hides stop control', () => {
  const state = getGoalStopControlState({ goal: { status: 'paused' }, busy: false });
  assert.equal(isActiveGoal({ status: 'paused' }), false);
  assert.equal(state.visible, false);
  assert.equal(state.action, 'none');
});
