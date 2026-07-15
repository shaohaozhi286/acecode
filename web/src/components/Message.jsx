// 单条消息渲染:user 气泡(右)/ assistant 正文(左)/ system 灰条。
// assistant 走 markdown-it 渲染(见 lib/markdown.js)。
//
// hover actions(codex 风格):user 消息和 assistant run 最后一条消息悬停时浮出
// 消息底部同侧的复制 + 分叉按钮(左消息在左下角,右消息在右下角)。复制走 navigator.clipboard.writeText;分叉
// 调上层 onFork(messageId) — disabled 当 messageId 缺失。

import { memo, useCallback, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { renderMarkdownBlocks } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { clsx, relativeTime } from '../lib/format.js';
import { buildCompactMessagePreview } from '../lib/compactMessagePreview.js';
import { assistantChromeState } from '../lib/assistantAvatarDisplay.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { VsIcon, CommandGlyph } from './Icon.jsx';
import { toast } from './Toast.jsx';
import { resolveLeadingSlashCommand } from '../lib/slashCommands.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { AttachmentStrip } from './AttachmentStrip.jsx';

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

// 斜杠命令徽标:skill 图标 + 蓝色命令名,hover 浮出描述。
// 放在 whitespace-pre-wrap 容器里随正文内联排版。
//
// 描述气泡走 portal 挂到 document.body + position:fixed:聊天区是 overflow 滚动
// 容器、顶部还有 sticky header,CSS 绝对定位的浮层会被裁剪 / 盖住。portal 到顶层
// 才能稳定盖在最前。顶部空间不足时(被 header 压住)自动翻到徽标下方。
function CommandToken({ token, name, kind, description }) {
  const anchorRef = useRef(null);
  const [tip, setTip] = useState(null);
  const displayName = String(name || token || '').replace(/^\/+/, '');

  const showTip = useCallback(() => {
    if (!description) return;
    const el = anchorRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const right = Math.max(8, window.innerWidth - r.right);
    if (r.top < 96) {
      // 顶部空间不足 → 翻到徽标下方,避免被 header 遮住
      setTip({ placement: 'below', right, top: Math.round(r.bottom + 6) });
    } else {
      setTip({ placement: 'above', right, bottom: Math.round(window.innerHeight - r.top + 6) });
    }
  }, [description]);
  const hideTip = useCallback(() => setTip(null), []);

  return (
    <span
      ref={anchorRef}
      className="ace-cmd-token"
      tabIndex={description ? 0 : undefined}
      onMouseEnter={showTip}
      onMouseLeave={hideTip}
      onFocus={showTip}
      onBlur={hideTip}
    >
      <CommandGlyph kind={kind} size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name">{displayName}</span>
      {tip
        ? createPortal(
            <span
              className="ace-cmd-token-tip"
              role="tooltip"
              data-placement={tip.placement}
              style={{
                right: tip.right,
                top: tip.placement === 'below' ? tip.top : undefined,
                bottom: tip.placement === 'above' ? tip.bottom : undefined,
              }}
            >
              <span className="ace-cmd-token-tip-name">{displayName}</span>
              <span className="ace-cmd-token-tip-desc">{description}</span>
            </span>,
            document.body,
          )
        : null}
    </span>
  );
}

// 用户消息正文:首段命中已知 skill / builtin 命令时把 "/name" 渲染成徽标,
// 其余原文照常;未命中(普通消息或未知命令)→ 纯文本回退。
function UserMessageBody({ content }) {
  const { commands } = useSlashCommands();
  const cmd = useMemo(() => resolveLeadingSlashCommand(content, commands), [content, commands]);
  if (!cmd) return content;
  return (
    <>
      <CommandToken token={cmd.token} name={cmd.name} kind={cmd.kind} description={cmd.description} />
      {cmd.rest}
    </>
  );
}

function UserBubble({ content, contentParts, ts, messageId, onFork }) {
  return (
    <div className="self-end min-w-0 max-w-[70%] flex flex-col items-end gap-0.5 group">
      <AttachmentStrip contentParts={contentParts} align="right" />
      {content ? (
        <div className="ace-chat-message-content px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
          <UserMessageBody content={content} />
        </div>
      ) : null}
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
  contentParts,
  ts,
  streaming,
  messageId,
  onFork,
  onOpenFilePreview,
  continuation,
  showFooter,
  showAceCodeAvatar,
}) {
  // 按块渲染(而非全文一次 dangerouslySetInnerHTML):流式追加时只有尾部
  // 块的 HTML 字符串变化,前缀块被 React 的字符串比较跳过,DOM 保持不动。
  // 整树替换会销毁浏览器滚动锚点并造成高度瞬时振荡 —— 那是 desktop
  // (WebView2)上流式出字时消息区上下跳动的主要来源。
  const blocks = useMemo(() => renderMarkdownBlocks(content || ''), [content]);
  const chrome = assistantChromeState({ showAceCodeAvatar, continuation });
  const handleMarkdownClick = useCallback(async (event) => {
    // 1) 本地文件链接 → 在中间详情页开预览。必须拦下默认导航,否则相对 href 会跳到
    //    http://<host>/<path> 命中 SPA 兜底(白屏/错误页),外链形态的还会开新标签页。
    const fileAnchor = event.target?.closest?.('a[data-file-path]');
    if (fileAnchor) {
      event.preventDefault();
      event.stopPropagation();
      const path = fileAnchor.getAttribute('data-file-path') || '';
      const lineAttr = fileAnchor.getAttribute('data-file-line');
      const line = lineAttr ? Number(lineAttr) : null;
      if (path) onOpenFilePreview?.(path, line);
      return;
    }
    // 2) 代码块复制按钮(原逻辑)。
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
  }, [onOpenFilePreview]);
  // ACECode 头像永久隐藏;不再保留空白占位,让左右外边距保持一致。
  return (
    <div className={`flex min-w-0 ${chrome.gapClass} max-w-[88%] group relative`}>
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
          className="ace-md ace-chat-message-content text-[13px] text-fg leading-[1.6] py-0.5"
          onClick={handleMarkdownClick}
        >
          {blocks.map((block) => (
            <div
              key={block.key}
              className="ace-md-block"
              dangerouslySetInnerHTML={{ __html: block.html }}
            />
          ))}
        </div>
        <AttachmentStrip contentParts={contentParts} align="left" />
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

function SystemRow({ role, content, metadata }) {
  const [expanded, setExpanded] = useState(false);
  const customLabel = metadata && typeof metadata.compact_label === 'string'
    ? metadata.compact_label
    : '';
  const isToolCompact = role === 'tool_call' || role === 'tool_result' || customLabel === '工具调用 / 返回';
  const { label, text, preview, lineCount, charCount } = useMemo(
    () => buildCompactMessagePreview({ role, content, label: customLabel }),
    [role, content, customLabel],
  );

  return (
    <div className={clsx(
      'self-stretch bg-surface-alt border border-dashed border-border rounded-md text-fg-2 overflow-hidden',
      isToolCompact ? 'ace-tool-call-text' : 'text-[12px]',
    )}>
      <button
        type="button"
        className="w-full px-3 py-1.5 flex items-center gap-2 text-left text-fg-mute hover:text-fg hover:bg-surface-hi transition"
        title={text || preview}
        onClick={() => setExpanded((v) => !v)}
      >
        <span className="font-medium shrink-0">{label}</span>
        <span className="text-[10px] flex-1 truncate font-mono" title={text || preview}>{preview}</span>
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

function ErrorRow({ content }) {
  return (
    <div className="ace-chat-message-content self-stretch max-w-[88%] rounded-md border border-danger/30 bg-danger-bg px-3 py-2 text-[12px] leading-5 text-danger whitespace-pre-wrap break-words">
      {content || '[Error]'}
    </div>
  );
}

export const Message = memo(function Message({
  role,
  content,
  contentParts,
  ts,
  streaming,
  messageId,
  metadata,
  onFork,
  onOpenFilePreview,
  continuation,
  showFooter = true,
  showAceCodeAvatar = false,
}) {
  if (role === 'user') {
    // expand-webui-skill-commands:daemon 把 /<skill> args 在送给 LLM 前展开为
    // 轻量提示;原文存到 metadata.display_text,UI 优先显示原文,不让用户看到
    // 内部展开。
    const hasDisplayText = metadata && typeof metadata.display_text === 'string'
      && (metadata.display_text.length > 0 || metadata.selection_context_expanded);
    const displayContent = hasDisplayText
      ? metadata.display_text
      : content;
    return <UserBubble content={displayContent} contentParts={contentParts} ts={ts}
                        messageId={messageId}
                        onFork={onFork} />;
  }
  if (role === 'assistant') {
    return <AssistantBubble content={content} contentParts={contentParts}
                             ts={ts} streaming={streaming}
                             messageId={messageId} onFork={onFork}
                             onOpenFilePreview={onOpenFilePreview}
                             continuation={continuation}
                             showFooter={showFooter}
                             showAceCodeAvatar={showAceCodeAvatar} />;
  }
  if (role === 'error') {
    return <ErrorRow content={content} />;
  }
  return <SystemRow role={role} content={content} metadata={metadata} />;
});
