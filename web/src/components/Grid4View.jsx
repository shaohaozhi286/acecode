// 4 宫格视图:取当前 daemon 最多 4 条 sessions 显示。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { MiniSession } from './MiniSession.jsx';

export function Grid4View({ onExpand }) {
  const [sessions, setSessions] = useState([]);

  useEffect(() => {
    let off = false;
    const tick = () => {
      api.listSessions()
        .then((list) => { if (!off) setSessions(Array.isArray(list) ? list.slice(0, 4) : []); })
        .catch(() => {});
    };
    tick();
    const t = setInterval(tick, 3000);
    return () => { off = true; clearInterval(t); };
  }, []);

  return (
    <div className="flex-1 overflow-hidden bg-bg p-1.5 grid grid-cols-2 grid-rows-2 gap-1.5">
      {sessions.length === 0 ? (
        <div className="col-span-2 row-span-2 flex items-center justify-center text-fg-mute text-sm">
          暂无会话 — 切到「单会话」新建第一个
        </div>
      ) : (
        sessions.map((s) => (
          <MiniSession key={s.id} session={s} onClick={onExpand} />
        ))
      )}
      {sessions.length > 0 && Array.from({ length: Math.max(0, 4 - sessions.length) }).map((_, i) => (
        <div
          key={`empty-${i}`}
          className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex items-center justify-center text-fg-mute text-[11px]"
        >
          空闲
        </div>
      ))}
    </div>
  );
}
