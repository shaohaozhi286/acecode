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
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState uses placeholder for continuation while avatar is hidden', () => {
  assert.deepEqual(assistantChromeState({ continuation: true }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState ignores enabled avatar requests and preserves the gutter', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: true,
    continuation: true,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState hides the name and preserves the gutter for first message', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: true,
    continuation: false,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('activityChromeState always hides waiting-state avatar', () => {
  assert.deepEqual(activityChromeState(true), {
    showAvatar: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});
