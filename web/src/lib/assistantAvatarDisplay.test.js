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

run('assistantChromeState shows avatar and name for first assistant message by default', () => {
  assert.deepEqual(assistantChromeState({ continuation: false }), {
    showAvatar: true,
    showName: true,
    showAvatarPlaceholder: false,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState uses placeholder for continuation when avatar display is enabled', () => {
  assert.deepEqual(assistantChromeState({ continuation: true }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState removes avatar chrome and placeholder when disabled', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: false,
    continuation: true,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: false,
    gapClass: 'gap-0',
  });
});

run('activityChromeState hides waiting-state avatar when disabled', () => {
  assert.deepEqual(activityChromeState(false), {
    showAvatar: false,
    gapClass: 'gap-0',
  });
});
