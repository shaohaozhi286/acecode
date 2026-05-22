// web/src/lib/modelManager.js
// 提交前的快速校验,避免没必要的 4xx 往返。规则与后端 saved_models_editor
// 保持一致;后端是真值源,前端不重复实现复杂分支。

export function validateModelDraft(draft) {
  if (!draft || typeof draft !== 'object') return { ok: false, code: 'BAD_REQUEST' };
  const { name, provider, model, base_url, api_key, context_window } = draft;
  if (!name || typeof name !== 'string' || name.length === 0)
    return { ok: false, code: 'INVALID_NAME' };
  if (name.startsWith('(')) return { ok: false, code: 'RESERVED_NAME' };
  if (provider !== 'openai' && provider !== 'copilot')
    return { ok: false, code: 'UNKNOWN_PROVIDER' };
  if (!model) return { ok: false, code: 'MISSING_MODEL' };
  if (provider === 'openai') {
    if (!base_url) return { ok: false, code: 'MISSING_BASE_URL' };
    if (!api_key) return { ok: false, code: 'INVALID_API_KEY' };
  }
  if (context_window !== undefined && context_window !== null) {
    const parsed = Number(context_window);
    if (!Number.isInteger(parsed) || parsed < 0 || parsed > 2147483647) {
      return { ok: false, code: 'INVALID_CONTEXT_WINDOW' };
    }
  }
  return { ok: true };
}

export function splitModelIds(value) {
  const seen = new Set();
  const out = [];
  String(value || '')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
    .forEach((id) => {
      if (seen.has(id)) return;
      seen.add(id);
      out.push(id);
    });
  return out;
}

export function filterModelIds(models, query) {
  const ids = Array.isArray(models) ? models : [];
  const terms = String(query || '')
    .trim()
    .toLowerCase()
    .split(/\s+/)
    .filter(Boolean);
  if (terms.length === 0) return ids;
  return ids.filter((id) => {
    const haystack = String(id || '').toLowerCase();
    return terms.every((term) => haystack.includes(term));
  });
}

export function parseContextWindowK(value) {
  const raw = String(value ?? '').trim();
  if (!raw) return { ok: true, tokens: null };
  if (!/^\d+(?:\.\d{1,3})?$/.test(raw)) {
    return { ok: false, code: 'INVALID_CONTEXT_WINDOW' };
  }
  const k = Number(raw);
  const tokens = Math.round(k * 1000);
  if (!Number.isFinite(k) || k <= 0 || tokens <= 0 || tokens > 2147483647) {
    return { ok: false, code: 'INVALID_CONTEXT_WINDOW' };
  }
  return { ok: true, tokens };
}

export function formatContextWindowK(tokens) {
  const value = Number(tokens);
  if (!Number.isFinite(value) || value <= 0) return '';
  const k = value / 1000;
  return Number.isInteger(k) ? String(k) : String(Number(k.toFixed(3)));
}

export function modelNameSlug(value, fallback = 'model') {
  const cleaned = String(value || '')
    .trim()
    .replace(/^\(+/, '')
    .replace(/[^A-Za-z0-9._-]+/g, '-')
    .replace(/-+/g, '-')
    .replace(/^-|-$/g, '');
  return cleaned || fallback;
}

export function buildModelDraftsFromSelection(draft) {
  const ids = splitModelIds(draft?.model);
  if (ids.length === 0) return [];
  const baseName = String(draft?.name || '').trim();
  const seenNames = new Set();
  return ids.map((modelId, index) => {
    const modelSlug = modelNameSlug(modelId, `model-${index + 1}`);
    const rawName = ids.length === 1
      ? modelNameSlug(baseName || modelId, modelSlug)
      : baseName
        ? `${modelNameSlug(baseName)}-${modelSlug}`
        : modelSlug;
    let name = rawName;
    let suffix = 2;
    while (seenNames.has(name)) {
      name = `${rawName}-${suffix}`;
      suffix += 1;
    }
    seenNames.add(name);
    return {
      ...draft,
      name,
      model: modelId,
    };
  });
}
