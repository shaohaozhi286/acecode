// 极简 toast:模块级订阅,任何地方 import { toast } 触发,Toaster 是渲染容器。
// 不引入 react-hot-toast 等依赖。

import { useEffect, useState } from 'react';
import { clsx } from '../lib/format.js';

const listeners = new Set();
let nextId = 1;

export function toast({ kind = 'info', text, duration = 3500 }) {
  const id = nextId++;
  const item = { id, kind, text, duration };
  for (const fn of listeners) fn({ type: 'add', item });
  if (duration > 0) {
    setTimeout(() => {
      for (const fn of listeners) fn({ type: 'remove', id });
    }, duration);
  }
  return id;
}

export function Toaster() {
  const [items, setItems] = useState([]);

  useEffect(() => {
    const handler = (evt) => {
      if (evt.type === 'add')    setItems((prev) => [...prev, evt.item]);
      if (evt.type === 'remove') setItems((prev) => prev.filter((x) => x.id !== evt.id));
    };
    listeners.add(handler);
    return () => listeners.delete(handler);
  }, []);

  return (
    <div className="fixed bottom-4 right-4 z-[1000] flex flex-col gap-2 pointer-events-none">
      {items.map((it) => (
        <div
          key={it.id}
          className={clsx(
            'pointer-events-auto px-3.5 py-2.5 rounded-lg text-xs font-medium border shadow-lg',
            'min-w-[200px] max-w-[360px] break-words',
            'animate-[ace-pulse_.18s_ease-out]',
            it.kind === 'ok'  && 'bg-ok-bg text-ok border-ok-border',
            it.kind === 'err' && 'bg-danger-bg text-danger border-danger/30',
            it.kind === 'info'&& 'bg-surface text-fg border-border',
          )}
          role="status"
        >
          {it.text}
        </div>
      ))}
    </div>
  );
}
