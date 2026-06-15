// 工具调用块:三态显示
//   - 进度模式: tool_start 之后,显示 5-line tail + 状态行(行数/字节/已耗时)
//   - summary 模式: tool_end 后,绿/红 chip(icon · verb · object · metrics)
//   - 失败折叠: success=false 时 summary 行下显示前 3 行 stderr,可展开看完整 output
// 用户点 chip 可切换"展开/收起"。task_complete 用 Done: <summary> 渲染。
//
// hunks 字段(file_edit / file_write):展开区走 diff2html 渲染,而不是
// 纯 <pre>{output}</pre>。bash 工具的展开区头部加 `$ <command>` prompt 行。

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { clsx, formatBytes, formatElapsed } from '../lib/format.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { compactOneLinePreview } from '../lib/compactMessagePreview.js';
import { normalizeAttachmentList } from '../lib/messageAttachments.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { AttachmentStrip } from './AttachmentStrip.jsx';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { ToolSummaryIcon, VsIcon } from './Icon.jsx';
import * as Diff2Html from 'diff2html';

function MetricList({ metrics }) {
  if (!metrics || !metrics.length) return null;
  return (
    <span className="text-fg-mute shrink-0 whitespace-nowrap tabular-nums">
      {metrics.map((m, i) => (
        <span key={i}>
          {' · '}
          <span>{m.label}={m.value}</span>
        </span>
      ))}
    </span>
  );
}

function ClampedQuestionText({ children, className = '' }) {
  const ref = useRef(null);
  const [clamped, setClamped] = useState(false);
  const text = String(children || '');

  useEffect(() => {
    const el = ref.current;
    if (!el) return undefined;
    const measure = () => {
      setClamped((el.scrollHeight - el.clientHeight) > 1);
    };
    measure();
    if (typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [text]);

  return (
    <span
      ref={ref}
      className={clsx('ace-qa-text-clamp', clamped && 'is-clamped', className)}
      title={text || undefined}
    >
      {text}
    </span>
  );
}

function askUserQuestionText(result) {
  const items = Array.isArray(result?.items) ? result.items : [];
  return items
    .filter((item) => item && (item.question || item.answer))
    .map((item) => `Q ${item.question || ''}\nA ${item.answer || ''}`)
    .join('\n\n');
}

function joinTooltipParts(...parts) {
  const text = parts
    .map((part) => String(part || '').trim())
    .filter(Boolean)
    .join('\n\n');
  return text || undefined;
}

function stringArg(args, key) {
  if (!args || typeof args !== 'object' || Array.isArray(args)) return '';
  const value = args[key];
  return typeof value === 'string' ? value : '';
}

function AskUserQuestionResultCard({ result, toolContextAttrs }) {
  const [collapsed, setCollapsed] = useState(false);
  const items = Array.isArray(result?.items)
    ? result.items.filter((item) => item && (item.question || item.answer))
    : [];
  if (items.length === 0) return null;
  const fullText = askUserQuestionText({ items });

  return (
    <div className={clsx('ace-qa-card my-0.5', collapsed && 'is-collapsed')} {...toolContextAttrs}>
      <button
        type="button"
        className="ace-qa-card-header"
        onClick={() => setCollapsed((value) => !value)}
        aria-expanded={!collapsed}
        title={collapsed ? fullText : '收起'}
      >
        <span className="ace-qa-card-icon">
          <VsIcon name="ok" size={14} mono={false} />
        </span>
        <span className="ace-qa-card-title">已确认 {items.length} 项</span>
        <span className="ace-qa-card-spacer" />
        <span className="ace-qa-card-state">{collapsed ? '展开' : '收起'}</span>
        <VsIcon
          name={collapsed ? 'expandRight' : 'expandDown'}
          size={15}
          className="ace-qa-card-chevron"
        />
      </button>
      {!collapsed && (
        <div className="ace-qa-card-body">
          {items.map((item, index) => (
            <div key={`${item.question || ''}-${index}`} className="ace-qa-item">
              <div className="ace-qa-row">
                <span className="ace-qa-mark ace-qa-mark-q">Q</span>
                <ClampedQuestionText className="ace-qa-question">
                  {item.question}
                </ClampedQuestionText>
              </div>
              <div className="ace-qa-row ace-qa-row-answer">
                <span className="ace-qa-mark ace-qa-mark-a">A</span>
                <ClampedQuestionText className="ace-qa-answer">
                  {item.answer}
                </ClampedQuestionText>
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

export const ToolBlock = memo(function ToolBlock({ entry, onReviewToggle }) {
  const [expanded, setExpanded] = useState(false);
  const contextIdRef = useRef('');
  if (!contextIdRef.current) {
    contextIdRef.current = `tool-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  const {
    isTaskComplete = false,
    isDone = false,
    success = null,
    title = '',
    tool = '',
    displayOverride = '',
    args = null,
    tailLines = [],
    currentPartial = '',
    totalLines = 0,
    totalBytes = 0,
    elapsed = 0,
    startedAtMs = 0,
    summary = null,
    output = '',
    hunks = [],
    attachments = [],
    askUserQuestionResult = null,
  } = entry || {};
  const attachmentItems = useMemo(() => normalizeAttachmentList(attachments), [attachments]);
  const askUserQuestionOutput = useMemo(() => {
    return askUserQuestionText(askUserQuestionResult);
  }, [askUserQuestionResult]);

  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    if (isDone || !startedAtMs) return undefined;
    setNowMs(Date.now());
    const id = window.setInterval(() => setNowMs(Date.now()), 1000);
    return () => window.clearInterval(id);
  }, [isDone, startedAtMs]);

  const liveElapsed = !isDone && startedAtMs
    ? Math.max(Number(elapsed) || 0, Math.max(0, (nowMs - startedAtMs) / 1000))
    : (Number(elapsed) || 0);
  const bashCommand = tool === 'bash' ? stringArg(args, 'command') : '';
  const bashPrompt = bashCommand || (tool === 'bash' ? displayOverride : '');
  const expandedInvocationText = bashPrompt ? `$ ${bashPrompt}` : '';
  const outputPreview = useMemo(
    () => compactOneLinePreview(output || askUserQuestionOutput || currentPartial || tailLines.join('\n') || bashCommand || title || displayOverride || tool),
    [askUserQuestionOutput, bashCommand, currentPartial, displayOverride, output, tailLines, title, tool],
  );

  // diff2html 渲染:先把 hunks 转 unified diff,再交给 diff2html。空 hunks 时
  // 不构造,避免每次 render 浪费。
  const diffText = useMemo(() => {
    if (!Array.isArray(hunks) || hunks.length === 0) return '';
    const file = (summary && summary.object) || displayOverride || 'change';
    return hunksToUnifiedDiff(hunks, file) || '';
  }, [hunks, summary, displayOverride]);

  const diffHtml = useMemo(() => {
    if (!diffText) return '';
    try {
      return Diff2Html.html(diffText, {
        drawFileList: false,
        outputFormat: 'line-by-line',
        matching: 'lines',
      });
    } catch {
      return '';
    }
  }, [diffText]);

  const fullOutput = output || askUserQuestionOutput || diffText || tailLines.join('\n') || currentPartial || '';
  const fullToolOutput = joinTooltipParts(expandedInvocationText, fullOutput) || fullOutput;
  const visibleOutput = expanded ? fullToolOutput : (outputPreview || currentPartial || tailLines.join('\n') || '');
  const toolName = bashCommand
    ? [summary?.verb || title || tool || 'bash', bashCommand].filter(Boolean).join(' · ')
    : (title || displayOverride || tool || summary?.object || 'tool');
  const summaryLabel = summary
    ? [summary.verb, bashCommand || summary.object].filter(Boolean).join(' · ')
    : '';
  const summaryObjectTitle = bashCommand || summary?.object || '';
  const buttonTooltip = joinTooltipParts(summaryLabel || toolName, fullToolOutput);
  const toolContextAttrs = {
    'data-desktop-tool-id': contextIdRef.current,
    'data-desktop-tool-name': toolName,
    'data-desktop-tool-visible-output': visibleOutput || undefined,
    'data-desktop-tool-full-output': fullToolOutput || undefined,
    'data-desktop-tool-expanded': expanded ? 'true' : 'false',
    'data-desktop-tool-toggle': isTaskComplete ? 'false' : 'true',
  };

  const toggleExpanded = useCallback(() => {
    onReviewToggle?.();
    setExpanded((v) => !v);
  }, [onReviewToggle]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'tool' || target.id !== contextIdRef.current) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL) {
        detail.handled = true;
        onReviewToggle?.();
        setExpanded(true);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_TOOL) {
        detail.handled = true;
        onReviewToggle?.();
        setExpanded(false);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onReviewToggle]);

  if (isDone && success !== false && askUserQuestionResult?.items?.length > 0) {
    return (
      <AskUserQuestionResultCard
        result={askUserQuestionResult}
        toolContextAttrs={toolContextAttrs}
      />
    );
  }

  if (isTaskComplete) {
    const text = (summary && summary.object) || '完成';
    return (
      <div
        className="ace-tool-call-text flex items-center gap-2 px-2.5 py-1 my-0.5 font-medium text-ok"
        title={joinTooltipParts('Done', text)}
        {...toolContextAttrs}
      >
        <VsIcon name="ok" size={13} mono={false} />
        <span>Done · {text}</span>
      </div>
    );
  }

  // summary 模式
  if (isDone && summary) {
    const ok = !!success;
    return (
      <div
        {...toolContextAttrs}
        className={clsx(
          'ace-tool-call-text rounded-md my-0.5 transition',
          ok ? 'bg-ok-bg border border-ok-border text-ok' : 'bg-danger-bg border border-danger/30 text-danger',
        )}
      >
        <button
          type="button"
          className="w-full min-w-0 overflow-hidden text-left flex items-center gap-1.5 px-2.5 py-[5px] cursor-pointer whitespace-nowrap"
          title={buttonTooltip || (expanded ? '收起' : '展开')}
          aria-label={expanded ? '收起' : '展开'}
          onClick={toggleExpanded}
        >
          <ToolSummaryIcon icon={summary.icon} ok={ok} className="shrink-0" />
          <span className="font-medium shrink-0">{summary.verb || ''}</span>
          {summary.object && <span className="text-fg-2 flex-1 min-w-0 truncate" title={summaryObjectTitle}>· {summary.object}</span>}
          <MetricList metrics={summary.metrics} />
          {!ok && output && <span className="text-fg-mute min-w-0 truncate" title={output}>· {outputPreview}</span>}
          <span className="ml-auto opacity-60 flex items-center shrink-0">
            <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
          </span>
        </button>
        {expanded && (
          <div className="px-3 pb-2 pt-1">
            {tool === 'bash' && bashPrompt && (
              <div className="text-fg-mute opacity-70 mb-1 break-all" title={bashPrompt}>
                $ {bashPrompt}
              </div>
            )}
            {diffHtml ? (
              <div
                className="ace-diff"
                dangerouslySetInnerHTML={{ __html: diffHtml }}
              />
            ) : output ? (
              <CopyableCodeFrame text={output}>
                <pre className="m-0 text-fg-2 whitespace-pre-wrap break-all max-h-[280px] overflow-y-auto" data-code-copy-source="true">{output}</pre>
              </CopyableCodeFrame>
            ) : null}
          </div>
        )}
        {attachmentItems.length > 0 && (
          <div className="px-3 pb-2 pt-1">
            <AttachmentStrip attachments={attachmentItems} align="left" compact />
          </div>
        )}
      </div>
    );
  }

  // done 但无 summary → fallback
  if (isDone) {
    const ok = !!success;
    return (
      <div
        {...toolContextAttrs}
        className={clsx(
          'ace-tool-call-text rounded-md my-0.5 transition',
          ok ? 'bg-ok-bg border border-ok-border text-ok' : 'bg-danger-bg border border-danger/30 text-danger',
        )}
      >
        <button
          type="button"
          className="w-full min-w-0 overflow-hidden text-left flex items-center gap-1.5 px-2.5 py-[5px] cursor-pointer whitespace-nowrap"
          title={buttonTooltip || outputPreview || (expanded ? '收起' : '展开')}
          aria-label={expanded ? '收起' : '展开'}
          onClick={toggleExpanded}
        >
          <VsIcon name={ok ? 'ok' : 'warning'} size={13} mono={false} className="shrink-0" />
          <span className="font-medium flex-1 min-w-0 truncate" title={toolName}>{title || tool || '工具完成'}</span>
          {output && <span className="text-fg-mute min-w-0 truncate" title={output}>· {outputPreview}</span>}
          <span className="ml-auto opacity-60 flex items-center shrink-0">
            <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
          </span>
        </button>
        {attachmentItems.length > 0 && (
          <div className="px-3 pb-2 pt-1">
            <AttachmentStrip attachments={attachmentItems} align="left" compact />
          </div>
        )}
        {expanded && (output || bashPrompt) && (
          <div className="px-3 pb-2 pt-1">
            {tool === 'bash' && bashPrompt && (
              <div className="text-fg-mute opacity-70 mb-1 break-all" title={bashPrompt}>
                $ {bashPrompt}
              </div>
            )}
            {output && (
              <CopyableCodeFrame text={output}>
                <pre className="m-0 text-fg-2 whitespace-pre-wrap break-all max-h-[280px] overflow-y-auto" data-code-copy-source="true">{output}</pre>
              </CopyableCodeFrame>
            )}
          </div>
        )}
      </div>
    );
  }

  // 进度模式
  const hidden = Math.max(0, totalLines - tailLines.length);
  return (
    <div
      className="ace-tool-call-text rounded-md border border-border bg-surface my-0.5 overflow-hidden"
      {...toolContextAttrs}
    >
      <button
        type="button"
        className="w-full min-w-0 overflow-hidden px-2.5 py-1.5 flex items-center gap-2 text-left text-fg hover:bg-surface-hi transition whitespace-nowrap"
        title={buttonTooltip || outputPreview || (expanded ? '收起' : '展开')}
        aria-label={expanded ? '收起' : '展开'}
        onClick={toggleExpanded}
      >
        <span className="ace-spinner w-3 h-3 shrink-0" />
        <span className="font-semibold flex-1 min-w-0 truncate" title={title}>{title}</span>
        <span className="text-fg-mute shrink-0">{totalLines} 行</span>
        <span className="text-fg-mute shrink-0">{formatBytes(totalBytes)}</span>
        <span className="text-fg-mute shrink-0">{formatElapsed(liveElapsed)}</span>
        <span className="ml-auto opacity-60 flex items-center shrink-0">
          <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
        </span>
      </button>
      {expanded && (
        <div className="px-2.5 pb-1.5">
          {tool === 'bash' && bashPrompt && (
            <div className="text-fg-mute opacity-70 mt-1 break-all" title={bashPrompt}>
              $ {bashPrompt}
            </div>
          )}
          {hidden > 0 && (
            <div className="text-fg-mute mt-1">... +{hidden} 行已折叠</div>
          )}
          {tailLines.length > 0 && (
            <CopyableCodeFrame text={tailLines.join('\n')} className="mt-1">
              <pre className="m-0 text-fg-2 whitespace-pre-wrap break-all max-h-[100px] overflow-hidden" title={tailLines.join('\n')} data-code-copy-source="true">{tailLines.join('\n')}</pre>
            </CopyableCodeFrame>
          )}
          {currentPartial && (
            <div className="text-fg-mute opacity-70 truncate" title={currentPartial}>{currentPartial}</div>
          )}
        </div>
      )}
    </div>
  );
});
