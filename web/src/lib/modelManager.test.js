// web/src/lib/modelManager.test.js
import assert from 'node:assert/strict';
import {
  buildModelDraftsFromSelection,
  capabilitySearchText,
  filterSavedModels,
  filterModelIds,
  formatContextWindowK,
  modelNameSlug,
  normalizeModelCapabilities,
  parseContextWindowK,
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

run('非法 context_window → INVALID_CONTEXT_WINDOW', () => {
  assert.equal(validateModelDraft({
    name: 'lm', provider: 'copilot', model: 'l', context_window: -1,
  }).code, 'INVALID_CONTEXT_WINDOW');
});

run('非法 capabilities → INVALID_CAPABILITY', () => {
  assert.equal(validateModelDraft({
    name: 'lm', provider: 'copilot', model: 'l', capabilities: ['vision', 'vision'],
  }).code, 'INVALID_CAPABILITY');
});

run('normalizeModelCapabilities 去重并保留顺序', () => {
  assert.deepEqual(
    normalizeModelCapabilities(['tool_use', '', 'vision', 'tool_use', 42]),
    ['tool_use', 'vision'],
  );
});

run('splitModelIds 去空去重并保留顺序', () => {
  assert.deepEqual(splitModelIds(' gpt-4o, ,glm-4,gpt-4o '), ['gpt-4o', 'glm-4']);
});

run('filterModelIds 使用输入内容缩小模型列表', () => {
  assert.deepEqual(
    filterModelIds(['gpt-4o', 'openrouter/anthropic/claude-3.7', 'glm-4-flash'], 'CLAUDE'),
    ['openrouter/anthropic/claude-3.7'],
  );
});

run('filterModelIds 支持多个空格分隔条件', () => {
  assert.deepEqual(
    filterModelIds(['gpt-4o-mini', 'gpt-4.1', 'gpt-4.1-mini'], 'gpt mini'),
    ['gpt-4o-mini', 'gpt-4.1-mini'],
  );
});

run('capabilitySearchText 展开标签别名', () => {
  const text = capabilitySearchText(['web_search', 'vision']);
  assert.match(text, /websearch/);
  assert.match(text, /联网/);
  assert.match(text, /vision/);
});

run('filterSavedModels 支持按能力标签搜索', () => {
  const models = [
    { name: 'code', provider: 'openai', model: 'deepseek-v4-pro', capabilities: ['tool_use'] },
    { name: 'eyes', provider: 'openai', model: 'vision-30b', capabilities: ['vision'] },
    { name: 'net', provider: 'copilot', model: 'gpt-web', capabilities: ['web_search'] },
  ];
  assert.deepEqual(filterSavedModels(models, 'vision').map((m) => m.name), ['eyes']);
  assert.deepEqual(filterSavedModels(models, 'websearch').map((m) => m.name), ['net']);
  assert.deepEqual(filterSavedModels(models, '联网').map((m) => m.name), ['net']);
});

run('parseContextWindowK 把 K 单位换算为 token 数', () => {
  assert.deepEqual(parseContextWindowK('128'), { ok: true, tokens: 128000 });
  assert.deepEqual(parseContextWindowK('131.072'), { ok: true, tokens: 131072 });
  assert.deepEqual(parseContextWindowK(''), { ok: true, tokens: null });
});

run('parseContextWindowK 拒绝非正数或超精度小数', () => {
  assert.equal(parseContextWindowK('0').code, 'INVALID_CONTEXT_WINDOW');
  assert.equal(parseContextWindowK('abc').code, 'INVALID_CONTEXT_WINDOW');
  assert.equal(parseContextWindowK('1.2345').code, 'INVALID_CONTEXT_WINDOW');
});

run('formatContextWindowK 把 token 数显示为 K', () => {
  assert.equal(formatContextWindowK(128000), '128');
  assert.equal(formatContextWindowK(131072), '131.072');
  assert.equal(formatContextWindowK(0), '');
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
