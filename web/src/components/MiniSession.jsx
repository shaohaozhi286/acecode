// 宫格里的迷你会话卡。状态点 + 标题 + 占位消息预览 + 迷你输入框。
//
// v1 不连 ws,只展示静态摘要(取最近一条消息的前 80 字);后续可加 lazy WS bind
// 让活动会话有实时打字预览。

import { useEffect, useMemo, useState } from 'react';
import { createApi } from '../lib/api.js';
import { clsx } from '../lib/format.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';

function statusColor(s) {
  if (s === 'running') return 'bg-ok shadow-[0_0_6px_var(--ace-ok)]';
  if (s === 'waiting') return 'bg-warn';
  return 'bg-fg-mute';
}
function statusLabel(s) {
  if (s === 'running') return '运行中';
  if (s === 'waiting') return '等待输入';
  return '空闲';
}

export function MiniSession({ session, compact, onClick }) {
  const [preview, setPreview] = useState([]);
  const api = useMemo(() => createApi(session), [session.port, session.token]);

  useEffect(() => {
    let off = false;
    api.getMessages(session.id, 0)
      .then((data) => {
        if (off) return;
        const msgs = (data && data.messages) || [];
        const last = msgs.slice(-4);
        setPreview(last);
      })
      .catch(() => {});
    return () => { off = true; };
  }, [session.id, api]);

  const active = session.status === 'running';

  return (
    <button
      type="button"
      onClick={() => onClick?.(session)}
      className={clsx(
        'flex flex-col rounded-lg overflow-hidden text-left transition cursor-pointer',
        'border bg-surface',
        active ? 'border-accent border-[1.5px] shadow-[0_0_0_1px_var(--ace-accent)/0.15]' : 'border-border ace-shadow hover:border-accent/60',
      )}
    >
      <div className={clsx(
        'flex items-center gap-1.5 px-2.5 py-1.5 border-b border-border shrink-0',
        active ? 'bg-accent-bg' : 'bg-surface-alt',
      )}>
        <span className={clsx('rounded-full shrink-0', compact ? 'w-1.5 h-1.5' : 'w-[7px] h-[7px]', statusColor(session.status))} />
        <span className={clsx('flex-1 truncate font-semibold text-fg', compact ? 'text-[10px]' : 'text-[11px]')}>
          {sessionDisplayTitle(session, session.id)}
        </span>
        <span className={clsx(compact ? 'text-[8px]' : 'text-[9px]', active ? 'text-ok' : 'text-fg-mute')}>
          {statusLabel(session.status)}
        </span>
      </div>

      <div className={clsx('flex-1 overflow-hidden flex flex-col', compact ? 'p-1 gap-px' : 'p-1.5 gap-0.5')}>
        {preview.length === 0 ? (
          <>
            <div className="self-end w-3/5 h-3 rounded bg-accent-bg border border-accent-soft" />
            <div className="w-2/5 h-1 rounded bg-border opacity-50" />
            <div className="w-1/2 h-1 rounded bg-border opacity-50" />
          </>
        ) : (
          preview.map((m, i) => (
            <div
              key={i}
              className={clsx(
                'rounded text-[9px] leading-tight px-1 py-0.5 truncate',
                m.role === 'user'
                  ? 'self-end max-w-[70%] bg-accent-bg border border-accent-soft text-fg'
                  : 'text-fg-2',
              )}
            >
              {(m.content || '').slice(0, compact ? 30 : 60)}
            </div>
          ))
        )}
        {active && (
          <div className="flex gap-0.5 px-1 mt-auto">
            {[0, 1, 2].map((i) => (
              <span
                key={i}
                className="w-1 h-1 rounded-full bg-fg-mute"
                style={{ animation: `ace-pulse 1.2s ease-in-out ${i * 0.2}s infinite` }}
              />
            ))}
          </div>
        )}
      </div>

      <div className={clsx('border-t border-border shrink-0', compact ? 'p-1' : 'p-1.5')}>
        <div className={clsx(
          'flex items-center gap-1 rounded border border-border bg-surface',
          compact ? 'h-[18px] px-1.5' : 'h-[22px] px-2',
        )}>
          <span className={clsx('flex-1 text-fg-mute', compact ? 'text-[8px]' : 'text-[9px]')}>
            输入消息…
          </span>
          <span className={clsx(
            'rounded-full bg-accent text-white flex items-center justify-center',
            compact ? 'w-3.5 h-3.5 text-[8px]' : 'w-4 h-4 text-[10px]',
          )}>↑</span>
        </div>
      </div>
      <div className={clsx('px-1.5 py-px text-fg-mute border-t border-border shrink-0', compact ? 'text-[7px]' : 'text-[8px]')}>
        {session.model || '—'} · {session.turns || 0}轮
      </div>
    </button>
  );
}
