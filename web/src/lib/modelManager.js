// web/src/lib/modelManager.js
// 提交前的快速校验,避免没必要的 4xx 往返。规则与后端 saved_models_editor
// 保持一致;后端是真值源,前端不重复实现复杂分支。

export const MODEL_CAPABILITY_OPTIONS = [
  {
    id: 'vision',
    label: '视觉',
    aliases: ['vision', 'image', 'multimodal', 'mm', '看图', '图像'],
  },
  {
    id: 'web_search',
    label: '联网',
    aliases: ['websearch', 'web_search', 'web', 'search', '联网', '搜索'],
  },
  {
    id: 'reasoning',
    label: '推理',
    aliases: ['reasoning', 'think', 'thinking', '推理', '思考'],
  },
  {
    id: 'tool_use',
    label: '工具',
    aliases: ['tool', 'tools', 'tool_use', 'function', 'function_calling', '工具', '函数'],
  },
  {
    id: 'rerank',
    label: '重排',
    aliases: ['rerank', 'rank', 'ranking', '重排', '排序'],
  },
  {
    id: 'embedding',
    label: '嵌入',
    aliases: ['embedding', 'embed', 'vector', '向量', '嵌入'],
  },
];

export const DEFAULT_MODEL_CAPABILITIES = ['tool_use'];

function isValidCapabilityTag(tag) {
  return typeof tag === 'string' && tag.length > 0 && !/[\u0000-\u001f\u007f]/.test(tag);
}

const HEADER_NAME_RE = /^[A-Za-z0-9!#$%&'*+\-.^_`|~]+$/;
const ENV_NAME_RE = /^[A-Za-z0-9_]+$/;

function hasControlChars(value) {
  return /[\u0000-\u001f\u007f]/.test(value);
}

function validateHeaderTemplate(value) {
  let pos = 0;
  while (pos < value.length) {
    const open = value.indexOf('{env:', pos);
    const closeBeforeOpen = value.indexOf('}', pos);
    if (open === -1) return closeBeforeOpen === -1;
    if (closeBeforeOpen !== -1 && closeBeforeOpen < open) return false;
    const close = value.indexOf('}', open + 5);
    if (close === -1) return false;
    const name = value.slice(open + 5, close);
    if (!ENV_NAME_RE.test(name)) return false;
    pos = close + 1;
  }
  return true;
}

export function validateRequestHeaders(headers, provider = 'openai') {
  if (headers === undefined || headers === null) return { ok: true };
  if (typeof headers !== 'object' || Array.isArray(headers)) {
    return { ok: false, code: 'INVALID_REQUEST_HEADER' };
  }
  const entries = Object.entries(headers);
  if (entries.length === 0) return { ok: true };
  if (provider !== 'openai') return { ok: false, code: 'INVALID_REQUEST_HEADER' };

  const seen = new Set();
  for (const [name, value] of entries) {
    if (typeof value !== 'string') return { ok: false, code: 'INVALID_REQUEST_HEADER' };
    if (!name || !HEADER_NAME_RE.test(name)) return { ok: false, code: 'INVALID_REQUEST_HEADER' };
    const lower = name.toLowerCase();
    if (seen.has(lower)) return { ok: false, code: 'INVALID_REQUEST_HEADER' };
    seen.add(lower);
    if (lower === 'content-type') return { ok: false, code: 'INVALID_REQUEST_HEADER' };
    if (hasControlChars(value)) return { ok: false, code: 'INVALID_REQUEST_HEADER' };
    if (!validateHeaderTemplate(value)) return { ok: false, code: 'INVALID_REQUEST_HEADER' };
  }
  return { ok: true };
}

export function parseRequestHeadersJson(value) {
  const raw = String(value ?? '').trim();
  if (!raw) return { ok: true, headers: undefined };
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ok: false, code: 'INVALID_REQUEST_HEADER' };
  }
  const valid = validateRequestHeaders(parsed, 'openai');
  if (!valid.ok) return valid;
  return { ok: true, headers: parsed };
}

export function formatRequestHeadersJson(headers) {
  if (!headers || typeof headers !== 'object' || Array.isArray(headers)) return '';
  if (Object.keys(headers).length === 0) return '';
  return JSON.stringify(headers, null, 2);
}

export function normalizeModelCapabilities(capabilities) {
  if (!Array.isArray(capabilities)) return [];
  const seen = new Set();
  const out = [];
  capabilities.forEach((tag) => {
    if (!isValidCapabilityTag(tag) || seen.has(tag)) return;
    seen.add(tag);
    out.push(tag);
  });
  return out;
}

export function validateModelDraft(draft) {
  if (!draft || typeof draft !== 'object') return { ok: false, code: 'BAD_REQUEST' };
  const { name, provider, model, base_url, api_key, context_window, capabilities, request_headers } = draft;
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
  if (capabilities !== undefined && capabilities !== null) {
    if (!Array.isArray(capabilities)) return { ok: false, code: 'INVALID_CAPABILITY' };
    const normalized = normalizeModelCapabilities(capabilities);
    if (normalized.length !== capabilities.length) {
      return { ok: false, code: 'INVALID_CAPABILITY' };
    }
  }
  const requestHeaders = validateRequestHeaders(request_headers, provider);
  if (!requestHeaders.ok) return requestHeaders;
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

export function capabilitySearchText(capabilities) {
  const ids = normalizeModelCapabilities(capabilities);
  if (ids.length === 0) return '';
  const definitionById = new Map(MODEL_CAPABILITY_OPTIONS.map((item) => [item.id, item]));
  return ids
    .flatMap((id) => {
      const def = definitionById.get(id);
      return def ? [id, def.label, ...(def.aliases || [])] : [id];
    })
    .join(' ')
    .toLowerCase();
}

export function filterSavedModels(models, query) {
  const list = Array.isArray(models) ? models : [];
  const terms = String(query || '')
    .trim()
    .toLowerCase()
    .split(/\s+/)
    .filter(Boolean);
  if (terms.length === 0) return list;
  return list.filter((model) => {
    const haystack = [
      model?.name,
      model?.provider,
      model?.model,
      capabilitySearchText(model?.capabilities),
    ].filter(Boolean).join(' ').toLowerCase();
    return terms.every((term) => haystack.includes(term));
  });
}

export function canDeleteSavedModel({ models = [], defaultName = '', name = '', busy = false } = {}) {
  const target = String(name || '');
  if (!target || busy) return false;
  const list = Array.isArray(models) ? models : [];
  if (target !== String(defaultName || '')) return true;
  return list.length === 1 && list[0]?.name === target;
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

function positiveContextWindow(value) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0 || parsed > 2147483647) return 0;
  return parsed;
}

export function normalizeModelProbeResult(result) {
  const models = [];
  const seen = new Set();
  const contextWindows = {};
  const addContext = (id, tokens) => {
    const key = String(id || '').trim();
    if (!key) return;
    const parsed = positiveContextWindow(tokens);
    if (parsed > 0) contextWindows[key] = parsed;
  };
  const addModel = (id, tokens) => {
    const key = String(id || '').trim();
    if (!key) return;
    if (!seen.has(key)) {
      seen.add(key);
      models.push(key);
    }
    addContext(key, tokens);
  };

  const rawModels = Array.isArray(result?.models) ? result.models : [];
  rawModels.forEach((item) => {
    if (typeof item === 'string') {
      addModel(item, result?.model_context_windows?.[item]);
      return;
    }
    if (!item || typeof item !== 'object') return;
    const id = item.id || item.model || item.name;
    addModel(id, item.context_window || item.contextWindow);
  });

  const map = result?.model_context_windows;
  if (map && typeof map === 'object' && !Array.isArray(map)) {
    Object.entries(map).forEach(([id, tokens]) => addContext(id, tokens));
  }

  return { models, contextWindows };
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
