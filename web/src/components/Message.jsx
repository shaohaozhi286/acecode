// 单条消息渲染:user 气泡(右)/ assistant 头像+正文(左)/ system 灰条。
// assistant 走 markdown-it 渲染(见 lib/markdown.js)。
//
// hover actions(codex 风格):user 消息和 assistant run 最后一条消息悬停时浮出
// 消息底部同侧的复制 + 分叉按钮(左消息在左下角,右消息在右下角)。复制走 navigator.clipboard.writeText;分叉
// 调上层 onFork(messageId) — disabled 当 messageId 缺失。

import { memo, useCallback, useMemo, useState } from 'react';
import { renderMarkdown } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { relativeTime } from '../lib/format.js';
import { buildCompactMessagePreview } from '../lib/compactMessagePreview.js';
import { assistantChromeState } from '../lib/assistantAvatarDisplay.js';
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
        <VsIcon name="copy" size={14} />
      </button>
      <button
        type="button"
        onClick={handleFork}
        disabled={!messageId}
        title={messageId ? '分叉到新会话' : '此消息不可分叉(无 ID)'}
      >
        <VsIcon name="fork" size={14} />
      </button>
    </div>
  );
}

function UserBubble({ content, ts, messageId, onFork }) {
  return (
    <div className="self-end max-w-[70%] flex flex-col items-end gap-0.5 group">
      <div className="px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
        {content}
      </div>
      <div className="min-h-6 flex items-center justify-end gap-1 mr-1">
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

function AssistantBubble({
  content,
  ts,
  streaming,
  messageId,
  onFork,
  continuation,
  showFooter,
  showAceCodeAvatar,
}) {
  const html = { __html: renderMarkdown(content || '') };
  const chrome = assistantChromeState({ showAceCodeAvatar, continuation });
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
  // continuation = true: 同一个 assistant run 中的非首条, 不重复显示头像 + ACECode 名牌,
  // 用一个等宽空白占位让正文与首条对齐(头像宽 6 + gap-2 = 总 32px, 与 flex gap 一致)。
  return (
    <div className={`flex ${chrome.gapClass} max-w-[88%] group relative`}>
      {chrome.showAvatarPlaceholder ? (
        <div className="w-6 shrink-0" aria-hidden="true" />
      ) : chrome.showAvatar ? (
        <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center shrink-0 mt-[2px]">A</div>
      ) : (
        null
      )}
      <div className="flex-1 min-w-0 flex flex-col gap-1">
        {chrome.showName && (
          <div className="text-[12px] font-semibold text-fg flex items-center gap-1.5">
            ACECode
          </div>
        )}
        <div
          className="ace-md text-[13px] text-fg leading-[1.6] py-0.5"
          onClick={handleMarkdownClick}
          dangerouslySetInnerHTML={html}
        />
        {showFooter && (
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
        )}
      </div>
    </div>
  );
}

function SystemRow({ role, content }) {
  const [expanded, setExpanded] = useState(false);
  const { label, text, preview, lineCount, charCount } = useMemo(
    () => buildCompactMessagePreview({ role, content }),
    [role, content],
  );

  return (
    <div className="self-stretch bg-surface-alt border border-dashed border-border rounded-md text-[12px] text-fg-2 overflow-hidden">
      <button
        type="button"
        className="w-full px-3 py-1.5 flex items-center gap-2 text-left text-fg-mute hover:text-fg hover:bg-surface-hi transition"
        title={preview}
        onClick={() => setExpanded((v) => !v)}
      >
        <span className="font-medium shrink-0">{label}</span>
        <span className="text-[10px] flex-1 truncate font-mono">{preview}</span>
        <span className="text-[10px] shrink-0 opacity-70">
          {lineCount > 1 ? `${lineCount} 行` : `${charCount} 字符`}
        </span>
        <span className="text-[10px] shrink-0 flex items-center gap-1">
          {expanded ? '收起' : '展开'}
          <VsIcon name={expanded ? 'glyphUp' : 'glyphDown'} size={9} />
        </span>
      </button>
      {expanded && (
        <CopyableCodeFrame text={text} className="ace-system-copy-frame">
          <div
            className="px-3 pb-2 pt-1 whitespace-pre-wrap break-words"
            data-code-copy-source="true"
          >
            {text}
          </div>
        </CopyableCodeFrame>
      )}
    </div>
  );
}

export const Message = memo(function Message({
  role,
  content,
  ts,
  streaming,
  messageId,
  metadata,
  onFork,
  continuation,
  showFooter = true,
  showAceCodeAvatar = true,
}) {
  if (role === 'user') {
    // expand-webui-skill-commands:daemon 把 /<skill> args 在送给 LLM 前展开为
    // 轻量提示;原文存到 metadata.display_text,UI 优先显示原文,不让用户看到
    // 内部展开。
    const displayContent = (metadata && typeof metadata.display_text === 'string'
                              && metadata.display_text.length > 0)
      ? metadata.display_text
      : content;
    return <UserBubble content={displayContent} ts={ts}
                        messageId={messageId}
                        onFork={onFork} />;
  }
  if (role === 'assistant') {
    return <AssistantBubble content={content} ts={ts} streaming={streaming}
                             messageId={messageId} onFork={onFork}
                             continuation={continuation}
                             showFooter={showFooter}
                             showAceCodeAvatar={showAceCodeAvatar} />;
  }
  return <SystemRow role={role} content={content} />;
});
