export function normalizeModelState(raw) {
  if (!raw || typeof raw !== 'object') return null;
  return {
    name: String(raw.name || raw.model_name || raw.model_preset || ''),
    provider: String(raw.provider || ''),
    model: String(raw.model || ''),
    contextWindow: Number(raw.context_window || raw.contextWindow || 0) || 0,
    isLegacy: !!(raw.is_legacy || raw.model_is_legacy),
  };
}

export function modelDisplayLabel(state, fallback = '加载中') {
  const normalized = normalizeModelState(state);
  if (!normalized) return fallback;
  if (normalized.name) return normalized.name;
  if (normalized.provider && normalized.model) return `${normalized.provider}/${normalized.model}`;
  return normalized.model || fallback;
}

export function selectedModelName(state) {
  return normalizeModelState(state)?.name || '';
}

export function modelSelectValue(state, pendingName = '') {
  return String(pendingName || '') || selectedModelName(state);
}

export function normalizeModelOptions(list) {
  const source = Array.isArray(list) ? list : [];
  const seen = new Set();
  const out = [];
  for (const item of source) {
    const state = normalizeModelState(item);
    if (!state?.name || seen.has(state.name)) continue;
    seen.add(state.name);
    out.push(state);
  }
  return out;
}

export function optionLabel(option) {
  const state = normalizeModelState(option);
  if (!state) return '';
  if (state.name && state.provider && state.model) {
    return `${state.name} (${state.provider}/${state.model})`;
  }
  return modelDisplayLabel(state, '');
}
