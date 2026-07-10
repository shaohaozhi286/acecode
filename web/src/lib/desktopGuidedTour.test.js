import assert from 'node:assert/strict';
import {
  DESKTOP_GUIDED_TOUR_TARGET_LIST,
  buildDesktopGuidedTourSteps,
  desktopGuidedTourHasModel,
  desktopGuidedTourLostRequiredTarget,
  desktopGuidedTourModeEligible,
  desktopGuidedTourTargetsReady,
  desktopGuidedTourTerminalAction,
  shouldAutoStartDesktopGuidedTour,
  shouldPrepareDesktopGuidedTour,
} from './desktopGuidedTour.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Desktop tour auto-start requires an eligible settled Desktop home', () => {
  const ready = {
    mode: 'shell',
    authState: 'ok',
    stateLoaded: true,
    dismissed: false,
    startupNavigationSettled: true,
    hasActiveSession: false,
    blocked: false,
    targetsReady: true,
    attempted: false,
  };
  assert.equal(shouldAutoStartDesktopGuidedTour(ready), true);
  for (const [key, value] of [
    ['mode', 'browser'],
    ['authState', 'checking'],
    ['stateLoaded', false],
    ['dismissed', true],
    ['startupNavigationSettled', false],
    ['hasActiveSession', true],
    ['blocked', true],
    ['targetsReady', false],
    ['attempted', true],
  ]) {
    assert.equal(shouldAutoStartDesktopGuidedTour({ ...ready, [key]: value }), false, key);
  }
  assert.equal(shouldAutoStartDesktopGuidedTour({ ...ready, mode: 'webapp' }), true);
});

run('Desktop tour mode and target helpers reject browser and missing targets', () => {
  assert.equal(desktopGuidedTourModeEligible('shell'), true);
  assert.equal(desktopGuidedTourModeEligible('webapp'), true);
  assert.equal(desktopGuidedTourModeEligible('browser'), false);
  const present = new Set(DESKTOP_GUIDED_TOUR_TARGET_LIST);
  assert.equal(desktopGuidedTourTargetsReady({ querySelector: (selector) => present.has(selector) ? {} : null }), true);
  present.delete(DESKTOP_GUIDED_TOUR_TARGET_LIST[2]);
  assert.equal(desktopGuidedTourTargetsReady({ querySelector: (selector) => present.has(selector) ? {} : null }), false);
});

run('Desktop tour builds seven ordered stable steps, distinguishes project entry points, and adapts no-model copy', () => {
  const configured = buildDesktopGuidedTourSteps({ hasModel: true });
  const missing = buildDesktopGuidedTourSteps({ hasModel: false });
  assert.equal(configured.length, 7);
  assert.deepEqual(configured.map((step) => step.id), [
    'sidebar',
    'add-project',
    'new-session',
    'workspace',
    'composer',
    'status',
    'settings',
  ]);
  assert.deepEqual(configured.map((step) => step.target), DESKTOP_GUIDED_TOUR_TARGET_LIST);
  assert.match(configured[1].content, /本地代码目录/);
  assert.match(configured[2].content, /不会创建项目目录/);
  assert.match(missing[5].title, /配置.*模型/);
  assert.match(missing[6].content, /添加模型/);
});

run('Desktop tour terminal actions dismiss and only no-model completion routes to Models', () => {
  assert.deepEqual(
    desktopGuidedTourTerminalAction({ type: 'tour:end', status: 'finished' }, { hasModel: false }),
    { dismiss: true, openModels: true },
  );
  assert.deepEqual(
    desktopGuidedTourTerminalAction({ type: 'tour:end', status: 'finished' }, { hasModel: true }),
    { dismiss: true, openModels: false },
  );
  assert.deepEqual(
    desktopGuidedTourTerminalAction({ type: 'tour:end', status: 'skipped' }, { hasModel: false }),
    { dismiss: true, openModels: false },
  );
  assert.equal(desktopGuidedTourTerminalAction({ type: 'step:after', status: 'running' }), null);
});

run('Desktop tour detects model availability and required target loss', () => {
  assert.equal(desktopGuidedTourHasModel([]), false);
  assert.equal(desktopGuidedTourHasModel([{ name: 'model' }]), true);
  assert.equal(desktopGuidedTourHasModel(null), false);
  assert.equal(desktopGuidedTourLostRequiredTarget({ type: 'error:target_not_found' }), true);
  assert.equal(desktopGuidedTourLostRequiredTarget({ type: 'tour:end' }), false);
});

run('Desktop tour forced replay ignores dismissal but still waits for application blockers', () => {
  const ready = {
    mode: 'shell',
    authState: 'ok',
    startupNavigationSettled: true,
    hasActiveSession: false,
    blocked: false,
  };
  assert.equal(shouldPrepareDesktopGuidedTour(ready), true);
  assert.equal(shouldPrepareDesktopGuidedTour({ ...ready, blocked: true }), false);
  assert.equal(shouldPrepareDesktopGuidedTour({ ...ready, hasActiveSession: true }), false);
  assert.equal(shouldPrepareDesktopGuidedTour({ ...ready, mode: 'browser' }), false);
});
