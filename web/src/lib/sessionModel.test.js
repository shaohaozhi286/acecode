import assert from 'node:assert/strict';
import {
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

run('model options dedupe by preset name and keep legacy selectable', () => {
  const options = normalizeModelOptions([
    { name: 'fast', provider: 'copilot', model: 'gpt-5' },
    { name: 'fast', provider: 'copilot', model: 'gpt-5' },
    { name: '(legacy)', provider: 'copilot', model: 'gpt-4o', is_legacy: true },
  ]);
  assert.equal(options.length, 2);
  assert.equal(options[1].name, '(legacy)');
  assert.equal(options[1].isLegacy, true);
  assert.equal(optionLabel(options[0]), 'fast (copilot/gpt-5)');
});

run('pending select value rolls back to previous model when pending clears', () => {
  const previous = normalizeModelState({ name: 'slow', provider: 'copilot', model: 'slow-model' });
  assert.equal(selectedModelName(previous), 'slow');
  assert.equal(modelSelectValue(previous, 'fast'), 'fast');
  assert.equal(modelSelectValue(previous, ''), 'slow');
});
