import assert from 'node:assert/strict';
import { formatUsageTokens, normalizeUsageStats, usageDataNote } from './usageStats.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('normalizeUsageStats maps summary, models, workspaces and estimates', () => {
  const stats = normalizeUsageStats({
    summary: {
      records: 3,
      estimated_records: 1,
      session_count: 2,
      totals: {
        prompt_tokens: 1000,
        completion_tokens: 300,
        total_tokens: 1300,
        cache_read_tokens: 200,
        reasoning_tokens: 50,
      },
    },
    daily: [
      { date: '2026-06-05', tokens: 100, records: 1, totals: { total_tokens: 100 } },
      { date: '2026-06-06', tokens: 1200, records: 2, estimated_records: 1, totals: { total_tokens: 1200 } },
    ],
    models: [
      { label: 'small', records: 1, totals: { total_tokens: 100 } },
      { label: 'large', records: 2, estimated_records: 1, totals: { total_tokens: 1200 } },
    ],
    workspaces: [
      { workspace_hash: 'w1', workspace_name: 'repo', records: 3, totals: { total_tokens: 1300 } },
    ],
    metadata: { days: 7, forward_only: true },
  });

  assert.equal(stats.hasData, true);
  assert.equal(stats.hasEstimates, true);
  assert.equal(stats.summary.totals.promptTokens, 1000);
  assert.equal(stats.summary.totals.totalTokens, 1300);
  assert.equal(stats.summary.estimatedRecords, 1);
  assert.equal(stats.maxDailyTokens, 1200);
  assert.equal(stats.models[0].label, 'large');
  assert.equal(stats.workspaces[0].workspaceName, 'repo');
  assert.match(usageDataNote(stats), /包含 1 条/);
});

run('normalizeUsageStats keeps empty state forward-only', () => {
  const stats = normalizeUsageStats({ summary: { records: 0, totals: {} }, metadata: { days: 30 } });
  assert.equal(stats.hasData, false);
  assert.equal(stats.daily.length, 0);
  assert.match(usageDataNote(stats), /从此版本开始记录/);
});

run('normalizeUsageStats accepts flat snake_case usage buckets', () => {
  const stats = normalizeUsageStats({
    summary: {
      records: 4,
      sessions: 2,
      input_tokens: 2000,
      output_tokens: 500,
      total_tokens: 2500,
    },
    daily: [
      { date: '2026-06-06', total_tokens: 2500 },
    ],
    models: [
      { model: 'gpt-4o', records: 4, input_tokens: 2000, output_tokens: 500, total_tokens: 2500 },
    ],
    workspaces: [
      { cwd: 'C:/repo', records: 4, total_tokens: 2500 },
    ],
  });

  assert.equal(stats.summary.totals.promptTokens, 2000);
  assert.equal(stats.summary.totals.completionTokens, 500);
  assert.equal(stats.summary.totals.totalTokens, 2500);
  assert.equal(stats.summary.sessionCount, 2);
  assert.equal(stats.daily[0].tokens, 2500);
  assert.equal(stats.models[0].totals.totalTokens, 2500);
  assert.equal(stats.workspaces[0].totals.totalTokens, 2500);
});

run('formatUsageTokens uses compact token units', () => {
  assert.equal(formatUsageTokens(608), '608');
  assert.equal(formatUsageTokens(79_800), '79.8K');
  assert.equal(formatUsageTokens(2_847_563), '2.85M');
});
