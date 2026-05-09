// web/src/components/ModelManager.jsx
// 左侧 saved_models 列表(默认带星);右侧编辑表单。增 / 改 / 删 / 设默认。
//
// 已知限制:OpenAI 类编辑时 api_key 必须重新填(后端 update_saved_model 走
// validate_draft_basic,空 api_key 直接 INVALID_API_KEY)。后端补 patch 语义
// (空字段保留旧值)前,前端这里只能给出"必填"的 placeholder。
import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { lookupErrorMessage } from '../lib/errors.js';
import { validateModelDraft } from '../lib/modelManager.js';

const EMPTY_DRAFT = {
  name: '', provider: 'openai', model: '',
  base_url: '', api_key: '',
};

export function ModelManager({ apiClient = api }) {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [editingName, setEditingName] = useState(null);
  const [draft, setDraft] = useState(EMPTY_DRAFT);
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
      return;
    }
    setEditingName(m.name);
    setDraft({
      name: m.name,
      provider: m.provider,
      model: m.model,
      base_url: m.base_url || '',
      api_key: '', // 后端列表不返回 api_key(安全考虑),用户编辑 OpenAI 类时需重新填
    });
  };

  const startNew = () => { setEditingName(null); setDraft(EMPTY_DRAFT); };

  const submit = async () => {
    const v = validateModelDraft(draft);
    if (!v.ok) { toast({ kind: 'err', text: lookupErrorMessage(v.code) }); return; }
    setBusy(true);
    try {
      if (editingName) await apiClient.updateModel(editingName, draft);
      else             await apiClient.addModel(draft);
      toast({ kind: 'ok', text: editingName ? '已更新' : '已新增' });
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
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
      if (editingName === name) { setEditingName(null); setDraft(EMPTY_DRAFT); }
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
    <div className="grid grid-cols-[260px_1fr] gap-4">
      <div className="border border-border rounded-md overflow-hidden">
        {models.map((m) => (
          <div
            key={m.name}
            className={
              'flex items-center justify-between px-2 py-1.5 cursor-pointer hover:bg-surface-hi ' +
              (editingName === m.name ? 'bg-surface-hi' : '')
            }
            onClick={() => onSelect(m)}
          >
            <span className="truncate text-[12px]">
              {defaultName === m.name ? '★ ' : '  '}
              {m.name}{m.is_legacy ? ' (legacy)' : ''}
            </span>
            {!m.is_legacy && (
              <span className="flex gap-1 shrink-0">
                {defaultName !== m.name && (
                  <button
                    type="button"
                    className="px-1.5 py-0.5 text-[11px] hover:underline"
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
        ))}
        <div
          className="px-2 py-2 text-[12px] hover:bg-surface-hi cursor-pointer border-t border-border"
          onClick={startNew}
        >
          + 新增模型
        </div>
      </div>

      <div className="space-y-2">
        <div className="text-[12px] font-semibold">{editingName ? '编辑 ' + editingName : '新增模型'}</div>
        <Field label="名字">
          <input
            value={draft.name}
            onChange={(e) => setDraft({ ...draft, name: e.target.value })}
            disabled={!!editingName}
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
          />
        </Field>
        {draft.provider === 'openai' && (
          <>
            <Field label="base_url">
              <input
                value={draft.base_url}
                onChange={(e) => setDraft({ ...draft, base_url: e.target.value })}
              />
            </Field>
            <Field label="api_key">
              <input
                type="password"
                value={draft.api_key}
                placeholder={editingName ? '修改 api_key(必填)' : ''}
                onChange={(e) => setDraft({ ...draft, api_key: e.target.value })}
              />
            </Field>
          </>
        )}
        <button
          type="button"
          className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
          onClick={submit}
          disabled={busy}
        >
          {editingName ? '保存' : '新增'}
        </button>
      </div>
    </div>
  );
}

function Field({ label, children }) {
  return (
    <label className="flex items-center gap-2 text-[12px]">
      <span className="w-20 text-right text-fg-mute">{label}</span>
      <span className="flex-1 [&>input]:w-full [&>input]:px-2 [&>input]:py-1 [&>input]:border [&>input]:border-border [&>input]:rounded [&>input]:bg-surface-alt [&>input]:text-fg [&>input]:outline-none [&>select]:px-2 [&>select]:py-1 [&>select]:border [&>select]:border-border [&>select]:rounded [&>select]:bg-surface-alt [&>select]:text-fg">
        {children}
      </span>
    </label>
  );
}
