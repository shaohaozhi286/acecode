// 单条消息渲染:user 气泡(右)/ assistant 头像+正文(左)/ system 灰条。
// assistant 走 markdown-it 渲染(见 lib/markdown.js)。
//
// hover actions(codex 风格):每条 user / assistant 消息悬停时浮出
// 右上角的复制 + 分叉按钮。复制走 navigator.clipboard.writeText;分叉
// 调上层 onFork(messageId) — disabled 当 messageId 缺失。

import { memo } from 'react';
import { renderMarkdown } from '../lib/markdown.js';
import { relativeTime, clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

function HoverActions({ messageId, getCopyText, onFork }) {
  const handleCopy = async () => {
    try {
      const text = getCopyText();
      if (!navigator.clipboard) throw new Error('clipboard unavailable');
      await navigator.clipboard.writeText(text);
      toast({ kind: 'ok', text: '已复制' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  };
  const handleFork = () => {
    if (!messageId) return;
    onFork?.(messageId);
  };
  return (
    <div className="ace-msg-actions absolute top-1 right-1 flex gap-0.5">
      <button type="button" onClick={handleCopy} title="复制">
        <VsIcon name="copy" size={13} />
      </button>
      <button
        type="button"
        onClick={handleFork}
        disabled={!messageId}
        title={messageId ? '分叉到新会话' : '此消息不可分叉(无 ID)'}
      >
        <VsIcon name="fork" size={13} />
      </button>
    </div>
  );
}

function UserBubble({ content, ts, messageId, onFork }) {
  return (
    <div className="self-end max-w-[70%] flex flex-col items-end gap-0.5">
      <div className="group relative px-3.5 py-2 pr-10 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
        {content}
        <HoverActions
          messageId={messageId}
          getCopyText={() => content}
          onFork={onFork}
        />
      </div>
      {ts != null && <span className="text-[10px] text-fg-mute mr-1">{relativeTime(ts)}</span>}
    </div>
  );
}

function AssistantBubble({ content, ts, streaming, messageId, onFork }) {
  const html = { __html: renderMarkdown(content || '') };
  return (
    <div className="flex gap-2 max-w-[88%] group relative">
      <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center shrink-0 mt-[2px]">A</div>
      <div className="flex-1 min-w-0 flex flex-col gap-1">
        <div className="text-[12px] font-semibold text-fg flex items-center gap-1.5">
          ACECode
          {ts != null && <span className="text-[10px] text-fg-mute font-normal">{relativeTime(ts)}</span>}
        </div>
        <div
          className={clsx(
            'ace-md text-[13px] text-fg leading-[1.6] py-0.5',
            streaming && 'ace-cursor',
          )}
          dangerouslySetInnerHTML={html}
        />
      </div>
      {!streaming && (
        <HoverActions
          messageId={messageId}
          getCopyText={() => content || ''}
          onFork={onFork}
        />
      )}
    </div>
  );
}

function SystemRow({ content }) {
  return (
    <div className="self-stretch px-3 py-1.5 bg-surface-alt border border-dashed border-border rounded-md text-[12px] text-fg-2 whitespace-pre-wrap">
      {content}
    </div>
  );
}

export const Message = memo(function Message({
  role, content, ts, streaming, messageId, onFork,
}) {
  if (role === 'user') {
    return <UserBubble content={content} ts={ts}
                        messageId={messageId} onFork={onFork} />;
  }
  if (role === 'assistant') {
    return <AssistantBubble content={content} ts={ts} streaming={streaming}
                             messageId={messageId} onFork={onFork} />;
  }
  return <SystemRow content={content} />;
});
