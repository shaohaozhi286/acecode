// web/src/components/ModelManager.jsx
// 左侧 saved_models 列表(默认带星);右侧编辑表单。增 / 改 / 删 / 设默认。
//
// api_key 编辑策略(配合 PUT /api/models/:name 的 patch 语义):
//   - 编辑模式打开时 api_key 字段预填 mask "••••••••"(纯视觉占位,不是真值)
//   - 用户聚焦后清空字段,等待输入;blur 时若没改回到 mask
//   - 提交时若 apiKeyTouched=false → payload 删 api_key,后端从 existing 注入
//   - apiKeyTouched=true → 发用户输入的新值(可能为空,后端会按校验拒绝)
import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { validateModelDraft } from '../lib/modelManager.js';

const EMPTY_DRAFT = {
  name: '', provider: 'openai', model: '',
  base_url: '', api_key: '',
};
const API_KEY_MASK = '••••••••';

export function ModelManager({ apiClient = api }) {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [editingName, setEditingName] = useState(null);
  const [draft, setDraft] = useState(EMPTY_DRAFT);
  const [apiKeyTouched, setApiKeyTouched] = useState(false);
  const [busy, setBusy] = useState(false);

  const refresh = async () => {
    try {
      const list = await apiClient.listModels();
      setModels(Array.isArray(list) ? list : []);
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    }
    try {
      const d = await apiClient.getDefaultModel();
      setDefaultName((d && d.name) || '');
    } catch {
      // 默认拿不到不致命,星标暂不显示
    }
  };

  useEffect(() => { refresh(); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, []);

  const onSelect = (m) => {
    if (m.is_legacy) {
      // (legacy) 行不可编辑 — 退回新增态。
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
      setApiKeyTouched(true);
      return;
    }
    setEditingName(m.name);
    setApiKeyTouched(false);
    setDraft({
      name: m.name,
      provider: m.provider,
      model: m.model,
      base_url: m.base_url || '',
      api_key: API_KEY_MASK, // 视觉 mask;真值不在 GET 响应里返,后端走 patch
    });
  };

  const startNew = () => {
    setEditingName(null);
    setDraft(EMPTY_DRAFT);
    setApiKeyTouched(true); // 新增模式 api_key 必填,直接进 touched 状态
  };

  const onApiKeyFocus = () => {
    if (editingName && !apiKeyTouched) {
      // 进入编辑:清空 mask 让用户能直接输入新 key,但还没真的算 touched —
      // 用户 blur 而没输入就恢复 mask。
      setDraft((d) => ({ ...d, api_key: '' }));
    }
  };
  const onApiKeyChange = (e) => {
    setDraft((d) => ({ ...d, api_key: e.target.value }));
    setApiKeyTouched(true);
  };
  const onApiKeyBlur = () => {
    if (editingName && !apiKeyTouched) {
      setDraft((d) => ({ ...d, api_key: API_KEY_MASK }));
    }
  };

  const submit = async () => {
    // 编辑模式 + 没改 api_key:client-side 校验跳过它(后端 PUT 会从 existing 注入)。
    const skipApiKey = !!editingName && !apiKeyTouched;
    const draftForValidate = skipApiKey
      ? { ...draft, api_key: '__patch__' }  // 占位让 validateModelDraft 通过 openai 必填检查
      : draft;
    const v = validateModelDraft(draftForValidate);
    if (!v.ok) { toast({ kind: 'err', text: lookupErrorMessage(v.code) }); return; }
    setBusy(true);
    try {
      if (editingName) {
        const payload = { ...draft };
        if (skipApiKey) delete payload.api_key; // 后端 patch:缺字段保留旧值
        await apiClient.updateModel(editingName, payload);
      } else {
        await apiClient.addModel(draft);
      }
      toast({ kind: 'ok', text: editingName ? '已更新' : '已新增' });
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
      setApiKeyTouched(true);
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const removeOne = async (name) => {
    setBusy(true);
    try {
      await apiClient.removeModel(name);
      toast({ kind: 'ok', text: '已删除 ' + name });
      if (editingName === name) { setEditingName(null); setDraft(EMPTY_DRAFT); setApiKeyTouched(true); }
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const setDefault = async (name) => {
    setBusy(true);
    try {
      await apiClient.setDefaultModel(name);
      setDefaultName(name);
      toast({ kind: 'ok', text: '默认: ' + name });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  return (
    <div className="grid grid-cols-[280px_1fr] gap-5">
      {/* 左栏:模型列表 */}
      <div>
        <div className="text-[14px] font-semibold mb-1">已保存模型</div>
        <p className="text-[12px] text-fg-mute mb-3">点条目选中右侧编辑;★ 是当前默认</p>
        <div className="space-y-2">
          {models.map((m) => {
            const active = editingName === m.name;
            const isDefault = defaultName === m.name;
            return (
              <div
                key={m.name}
                role={m.is_legacy ? undefined : 'button'}
                tabIndex={m.is_legacy ? -1 : 0}
                onClick={() => onSelect(m)}
                onKeyDown={(e) => {
                  if (m.is_legacy) return;
                  if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); onSelect(m); }
                }}
                className={clsx(
                  'flex items-center justify-between px-3.5 py-2.5 rounded-md border transition',
                  m.is_legacy ? 'bg-surface border-border opacity-70 cursor-default'
                              : 'bg-surface cursor-pointer hover:bg-surface-hi',
                  active ? 'border-accent border-2 bg-accent-bg' : 'border-border',
                )}
              >
                <div className="min-w-0 flex-1">
                  <div className="flex items-center gap-1.5">
                    {isDefault && <span className="text-accent text-[12px]" aria-label="默认">★</span>}
                    <span className="text-[13px] font-medium truncate">{m.name}</span>
                    {m.is_legacy && <span className="text-[11px] text-fg-mute">(legacy)</span>}
                  </div>
                  <div className="text-[11px] text-fg-mute mt-0.5 truncate">
                    {m.provider} · {m.model}
                  </div>
                </div>
                {!m.is_legacy && (
                  <span className="flex gap-1 shrink-0 ml-2">
                    {!isDefault && (
                      <button
                        type="button"
                        className="px-1.5 py-0.5 text-[11px] text-fg-mute hover:text-accent hover:underline"
                        onClick={(e) => { e.stopPropagation(); setDefault(m.name); }}
                        disabled={busy}
                      >
                        设为默认
                      </button>
                    )}
                    <button
                      type="button"
                      className="px-1.5 py-0.5 text-[11px] text-danger hover:underline"
                      onClick={(e) => { e.stopPropagation(); removeOne(m.name); }}
                      disabled={busy}
                    >
                      删
                    </button>
                  </span>
                )}
              </div>
            );
          })}
          <button
            type="button"
            onClick={startNew}
            className="w-full flex items-center justify-center gap-1.5 px-3.5 py-2.5 rounded-md border border-dashed border-border bg-surface text-[12px] text-fg-mute hover:text-accent hover:border-accent/50 hover:bg-surface-hi transition"
          >
            <span className="text-[14px] leading-none">+</span>
            <span>新增模型</span>
          </button>
        </div>
      </div>

      {/* 右栏:编辑表单 */}
      <div>
        <div className="text-[14px] font-semibold mb-1">
          {editingName ? '编辑模型' : '新增模型'}
          {editingName && (
            <span className="ml-2 text-[12px] text-fg-mute font-normal">{editingName}</span>
          )}
        </div>
        <p className="text-[12px] text-fg-mute mb-3">
          {editingName ? 'OpenAI 类的 api_key 字段聚焦后输入新值,留空保留旧值' : '填写后点新增,字段后续可改'}
        </p>
        <div className="space-y-2.5 max-w-xl">
          <Field label="名字">
            <input
              value={draft.name}
              onChange={(e) => setDraft({ ...draft, name: e.target.value })}
              disabled={!!editingName}
              placeholder="例如: gpt-4o-fast"
            />
          </Field>
          <Field label="provider">
            <select value={draft.provider} onChange={(e) => setDraft({ ...draft, provider: e.target.value })}>
              <option value="openai">openai</option>
              <option value="copilot">copilot</option>
            </select>
          </Field>
          <Field label="model">
            <input
              value={draft.model}
              onChange={(e) => setDraft({ ...draft, model: e.target.value })}
              placeholder={draft.provider === 'copilot' ? '例如: gpt-4o' : '例如: llama-3 / openai/gpt-4o'}
            />
          </Field>
          {draft.provider === 'openai' && (
            <>
              <Field label="base_url">
                <input
                  value={draft.base_url}
                  onChange={(e) => setDraft({ ...draft, base_url: e.target.value })}
                  placeholder="https://api.openai.com/v1"
                />
              </Field>
              <Field label="api_key">
                <input
                  type="password"
                  value={draft.api_key}
                  placeholder={editingName ? '聚焦后输入新 api_key,留空则保留旧值' : 'sk-...'}
                  onFocus={onApiKeyFocus}
                  onChange={onApiKeyChange}
                  onBlur={onApiKeyBlur}
                />
              </Field>
            </>
          )}
          <div className="pt-2 flex items-center gap-2">
            <button
              type="button"
              className="px-4 py-1.5 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 active:opacity-100 disabled:opacity-50 transition"
              onClick={submit}
              disabled={busy}
            >
              {editingName ? '保存' : '新增'}
            </button>
            {editingName && (
              <button
                type="button"
                className="px-3 py-1.5 rounded-md text-[12px] text-fg-mute hover:text-fg hover:bg-surface-hi transition"
                onClick={() => { setEditingName(null); setDraft(EMPTY_DRAFT); setApiKeyTouched(true); }}
                disabled={busy}
              >
                取消
              </button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

function Field({ label, children }) {
  // 全宽表单字段。input/select 通过孙选择器统一样式 — 高度 h-8、focus 描边变
  // accent、disabled 字色变浅。占位走 placeholder:text-fg-mute/60。
  return (
    <label className="flex items-center gap-3 text-[12px]">
      <span className="w-[72px] text-right text-fg-mute shrink-0">{label}</span>
      <span className="flex-1 [&>input]:w-full [&>input]:h-8 [&>input]:px-2.5 [&>input]:text-[12px] [&>input]:border [&>input]:border-border [&>input]:rounded-md [&>input]:bg-surface-alt [&>input]:text-fg [&>input]:outline-none [&>input]:transition [&>input:focus]:border-accent [&>input:disabled]:opacity-60 [&>input::placeholder]:text-fg-mute/60 [&>select]:w-full [&>select]:h-8 [&>select]:px-2.5 [&>select]:text-[12px] [&>select]:border [&>select]:border-border [&>select]:rounded-md [&>select]:bg-surface-alt [&>select]:text-fg [&>select]:outline-none [&>select]:transition [&>select:focus]:border-accent">
        {children}
      </span>
    </label>
  );
}
