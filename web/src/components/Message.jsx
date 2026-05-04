// 单条消息渲染:user 气泡(右)/ assistant 头像+正文(左)/ system 灰条。
// assistant 走 markdown-it 渲染(见 lib/markdown.js)。
//
// hover actions(codex 风格):每条 user / assistant 消息悬停时浮出
// 消息底部同侧的复制 + 分叉按钮(左消息在左下角,右消息在右下角)。复制走 navigator.clipboard.writeText;分叉
// 调上层 onFork(messageId) — disabled 当 messageId 缺失。

import { memo, useCallback, useMemo, useState } from 'react';
import { renderMarkdown } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { relativeTime, clsx } from '../lib/format.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

function HoverActions({ messageId, getCopyText, onFork }) {
  const handleCopy = async (event) => {
    event.stopPropagation();
    try {
      const text = getCopyText();
      if (!navigator.clipboard) throw new Error('clipboard unavailable');
      await navigator.clipboard.writeText(text);
      toast({ kind: 'ok', text: '已复制' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  };
  const handleFork = (event) => {
    event.stopPropagation();
    if (!messageId) return;
    onFork?.(messageId);
  };
  return (
    <div className="ace-msg-actions flex gap-0.5">
      <button type="button" onClick={handleCopy} title="复制">
        <VsIcon name="copy" size={17} />
      </button>
      <button
        type="button"
        onClick={handleFork}
        disabled={!messageId}
        title={messageId ? '分叉到新会话' : '此消息不可分叉(无 ID)'}
      >
        <VsIcon name="fork" size={17} />
      </button>
    </div>
  );
}

function queuedStatusLabel(state) {
  if (state === 'sending') return '发送中';
  if (state === 'failed') return '发送失败';
  return '排队中';
}

function UserBubble({ content, ts, messageId, queued, onCancelQueued, onRetryQueued, onFork }) {
  const queuedId = queued?.id || '';
  const queuedState = queued?.state || '';
  const canCancelQueued = queuedId && queuedState === 'queued';
  const canRetryQueued = queuedId && queuedState === 'failed';
  return (
    <div className="self-end max-w-[70%] flex flex-col items-end gap-0.5 group" data-queued-state={queuedState || undefined}>
      <div className="px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
        {content}
      </div>
      <div className="min-h-6 flex items-center justify-end gap-1 mr-1">
        {queued && (
          <span
            className={clsx(
              'text-[10px] rounded-full px-1.5 h-5 inline-flex items-center border',
              queuedState === 'failed'
                ? 'text-danger border-danger/30 bg-danger-bg'
                : 'text-fg-mute border-border bg-surface-hi',
            )}
            title={queued.error || queuedStatusLabel(queuedState)}
          >
            {queuedStatusLabel(queuedState)}
          </span>
        )}
        {canRetryQueued && (
          <button
            type="button"
            className="text-[10px] text-accent hover:underline px-1"
            onClick={(event) => {
              event.stopPropagation();
              onRetryQueued?.(queuedId);
            }}
          >
            重试
          </button>
        )}
        {(canCancelQueued || canRetryQueued) && (
          <button
            type="button"
            className="text-[10px] text-fg-mute hover:text-fg px-1"
            onClick={(event) => {
              event.stopPropagation();
              onCancelQueued?.(queuedId);
            }}
          >
            取消
          </button>
        )}
        {ts != null && <span className="text-[10px] text-fg-mute">{relativeTime(ts)}</span>}
        <HoverActions
          messageId={messageId}
          getCopyText={() => content}
          onFork={onFork}
        />
      </div>
    </div>
  );
}

function AssistantBubble({ content, ts, streaming, messageId, onFork }) {
  const html = { __html: renderMarkdown(content || '') };
  const handleMarkdownClick = useCallback(async (event) => {
    const text = codeTextFromCopyButtonTarget(event.target);
    if (text == null) return;
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  }, []);
  return (
    <div className="flex gap-2 max-w-[88%] group relative">
      <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center shrink-0 mt-[2px]">A</div>
      <div className="flex-1 min-w-0 flex flex-col gap-1">
        <div className="text-[12px] font-semibold text-fg flex items-center gap-1.5">
          ACECode
        </div>
        <div
          className={clsx(
            'ace-md text-[13px] text-fg leading-[1.6] py-0.5',
            streaming && 'ace-cursor',
          )}
          onClick={handleMarkdownClick}
          dangerouslySetInnerHTML={html}
        />
        <div className="min-h-6 flex items-center gap-1">
          {!streaming && (
            <HoverActions
              messageId={messageId}
              getCopyText={() => content || ''}
              onFork={onFork}
            />
          )}
          {ts != null && <span className="text-[10px] text-fg-mute font-normal">{relativeTime(ts)}</span>}
        </div>
      </div>
    </div>
  );
}

const SYSTEM_COLLAPSE_LINES = 6;
const SYSTEM_COLLAPSE_CHARS = 900;

function buildSystemPreview(content) {
  const text = String(content || '');
  const lines = text.split('\n');
  const byLines = lines.length > SYSTEM_COLLAPSE_LINES;
  const byChars = text.length > SYSTEM_COLLAPSE_CHARS;
  if (!byLines && !byChars) {
    return { text, preview: text, collapsible: false, hiddenLines: 0 };
  }

  let preview = byLines
    ? lines.slice(0, SYSTEM_COLLAPSE_LINES).join('\n')
    : text.slice(0, SYSTEM_COLLAPSE_CHARS);
  if (preview.length > SYSTEM_COLLAPSE_CHARS) {
    preview = preview.slice(0, SYSTEM_COLLAPSE_CHARS);
  }
  return {
    text,
    preview: `${preview.trimEnd()}\n...`,
    collapsible: true,
    hiddenLines: Math.max(0, lines.length - SYSTEM_COLLAPSE_LINES),
  };
}

function SystemRow({ role, content }) {
  const [expanded, setExpanded] = useState(false);
  const { text, preview, collapsible, hiddenLines } = useMemo(
    () => buildSystemPreview(content),
    [content],
  );
  const label = role === 'tool' ? '工具信息' : '系统信息';
  const shownText = expanded || !collapsible ? text : preview;

  return (
    <div className="self-stretch bg-surface-alt border border-dashed border-border rounded-md text-[12px] text-fg-2 overflow-hidden">
      {collapsible && (
        <button
          type="button"
          className="w-full px-3 py-1.5 flex items-center gap-2 text-left text-fg-mute hover:text-fg hover:bg-surface-hi transition"
          onClick={() => setExpanded((v) => !v)}
        >
          <span className="font-medium">{label}</span>
          <span className="text-[10px] flex-1 truncate">
            {expanded ? '已展开' : hiddenLines > 0 ? `已折叠 ${hiddenLines} 行` : '已折叠长内容'}
          </span>
          <span className="text-[10px] flex items-center gap-1">
            {expanded ? '收起' : '展开'}
            <VsIcon name={expanded ? 'glyphUp' : 'glyphDown'} size={9} />
          </span>
        </button>
      )}
      <CopyableCodeFrame text={text} className="ace-system-copy-frame">
        <div
          className={clsx('px-3 whitespace-pre-wrap break-words', collapsible ? 'pb-2 pt-1' : 'py-1.5')}
          data-code-copy-source="true"
        >
          {shownText}
        </div>
      </CopyableCodeFrame>
    </div>
  );
}

export const Message = memo(function Message({
  role, content, ts, streaming, messageId, queued, onCancelQueued, onRetryQueued, onFork,
}) {
  if (role === 'user') {
    return <UserBubble content={content} ts={ts}
                        messageId={messageId} queued={queued}
                        onCancelQueued={onCancelQueued} onRetryQueued={onRetryQueued}
                        onFork={onFork} />;
  }
  if (role === 'assistant') {
    return <AssistantBubble content={content} ts={ts} streaming={streaming}
                             messageId={messageId} onFork={onFork} />;
  }
  return <SystemRow role={role} content={content} />;
});
