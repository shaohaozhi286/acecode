import assert from 'node:assert/strict';
import {
  buildStatusBarModelMenu,
  modelDisplayLabel,
  modelSelectValue,
  normalizeModelOptions,
  normalizeModelState,
  optionLabel,
  selectedModelName,
} from './sessionModel.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('footer model label uses session model name instead of placeholder', () => {
  const state = normalizeModelState({ name: 'gpt-5', provider: 'copilot', model: 'gpt-5' });
  assert.equal(modelDisplayLabel(state, '—'), 'gpt-5');
});

run('session switch refresh can display a different selected model', () => {
  const a = normalizeModelState({ name: 'gpt-5', provider: 'copilot', model: 'gpt-5' });
  const b = normalizeModelState({ name: 'gpt-5.4', provider: 'copilot', model: 'gpt-5.4' });
  assert.equal(modelDisplayLabel(a), 'gpt-5');
  assert.equal(modelDisplayLabel(b), 'gpt-5.4');
});

run('model options dedupe by preset name', () => {
  const options = normalizeModelOptions([
    { name: 'fast', provider: 'copilot', model: 'gpt-5' },
    { name: 'fast', provider: 'copilot', model: 'gpt-5' },
    { name: 'slow', provider: 'copilot', model: 'gpt-4o' },
  ]);
  assert.equal(options.length, 2);
  assert.equal(options[1].name, 'slow');
  assert.equal(optionLabel(options[0]), 'fast (copilot/gpt-5)');
});

run('pending select value rolls back to previous model when pending clears', () => {
  const previous = normalizeModelState({ name: 'slow', provider: 'copilot', model: 'slow-model' });
  assert.equal(selectedModelName(previous), 'slow');
  assert.equal(modelSelectValue(previous, 'fast'), 'fast');
  assert.equal(modelSelectValue(previous, ''), 'slow');
});

run('status bar model menu marks selected option active', () => {
  const menu = buildStatusBarModelMenu({
    selectedModelName: 'fast',
    modelOptions: [
      { name: 'fast', provider: 'openai', model: 'gpt-5' },
      { name: 'slow', provider: 'copilot', model: 'gpt-4o' },
    ],
    fallbackLabel: '加载中',
  });
  assert.equal(menu.displayLabel, 'fast (openai/gpt-5)');
  assert.deepEqual(menu.items.map((item) => [item.name, item.active]), [
    ['fast', true],
    ['slow', false],
  ]);
  assert.equal(menu.widthLabel, 'slow (copilot/gpt-4o)');
});

run('status bar model menu uses fallback when no selection exists', () => {
  const menu = buildStatusBarModelMenu({
    selectedModelName: '',
    modelOptions: [{ name: 'fast', provider: 'openai', model: 'gpt-5' }],
    fallbackLabel: '加载中',
  });
  assert.equal(menu.displayLabel, '加载中');
  assert.equal(menu.items[0].active, false);
});

run('status bar model menu displays missing selected name without fake option', () => {
  const menu = buildStatusBarModelMenu({
    selectedModelName: 'manual-model',
    modelOptions: [{ name: 'fast', provider: 'openai', model: 'gpt-5' }],
    fallbackLabel: '加载中',
  });
  assert.equal(menu.displayLabel, 'manual-model');
  assert.deepEqual(menu.items.map((item) => item.name), ['fast']);
  assert.equal(menu.items.some((item) => item.active), false);
});

run('status bar model menu exposes longest label for intrinsic sizing', () => {
  const longLabel = 'very-long-model-profile-name-for-desktop-status-bar (openai/provider/model-with-a-very-long-routing-name)';
  const menu = buildStatusBarModelMenu({
    selectedModelName: 'short',
    modelOptions: [
      { name: 'short', provider: 'openai', model: 'gpt-5' },
      {
        name: 'very-long-model-profile-name-for-desktop-status-bar',
        provider: 'openai',
        model: 'provider/model-with-a-very-long-routing-name',
      },
    ],
    fallbackLabel: '加载中',
  });

  assert.equal(menu.widthLabel, longLabel);
});
