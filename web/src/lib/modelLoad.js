// 模型池负载的前端纯函数:色阶判定 + 从 /api/model-pool-status 快照里按模型 id
// 精确匹配 modelPoolName 取负载。阈值与后端一致(<70 绿 / 70..90 黄 / >90 红)。

// 负载色阶。返回 'green' | 'yellow' | 'red' | null(未知/无效)。
export function loadTier(usageRate) {
  if (typeof usageRate !== 'number' || !Number.isFinite(usageRate) || usageRate < 0) return null;
  if (usageRate > 90) return 'red';
  if (usageRate >= 70) return 'yellow';
  return 'green';
}

// 色阶 → Tailwind 文本色类(globals.css 的 --color-ok/warn/danger)。
export function loadTierTextClass(tier) {
  if (tier === 'red') return 'text-danger';
  if (tier === 'yellow') return 'text-warn';
  if (tier === 'green') return 'text-ok';
  return 'text-fg-mute';
}

// 从 models 数组(/api/model-pool-status 的 models 字段)里按 modelPoolName 精确
// 匹配 modelId(= saved model 的 model 字段),返回归一化负载对象或 null。
// modelId 为空 / 未命中 / models 非数组 → null(调用方据此不渲染负载指示)。
export function pickModelLoad(models, modelId) {
  if (!Array.isArray(models) || !modelId) return null;
  const hit = models.find((m) => m && m.modelPoolName === modelId);
  if (!hit) return null;
  const usageRate = typeof hit.usageRate === 'number' ? hit.usageRate : -1;
  if (loadTier(usageRate) === null) return null; // 命中但负载未知 → 不显示
  return {
    usageRate,
    maxWindowTokens: typeof hit.maxWindowTokens === 'number' ? hit.maxWindowTokens : 0,
    effectiveContextWindow:
      typeof hit.effectiveContextWindow === 'number' ? hit.effectiveContextWindow : 0,
  };
}
