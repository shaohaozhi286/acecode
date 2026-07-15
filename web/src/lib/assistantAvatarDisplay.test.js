import assert from 'node:assert/strict';
import {
  activityChromeState,
  assistantChromeState,
} from './assistantAvatarDisplay.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('assistantChromeState hides avatar and name for first assistant message by default', () => {
  assert.deepEqual(assistantChromeState({ continuation: false }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});

run('assistantChromeState keeps continuation flush left while avatar is hidden', () => {
  assert.deepEqual(assistantChromeState({ continuation: true }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});

run('assistantChromeState ignores enabled avatar requests without reserving a gutter', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: true,
    continuation: true,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});

run('assistantChromeState hides the name without reserving a gutter for first message', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: true,
    continuation: false,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});

run('activityChromeState always hides waiting-state avatar', () => {
  assert.deepEqual(activityChromeState(true), {
    showAvatar: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});
