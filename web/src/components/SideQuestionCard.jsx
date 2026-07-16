import { useCallback, useMemo } from 'react';

import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { renderMarkdown } from '../lib/markdown.js';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

export function SideQuestionCard({ state, onDismiss }) {
  const status = state?.status || '';
  const question = String(state?.question || '');
  const answer = String(state?.answer || '');
  const error = String(state?.error || '');
  const html = useMemo(() => ({ __html: renderMarkdown(answer) }), [answer]);
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

  if (!state || !question) return null;
  return (
    <section
      aria-label="引导旁路提问"
      aria-live="polite"
      data-side-question-state={status}
      className="mx-2.5 mt-2 rounded-xl border border-accent/40 bg-surface-hi shadow-sm overflow-hidden"
    >
      <div className="min-h-9 px-3 py-1.5 flex items-center gap-2 border-b border-border/70">
        <span className="w-5 h-5 rounded-full bg-accent-bg text-accent flex items-center justify-center shrink-0">
          <VsIcon name="glyphUp" size={11} />
        </span>
        <span className="text-[12px] font-medium text-fg">引导</span>
        <span className="text-[10px] text-accent">/btw</span>
        <span className="flex-1 min-w-0 truncate text-[11px] text-fg-mute" title={question}>{question}</span>
        {status === 'loading' ? (
          <span className="ace-spinner shrink-0" aria-label="引导回答生成中" />
        ) : (
          <button
            type="button"
            onClick={onDismiss}
            aria-label="关闭引导回答"
            className="w-6 h-6 rounded flex items-center justify-center text-fg-mute hover:text-fg hover:bg-surface transition"
          >
            <VsIcon name="close" size={12} />
          </button>
        )}
      </div>
      <div className="px-3 py-2 max-h-[30vh] overflow-y-auto">
        {status === 'loading' && (
          <div className="text-[12px] text-fg-mute">正在基于当前会话上下文回答，不会打断主任务…</div>
        )}
        {status === 'success' && (
          <div
            className="ace-md text-[13px] text-fg leading-[1.6]"
            onClick={handleMarkdownClick}
            dangerouslySetInnerHTML={html}
          />
        )}
        {status === 'error' && (
          <div className="text-[12px] text-danger">引导失败：{error || '未知错误'}</div>
        )}
      </div>
    </section>
  );
}

export default SideQuestionCard;
