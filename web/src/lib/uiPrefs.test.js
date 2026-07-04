import assert from 'node:assert/strict';
import {
  DEFAULT_FONT_SIZE,
  DEFAULT_UI_PREFS,
  effectiveFontSize,
  effectiveShowAceCodeAvatar,
  FONT_SIZE_VALUES,
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

run('DEFAULT_UI_PREFS keeps ACECode avatar hidden by default', () => {
  assert.equal(DEFAULT_UI_PREFS.showAceCodeAvatar, false);
  assert.equal(effectiveShowAceCodeAvatar(DEFAULT_UI_PREFS), false);
});

run('DEFAULT_UI_PREFS uses medium font size by default', () => {
  assert.equal(DEFAULT_FONT_SIZE, 'medium');
  assert.equal(DEFAULT_UI_PREFS.fontSize, 'medium');
  assert.equal(effectiveFontSize(DEFAULT_UI_PREFS), 'medium');
});

run('validateUiPrefs accepts old objects without showAceCodeAvatar', () => {
  assert.equal(validateUiPrefs({
    view: 'single',
    sidePanelCollapsed: false,
    sidebarCollapsed: true,
    sidePanelMaximized: false,
  }), true);
});

run('validateUiPrefs accepts legacy objects without fontSize', () => {
  assert.equal(validateUiPrefs({
    view: 'single',
    sidePanelCollapsed: false,
    sidebarCollapsed: true,
    sidePanelMaximized: false,
    showAceCodeAvatar: false,
  }), true);
});

run('validateUiPrefs accepts small, medium, and large font sizes', () => {
  for (const fontSize of FONT_SIZE_VALUES) {
    assert.equal(validateUiPrefs({
      ...DEFAULT_UI_PREFS,
      fontSize,
    }), true);
  }
});

run('validateUiPrefs rejects invalid fontSize values', () => {
  assert.equal(validateUiPrefs({
    ...DEFAULT_UI_PREFS,
    fontSize: 'huge',
  }), false);
});

run('effectiveFontSize returns medium for missing and invalid values', () => {
  assert.equal(effectiveFontSize({
    view: 'single',
    sidePanelCollapsed: false,
  }), 'medium');
  assert.equal(effectiveFontSize({
    ...DEFAULT_UI_PREFS,
    fontSize: 'huge',
  }), 'medium');
});

run('effectiveShowAceCodeAvatar treats missing value as hidden', () => {
  assert.equal(effectiveShowAceCodeAvatar({
    view: 'single',
    sidePanelCollapsed: false,
  }), false);
});

run('effectiveShowAceCodeAvatar ignores explicit true and stays hidden', () => {
  assert.equal(effectiveShowAceCodeAvatar({
    ...DEFAULT_UI_PREFS,
    showAceCodeAvatar: true,
  }), false);
});

run('validateUiPrefs rejects non-boolean showAceCodeAvatar', () => {
  assert.equal(validateUiPrefs({
    ...DEFAULT_UI_PREFS,
    showAceCodeAvatar: 'false',
  }), false);
});
