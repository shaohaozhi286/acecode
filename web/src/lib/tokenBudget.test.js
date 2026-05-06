import assert from 'node:assert/strict';
import { normalizeTokenBudget } from './tokenBudget.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('prompt_tokens / context_window 决定圆环进度', () => {
  const budget = normalizeTokenBudget({
    usage: { promptTokens: 32000, completionTokens: 16000, totalTokens: 48000, hasData: true },
    contextWindow: 128000,
  });
  assert.equal(budget.known, true);
  assert.equal(budget.usedTokens, 32000);
  assert.equal(budget.limitTokens, 128000);
  assert.equal(budget.percent, 25);
  assert.equal(budget.usedRatio, 0.25);
});

run('completion_tokens 和 total_tokens 不影响圆环进度', () => {
  const a = normalizeTokenBudget({
    usage: { promptTokens: 32000, completionTokens: 0, totalTokens: 32000, hasData: true },
    contextWindow: 128000,
  });
  const b = normalizeTokenBudget({
    usage: { promptTokens: 32000, completionTokens: 90000, totalTokens: 122000, hasData: true },
    contextWindow: 128000,
  });
  assert.equal(a.percent, 25);
  assert.equal(b.percent, 25);
  assert.equal(b.completionTokens, 90000);
  assert.equal(b.totalTokens, 122000);
});

run('超过 context_window 时视觉进度钳制到 100%', () => {
  const budget = normalizeTokenBudget({
    usage: { prompt_tokens: 150000, completion_tokens: 10, total_tokens: 150010, has_data: true },
    contextWindow: 128000,
  });
  assert.equal(budget.usedTokens, 150000);
  assert.equal(budget.limitTokens, 128000);
  assert.equal(budget.remainingTokens, 0);
  assert.equal(budget.usedRatio, 1);
  assert.equal(budget.percent, 100);
  assert.equal(budget.severity, 'danger');
  assert.match(budget.title, /150000 \/ 128000/);
});

run('无 usage 或无 context_window 时为 unknown', () => {
  const noUsage = normalizeTokenBudget({ contextWindow: 128000 });
  assert.equal(noUsage.known, false);
  assert.equal(noUsage.severity, 'unknown');
  assert.equal(noUsage.reason, 'no_usage');

  const noLimit = normalizeTokenBudget({
    usage: { promptTokens: 8000, completionTokens: 1, totalTokens: 8001, hasData: true },
    contextWindow: 0,
  });
  assert.equal(noLimit.known, false);
  assert.equal(noLimit.reason, 'no_limit');
});

run('severity 阈值仅按 prompt token 已用比例计算', () => {
  assert.equal(normalizeTokenBudget({ usage: { promptTokens: 64000, hasData: true }, contextWindow: 128000 }).severity, 'safe');
  assert.equal(normalizeTokenBudget({ usage: { promptTokens: 89600, hasData: true }, contextWindow: 128000 }).severity, 'warning');
  assert.equal(normalizeTokenBudget({ usage: { promptTokens: 96000, hasData: true }, contextWindow: 128000 }).severity, 'warning');
  assert.equal(normalizeTokenBudget({ usage: { promptTokens: 115200, hasData: true }, contextWindow: 128000 }).severity, 'danger');
});
