import assert from 'node:assert/strict';
import {
  DEFAULT_UI_PREFS,
  effectiveShowAceCodeAvatar,
  validateUiPrefs,
} from './uiPrefs.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('DEFAULT_UI_PREFS keeps ACECode avatar visible by default', () => {
  assert.equal(DEFAULT_UI_PREFS.showAceCodeAvatar, true);
  assert.equal(effectiveShowAceCodeAvatar(DEFAULT_UI_PREFS), true);
});

run('validateUiPrefs accepts old objects without showAceCodeAvatar', () => {
  assert.equal(validateUiPrefs({
    view: 'single',
    sidePanelCollapsed: false,
    sidebarCollapsed: true,
    sidePanelMaximized: false,
  }), true);
});

run('effectiveShowAceCodeAvatar treats missing value as visible', () => {
  assert.equal(effectiveShowAceCodeAvatar({
    view: 'single',
    sidePanelCollapsed: false,
  }), true);
});

run('effectiveShowAceCodeAvatar honors explicit false', () => {
  assert.equal(effectiveShowAceCodeAvatar({
    ...DEFAULT_UI_PREFS,
    showAceCodeAvatar: false,
  }), false);
});

run('validateUiPrefs rejects non-boolean showAceCodeAvatar', () => {
  assert.equal(validateUiPrefs({
    ...DEFAULT_UI_PREFS,
    showAceCodeAvatar: 'false',
  }), false);
});
