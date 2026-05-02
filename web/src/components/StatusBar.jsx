// 底部 22px 状态栏:权限模式下拉 + 模型 tag + 轮次 + 分支(占位)
//
// 权限模式只是 UI 状态,实际权限决策仍在 daemon 端;切换会通过 (TODO) 一个
// future API 同步,v1 仅本地反映。

import { useEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

const MODES = [
  { id: 'default',     label: '默认',          hint: '写/执行操作前确认',                  color: 'ok'     },
  { id: 'acceptEdits', label: '自动接受编辑',   hint: '文件编辑自动通过,命令仍确认',        color: 'warn'   },
  { id: 'yolo',        label: 'Yolo',         hint: '跳过所有确认',                       color: 'danger' },
];

export function StatusBar({ model = '—', turns = 0, branch = '' }) {
  const [mode, setMode] = useState('default');
  const [open, setOpen] = useState(false);
  const ref = useRef(null);

  useEffect(() => {
    if (!open) return;
    const onDoc = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(false); };
    document.addEventListener('mousedown', onDoc);
    return () => document.removeEventListener('mousedown', onDoc);
  }, [open]);

  const cur = MODES.find((m) => m.id === mode) || MODES[0];
  const dotCls = cur.color === 'ok' ? 'bg-ok' : cur.color === 'warn' ? 'bg-warn' : 'bg-danger';

  return (
    <div className="h-[22px] flex items-center px-2.5 gap-4 bg-surface-alt border-t border-border text-[11px] text-fg-mute shrink-0">
      <div ref={ref} className="relative">
        <button
          type="button"
          onClick={() => setOpen((o) => !o)}
          title={cur.hint}
          className="flex items-center gap-1 px-1.5 py-px rounded hover:bg-surface-hi text-fg-mute transition"
        >
          <span className={clsx('w-1.5 h-1.5 rounded-full', dotCls)} />
          <span>{cur.label}</span>
          <VsIcon name="glyphDown" size={9} className="opacity-70" />
        </button>
        {open && (
          <div className="absolute bottom-full left-0 mb-1 bg-surface border border-border rounded-md ace-shadow-lg p-1 min-w-[220px] z-50">
            {MODES.map((m) => {
              const active = m.id === mode;
              const c = m.color === 'ok' ? 'bg-ok' : m.color === 'warn' ? 'bg-warn' : 'bg-danger';
              return (
                <button
                  key={m.id}
                  type="button"
                  onClick={() => { setMode(m.id); setOpen(false); }}
                  className={clsx(
                    'w-full text-left px-2.5 py-1.5 rounded transition flex flex-col gap-0.5',
                    active ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                  )}
                >
                  <span className="flex items-center gap-1.5">
                    <span className={clsx('w-1.5 h-1.5 rounded-full', c)} />
                    <span className={clsx('text-xs', active ? 'text-accent font-semibold' : 'text-fg')}>{m.label}</span>
                  </span>
                  <span className="text-[10px] text-fg-mute pl-3">{m.hint}</span>
                </button>
              );
            })}
          </div>
        )}
      </div>
      <span className="px-1.5 py-px rounded bg-surface-hi text-[10px]">{model}</span>
      <span>{turns} 轮次</span>
      {branch && <span className="ml-auto truncate max-w-[40%]">{branch}</span>}
      {!branch && <span className="ml-auto" />}
    </div>
  );
}
