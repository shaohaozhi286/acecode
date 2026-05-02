// 单条消息渲染:user 气泡(右)/ assistant 头像+正文(左)/ system 灰条。
// assistant 走 markdown 渲染(见 lib/markdown.js — 不依赖外部库)。

import { memo } from 'react';
import { renderMarkdown } from '../lib/markdown.js';
import { relativeTime, clsx } from '../lib/format.js';

function UserBubble({ content, ts }) {
  return (
    <div className="self-end max-w-[70%] flex flex-col items-end gap-0.5">
      <div className="px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
        {content}
      </div>
      {ts != null && <span className="text-[10px] text-fg-mute mr-1">{relativeTime(ts)}</span>}
    </div>
  );
}

function AssistantBubble({ content, ts, streaming }) {
  const html = { __html: renderMarkdown(content || '') };
  return (
    <div className="flex gap-2 max-w-[88%]">
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

export const Message = memo(function Message({ role, content, ts, streaming }) {
  if (role === 'user')      return <UserBubble  content={content} ts={ts} />;
  if (role === 'assistant') return <AssistantBubble content={content} ts={ts} streaming={streaming} />;
  return <SystemRow content={content} />;
});
