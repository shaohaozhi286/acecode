function toNonNegativeInt(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.trunc(n));
}

function readTokenValue(source, camelKey, snakeKey) {
  if (!source || typeof source !== 'object') return 0;
  return toNonNegativeInt(source[camelKey] ?? source[snakeKey]);
}

function hasApiUsageData(usage) {
  if (!usage || typeof usage !== 'object') return false;
  return usage.hasData === true || usage.has_data === true;
}

function unknownBudget(limitTokens, reason = 'no_usage') {
  const label = reason === 'no_limit'
    ? '上下文窗口未知，暂无法计算 token 余量'
    : '尚未收到 token 用量数据';
  return {
    known: false,
    reason,
    hasData: false,
    usedTokens: 0,
    completionTokens: 0,
    totalTokens: 0,
    limitTokens,
    remainingTokens: null,
    usedRatio: 0,
    rawUsedRatio: 0,
    percent: 0,
    severity: 'unknown',
    title: label,
    ariaLabel: label,
  };
}

export function normalizeTokenBudget({ usage = null, contextWindow = 0 } = {}) {
  const limitTokens = toNonNegativeInt(contextWindow);
  if (!hasApiUsageData(usage)) {
    return unknownBudget(limitTokens, 'no_usage');
  }
  if (limitTokens <= 0) {
    return unknownBudget(limitTokens, 'no_limit');
  }

  const usedTokens = readTokenValue(usage, 'promptTokens', 'prompt_tokens');
  const completionTokens = readTokenValue(usage, 'completionTokens', 'completion_tokens');
  const totalTokens = readTokenValue(usage, 'totalTokens', 'total_tokens');
  const rawUsedRatio = usedTokens / limitTokens;
  const usedRatio = Math.min(1, Math.max(0, rawUsedRatio));
  const percent = Math.round(usedRatio * 100);
  const remainingTokens = Math.max(0, limitTokens - usedTokens);
  const severity = rawUsedRatio >= 0.9
    ? 'danger'
    : rawUsedRatio >= 0.7
      ? 'warning'
      : 'safe';
  const title = `上下文 token：${usedTokens} / ${limitTokens}（${percent}% 已用），剩余 ${remainingTokens}`;

  return {
    known: true,
    reason: '',
    hasData: true,
    usedTokens,
    completionTokens,
    totalTokens,
    limitTokens,
    remainingTokens,
    usedRatio,
    rawUsedRatio,
    percent,
    severity,
    title,
    ariaLabel: title,
  };
}
