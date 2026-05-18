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

run('assistantChromeState hides the avatar but preserves the assistant gutter when disabled', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: false,
    continuation: true,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('assistantChromeState hides the name but preserves the gutter for first message when avatar is disabled', () => {
  assert.deepEqual(assistantChromeState({
    showAceCodeAvatar: false,
    continuation: false,
  }), {
    showAvatar: false,
    showName: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});

run('activityChromeState hides waiting-state avatar when disabled', () => {
  assert.deepEqual(activityChromeState(false), {
    showAvatar: false,
    showAvatarPlaceholder: true,
    gapClass: 'gap-2',
  });
});
