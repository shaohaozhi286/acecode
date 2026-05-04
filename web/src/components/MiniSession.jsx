// 宫格里的迷你会话卡。使用共享 transcript 投影渲染 compact viewport,
// active/running 会话实时更新;inactive 磁盘历史只显示静态历史。

import { useMemo } from 'react';
import { clsx } from '../lib/format.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { projectCompactTranscriptItems, useSessionTranscript } from '../lib/sessionTranscript.js';
import { VsIcon } from './Icon.jsx';

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

function compactText(value, max) {
  const text = String(value || '').replace(/\s+/g, ' ').trim();
  if (text.length <= max) return text;
  return text.slice(0, Math.max(0, max - 1)) + '…';
}

function MiniMessageItem({ item, compact }) {
  const max = compact ? 42 : 90;
  const content = compactText(item.content || (item.streaming ? '正在输出…' : ''), max);
  if (!content) return null;
  return (
    <div
      className={clsx(
        'rounded leading-tight px-1 py-0.5 break-words line-clamp-2',
        compact ? 'text-[8px]' : 'text-[9px]',
        item.role === 'user'
          ? 'self-end max-w-[76%] bg-accent-bg border border-accent-soft text-fg'
          : 'self-start max-w-full text-fg-2 bg-surface-alt/50',
        item.streaming && 'border border-accent-soft text-accent',
      )}
      title={item.content || ''}
    >
      {content}
    </div>
  );
}

function toolSummaryText(tool) {
  if (!tool) return '';
  if (tool.isTaskComplete) return `Done · ${tool.summary?.object || '完成'}`;
  if (!tool.isDone) return tool.title || tool.displayOverride || tool.tool || '工具运行中';
  const summary = tool.summary || {};
  return [summary.verb, summary.object].filter(Boolean).join(' · ') || tool.title || tool.tool || '工具完成';
}

function MiniToolItem({ item, compact }) {
  const tool = item.tool || {};
  const running = !tool.isDone && !tool.isTaskComplete;
  const ok = tool.isTaskComplete || tool.success !== false;
  const tail = Array.isArray(tool.tailLines) ? tool.tailLines.slice(-2).join(' ') : '';
  const output = !ok && tool.output ? tool.output.split('\n').slice(0, 1).join(' ') : '';
  const detail = compactText(tool.currentPartial || tail || output, compact ? 44 : 84);
  return (
    <div
      className={clsx(
        'w-full rounded border px-1 py-0.5 font-mono leading-tight',
        compact ? 'text-[7px]' : 'text-[8px]',
        running && 'border-border bg-surface-alt text-fg-2',
        !running && ok && 'border-ok-border bg-ok-bg text-ok',
        !running && !ok && 'border-danger/30 bg-danger-bg text-danger',
      )}
      title={tool.title || tool.displayOverride || tool.tool || ''}
    >
      <div className="flex items-center gap-1 min-w-0">
        {running && <span className="ace-spinner w-2.5 h-2.5 shrink-0" />}
        <span className="truncate">{compactText(toolSummaryText(tool), compact ? 38 : 76)}</span>
      </div>
      {detail && <div className="text-fg-mute truncate mt-px">{detail}</div>}
    </div>
  );
}

function MiniTranscriptItem({ item, compact }) {
  if (item.kind === 'tool') return <MiniToolItem item={item} compact={compact} />;
  return <MiniMessageItem item={item} compact={compact} />;
}

export function MiniSession({ session, compact, onClick }) {
  const sessionRef = useMemo(() => ({
    sessionId: session.sessionId || session.id,
    id: session.id,
    active: !!session.active,
    busy: !!session.busy,
    status: session.status || 'idle',
    workspaceHash: session.workspaceHash || session.workspace_hash || '',
    port: session.port,
    token: session.token,
    cwd: session.cwd,
    title: session.title,
    summary: session.summary,
  }), [
    session.active,
    session.busy,
    session.cwd,
    session.id,
    session.port,
    session.sessionId,
    session.status,
    session.summary,
    session.title,
    session.token,
    session.workspaceHash,
    session.workspace_hash,
  ]);
  const transcript = useSessionTranscript(sessionRef, { live: 'auto' });
  const shownItems = useMemo(
    () => projectCompactTranscriptItems(transcript.items, compact ? 5 : 7),
    [compact, transcript.items],
  );
  const live = transcript.isLive;
  const active = transcript.busy || session.status === 'running' || transcript.status === 'running';
  const stateLabel = active ? 'running' : session.status;

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
        <span className={clsx('rounded-full shrink-0', compact ? 'w-1.5 h-1.5' : 'w-[7px] h-[7px]', statusColor(stateLabel))} />
        <span className={clsx('flex-1 truncate font-semibold text-fg', compact ? 'text-[10px]' : 'text-[11px]')}>
          {sessionDisplayTitle(session, session.id)}
        </span>
        <span className={clsx(compact ? 'text-[8px]' : 'text-[9px]', live ? 'text-ok' : 'text-fg-mute')}>
          {live ? '实时' : '静态'}
        </span>
      </div>

      <div className={clsx('flex-1 overflow-hidden flex flex-col', compact ? 'p-1 gap-px' : 'p-1.5 gap-0.5')}>
        {shownItems.length === 0 ? (
          <>
            <div className="self-end w-3/5 h-3 rounded bg-accent-bg border border-accent-soft" />
            <div className="w-2/5 h-1 rounded bg-border opacity-50" />
            <div className="w-1/2 h-1 rounded bg-border opacity-50" />
          </>
        ) : (
          shownItems.map((item) => (
            <MiniTranscriptItem key={item.id} item={item} compact={compact} />
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
          )}><VsIcon name="send" size={compact ? 10 : 12} mono={false} className="ace-icon-on-accent" /></span>
        </div>
      </div>
      <div className={clsx('px-1.5 py-px text-fg-mute border-t border-border shrink-0', compact ? 'text-[7px]' : 'text-[8px]')}>
        {session.model || '—'} · {transcript.turns || session.turns || 0}轮 · {statusLabel(stateLabel)}
      </div>
    </button>
  );
}
