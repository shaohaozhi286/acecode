// MCP 面板:JSON 编辑器 + 客户端校验 + 保存。
// daemon v1 不支持热重载,保存后给提示需要重启。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { SlideOver } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';

export function MCPPanel({ onClose }) {
  const [text,    setText]    = useState('');
  const [err,     setErr]     = useState('');
  const [saving,  setSaving]  = useState(false);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    let off = false;
    api.getMcp()
      .then((cfg) => { if (!off) setText(JSON.stringify(cfg || {}, null, 2)); })
      .catch((e) => toast({ kind: 'err', text: '加载 MCP 失败:' + (e.message || '') }))
      .finally(() => { if (!off) setLoading(false); });
    return () => { off = true; };
  }, []);

  const onChange = (v) => {
    setText(v);
    try { JSON.parse(v); setErr(''); }
    catch (e) { setErr(e.message || 'JSON 语法错误'); }
  };

  const save = async () => {
    if (err) return;
    setSaving(true);
    try {
      const obj = JSON.parse(text);
      await api.putMcp(obj);
      toast({ kind: 'ok', text: '已保存。需重启 daemon 才能生效' });
    } catch (e) {
      toast({ kind: 'err', text: '保存失败:' + (e.message || '') });
    } finally {
      setSaving(false);
    }
  };

  const reload = async () => {
    try { const r = await api.reloadMcp(); toast({ kind: 'ok', text: 'Reload: ' + JSON.stringify(r) }); }
    catch (e) { toast({ kind: 'err', text: 'v1 不支持热重载,需重启 daemon' }); }
  };

  return (
    <SlideOver width={460} onClose={onClose}>
      {({ close }) => (
        <>
          <div className="h-11 px-4 flex items-center bg-surface-alt border-b border-border shrink-0">
            <span className="flex-1 text-[14px] font-semibold">🔌 MCP Servers</span>
            <button
              type="button"
              onClick={close}
              className="w-7 h-7 rounded text-fg-mute hover:text-fg hover:bg-surface-hi text-[16px] transition"
            >✕</button>
          </div>
          <div className="flex-1 flex flex-col p-3 gap-2 overflow-hidden">
            <div className="text-[11px] text-fg-mute">配置 JSON(直接编辑 mcp_servers 部分)</div>
            <textarea
              value={text}
              onChange={(e) => onChange(e.target.value)}
              spellCheck={false}
              disabled={loading}
              className={clsx(
                'flex-1 w-full p-3 text-[12px] font-mono leading-[1.55] rounded-md border bg-surface-alt outline-none transition resize-none',
                err ? 'border-danger focus:border-danger' : 'border-border focus:border-accent',
              )}
              placeholder={loading ? '加载中…' : '{\n  "mcp_servers": { ... }\n}'}
            />
            {err && <div className="text-danger text-[11px]">{err}</div>}
            <div className="flex gap-2">
              <button
                type="button"
                onClick={save}
                disabled={!!err || saving || loading}
                className="flex-1 h-8 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 disabled:opacity-50 disabled:cursor-not-allowed transition"
              >
                {saving ? '保存中…' : '保存'}
              </button>
              <button
                type="button"
                onClick={reload}
                className="px-3 h-8 rounded-md bg-surface-hi text-fg-2 text-[12px] hover:bg-border transition"
              >
                Reload
              </button>
            </div>
          </div>
        </>
      )}
    </SlideOver>
  );
}
