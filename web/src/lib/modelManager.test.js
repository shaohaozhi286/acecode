// web/src/lib/modelManager.test.js
import assert from 'node:assert/strict';
import { validateModelDraft } from './modelManager.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('空名字 → INVALID_NAME', () => {
  assert.equal(validateModelDraft({ name: '', provider: 'copilot', model: 'gpt-4o' }).code,
                'INVALID_NAME');
});

run('保留前缀 → RESERVED_NAME', () => {
  assert.equal(validateModelDraft({ name: '(x)', provider: 'copilot', model: 'gpt-4o' }).code,
                'RESERVED_NAME');
});

run('未知 provider → UNKNOWN_PROVIDER', () => {
  assert.equal(validateModelDraft({ name: 'a', provider: 'anthropic', model: 'x' }).code,
                'UNKNOWN_PROVIDER');
});

run('OpenAI 缺 model → MISSING_MODEL', () => {
  assert.equal(validateModelDraft({ name: 'lm', provider: 'openai' }).code,
                'MISSING_MODEL');
});

run('OpenAI 缺 base_url → MISSING_BASE_URL', () => {
  assert.equal(validateModelDraft({
    name: 'lm', provider: 'openai', model: 'l',
  }).code, 'MISSING_BASE_URL');
});

run('OpenAI 缺 api_key → INVALID_API_KEY', () => {
  assert.equal(validateModelDraft({
    name: 'lm', provider: 'openai', model: 'l', base_url: 'http://x',
  }).code, 'INVALID_API_KEY');
});

run('合法 copilot 草稿 → ok', () => {
  assert.deepEqual(validateModelDraft({
    name: 'cp', provider: 'copilot', model: 'gpt-4o',
  }), { ok: true });
});

run('合法 openai 草稿 → ok', () => {
  assert.deepEqual(validateModelDraft({
    name: 'lm', provider: 'openai', model: 'l',
    base_url: 'http://x', api_key: 'sk-x',
  }), { ok: true });
});
