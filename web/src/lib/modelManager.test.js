// web/src/lib/modelManager.test.js
import assert from 'node:assert/strict';
import {
  buildModelDraftsFromSelection,
  modelNameSlug,
  splitModelIds,
  validateModelDraft,
} from './modelManager.js';

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

run('splitModelIds 去空去重并保留顺序', () => {
  assert.deepEqual(splitModelIds(' gpt-4o, ,glm-4,gpt-4o '), ['gpt-4o', 'glm-4']);
});

run('modelNameSlug 清理不可用字符并避免保留前缀', () => {
  assert.equal(modelNameSlug('(open router/gpt 4o)'), 'open-router-gpt-4o');
  assert.equal(modelNameSlug(''), 'model');
});

run('buildModelDraftsFromSelection 单模型使用输入名称或模型名', () => {
  assert.deepEqual(buildModelDraftsFromSelection({
    name: '', provider: 'copilot', model: 'gpt-4o',
  }), [{
    name: 'gpt-4o', provider: 'copilot', model: 'gpt-4o',
  }]);
});

run('buildModelDraftsFromSelection 多模型生成稳定名称', () => {
  const drafts = buildModelDraftsFromSelection({
    name: 'open router', provider: 'openai', model: 'gpt-4o, anthropic/claude',
    base_url: 'http://x', api_key: 'sk',
  });
  assert.equal(drafts.length, 2);
  assert.equal(drafts[0].name, 'open-router-gpt-4o');
  assert.equal(drafts[0].model, 'gpt-4o');
  assert.equal(drafts[1].name, 'open-router-anthropic-claude');
  assert.equal(drafts[1].model, 'anthropic/claude');
});

run('buildModelDraftsFromSelection 多模型名称冲突时追加后缀', () => {
  const drafts = buildModelDraftsFromSelection({
    name: '', provider: 'openai', model: 'a/b, a b, a-b',
    base_url: 'http://x', api_key: 'sk',
  });
  assert.deepEqual(drafts.map((d) => d.name), ['a-b', 'a-b-2', 'a-b-3']);
});
