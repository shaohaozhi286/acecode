// 顶部 chat header 上的模型下拉。失败回滚 + 红 toast。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';

export function ModelPicker({ sessionId }) {
  const [models, setModels] = useState([]);
  const [value,  setValue]  = useState('');
  const [busy,   setBusy]   = useState(false);

  useEffect(() => {
    let off = false;
    api.listModels()
      .then((list) => { if (!off) setModels(Array.isArray(list) ? list : []); })
      .catch(() => {});
    return () => { off = true; };
  }, []);

  const onChange = async (e) => {
    if (!sessionId) { e.preventDefault(); return; }
    const next = e.target.value;
    const prev = value;
    setValue(next); setBusy(true);
    try {
      await api.switchModel(sessionId, next);
      toast({ kind: 'ok', text: '已切换至 ' + next });
    } catch (err) {
      setValue(prev);
      toast({ kind: 'err', text: '切换失败:' + (err.message || '') });
    } finally {
      setBusy(false);
    }
  };

  return (
    <select
      value={value}
      onChange={onChange}
      disabled={busy || !sessionId}
      className="h-7 px-2 pr-6 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-60 disabled:cursor-not-allowed"
    >
      {models.length === 0 && <option value="">—</option>}
      {models.map((m) => (
        <option key={m.name} value={m.name}>
          {m.name}{m.is_legacy ? ' (legacy)' : ''}
        </option>
      ))}
    </select>
  );
}
