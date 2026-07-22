import { effectiveLocale } from '../i18n/index.js';
import { formatCompactNumber } from './format.js';

function numberValue(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.trunc(n));
}

function stringValue(value) {
  return typeof value === 'string' ? value : '';
}

function normalizeTotals(raw = {}) {
  return {
    promptTokens: numberValue(raw.prompt_tokens ?? raw.promptTokens ?? raw.input_tokens ?? raw.inputTokens),
    completionTokens: numberValue(raw.completion_tokens ?? raw.completionTokens ?? raw.output_tokens ?? raw.outputTokens),
    totalTokens: numberValue(raw.total_tokens ?? raw.totalTokens),
    cacheReadTokens: numberValue(raw.cache_read_tokens ?? raw.cacheReadTokens),
    cacheWriteTokens: numberValue(raw.cache_write_tokens ?? raw.cacheWriteTokens),
    reasoningTokens: numberValue(raw.reasoning_tokens ?? raw.reasoningTokens),
  };
}

function normalizeCommonBucket(raw = {}) {
  const totalsSource = raw.totals && typeof raw.totals === 'object' ? raw.totals : raw;
  return {
    records: numberValue(raw.records),
    estimatedRecords: numberValue(raw.estimated_records ?? raw.estimatedRecords),
    sessionCount: numberValue(raw.session_count ?? raw.sessionCount ?? raw.sessions),
    totals: normalizeTotals(totalsSource),
  };
}

export function formatUsageTokens(value, locale = effectiveLocale()) {
  const n = numberValue(value);
  return formatCompactNumber(n, { maximumFractionDigits: 2 }, locale);
}

export function normalizeUsageStats(raw = {}) {
  const summaryRaw = raw.summary || {};
  const summary = {
    ...normalizeCommonBucket(summaryRaw),
  };
  const daily = Array.isArray(raw.daily) ? raw.daily.map((item) => ({
    ...normalizeCommonBucket(item),
    date: stringValue(item.date),
    dayStartMs: numberValue(item.day_start_ms ?? item.dayStartMs),
    tokens: numberValue(item.tokens ?? item.total_tokens ?? item.totalTokens ?? item.totals?.total_tokens ?? item.totals?.totalTokens),
  })) : [];
  const models = Array.isArray(raw.models) ? raw.models.map((item) => ({
    ...normalizeCommonBucket(item),
    label: stringValue(item.label) || stringValue(item.model_preset) || stringValue(item.model) || stringValue(item.provider) || 'unknown',
    provider: stringValue(item.provider),
    model: stringValue(item.model),
    modelPreset: stringValue(item.model_preset ?? item.modelPreset),
  })).sort((a, b) => b.totals.totalTokens - a.totals.totalTokens || b.records - a.records) : [];
  const workspaces = Array.isArray(raw.workspaces) ? raw.workspaces.map((item) => ({
    ...normalizeCommonBucket(item),
    workspaceHash: stringValue(item.workspace_hash ?? item.workspaceHash),
    workspaceName: stringValue(item.workspace_name ?? item.workspaceName) || stringValue(item.cwd) || stringValue(item.workspace_hash),
    cwd: stringValue(item.cwd),
  })).sort((a, b) => b.totals.totalTokens - a.totals.totalTokens || b.records - a.records) : [];
  const metadataRaw = raw.metadata || {};
  const metadata = {
    days: numberValue(metadataRaw.days) || 30,
    periodStartMs: numberValue(metadataRaw.period_start_ms ?? metadataRaw.periodStartMs),
    periodEndMs: numberValue(metadataRaw.period_end_ms ?? metadataRaw.periodEndMs),
    periodStart: stringValue(metadataRaw.period_start ?? metadataRaw.periodStart),
    periodEnd: stringValue(metadataRaw.period_end ?? metadataRaw.periodEnd),
    timezoneOffsetMinutes: Number.isFinite(Number(metadataRaw.timezone_offset_minutes ?? metadataRaw.timezoneOffsetMinutes))
      ? Math.trunc(Number(metadataRaw.timezone_offset_minutes ?? metadataRaw.timezoneOffsetMinutes))
      : 0,
    forwardOnly: metadataRaw.forward_only !== false && metadataRaw.forwardOnly !== false,
    workspaceFilter: stringValue(metadataRaw.workspace_filter ?? metadataRaw.workspaceFilter),
  };
  return {
    summary,
    daily,
    models,
    workspaces,
    metadata,
    hasData: summary.records > 0 || summary.totals.totalTokens > 0,
    hasEstimates: summary.estimatedRecords > 0,
    maxDailyTokens: daily.reduce((max, item) => Math.max(max, item.tokens), 0),
    maxModelTokens: models.reduce((max, item) => Math.max(max, item.totals.totalTokens), 0),
    maxWorkspaceTokens: workspaces.reduce((max, item) => Math.max(max, item.totals.totalTokens), 0),
  };
}

export function usageDataNote(stats) {
  if (!stats?.hasData) {
    return '从此版本开始记录新的 token 用量,历史会话不会回填';
  }
  if (stats.hasEstimates) {
    return `包含 ${stats.summary.estimatedRecords} 条本地估算记录,总量可能与服务商账单不同`;
  }
  return '统计来自 provider usage 回传;历史会话不会回填';
}
