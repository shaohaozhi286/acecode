export function normalizeModelState(raw) {
  if (!raw || typeof raw !== 'object') return null;
  return {
    name: String(raw.name || raw.model_name || raw.model_preset || ''),
    provider: String(raw.provider || ''),
    model: String(raw.model || ''),
    contextWindow: Number(raw.context_window || raw.contextWindow || 0) || 0,
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

function visualColumns(text) {
  let columns = 0;
  for (const ch of String(text || '')) {
    columns += ch.charCodeAt(0) > 127 ? 2 : 1;
  }
  return columns;
}

export function buildStatusBarModelMenu({ modelOptions = [], selectedModelName = '', fallbackLabel = '—' } = {}) {
  const options = normalizeModelOptions(modelOptions);
  const selected = String(selectedModelName || '');
  const selectedOption = options.find((option) => option.name === selected);
  const displayLabel = selectedOption
    ? optionLabel(selectedOption)
    : (selected || String(fallbackLabel || '—'));
  const items = options.map((option) => ({
    name: option.name,
    label: optionLabel(option),
    provider: option.provider,
    model: option.model,
    active: option.name === selected,
  }));
  const widthLabel = [displayLabel, ...items.map((item) => item.label)]
    .reduce((longest, label) => (
      visualColumns(label) > visualColumns(longest) ? label : longest
    ), '');
  return {
    displayLabel,
    widthLabel: widthLabel || displayLabel,
    items,
  };
}

export function resolveHomeModelName(modelOptions = [], defaultModelName = '', previousName = '') {
  const options = normalizeModelOptions(modelOptions);
  const hasOption = (name) => !!name && options.some((option) => option.name === name);
  const previous = String(previousName || '');
  if (hasOption(previous)) return previous;
  const defaultName = String(defaultModelName || '');
  if (hasOption(defaultName)) return defaultName;
  return options[0]?.name || previous || defaultName || '';
}

export function withCreateSessionModel(options = {}, modelName = '') {
  const next = { ...(options && typeof options === 'object' ? options : {}) };
  const name = String(modelName || '').trim();
  if (name) next.name = name;
  return next;
}
