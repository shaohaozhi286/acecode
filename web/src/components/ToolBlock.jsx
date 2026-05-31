// 工具调用块:三态显示
//   - 进度模式: tool_start 之后,显示 5-line tail + 状态行(行数/字节/已耗时)
//   - summary 模式: tool_end 后,绿/红 chip(icon · verb · object · metrics)
//   - 失败折叠: success=false 时 summary 行下显示前 3 行 stderr,可展开看完整 output
// 用户点 chip 可切换"展开/收起"。task_complete 用 Done: <summary> 渲染。
//
// hunks 字段(file_edit / file_write):展开区走 diff2html 渲染,而不是
// 纯 <pre>{output}</pre>。bash 工具的展开区头部加 `$ <command>` prompt 行。

import { memo, useEffect, useMemo, useState } from 'react';
import { clsx, formatBytes, formatElapsed } from '../lib/format.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { compactOneLinePreview } from '../lib/compactMessagePreview.js';
import { normalizeAttachmentList } from '../lib/messageAttachments.js';
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
          <span className="font-mono">{m.label}={m.value}</span>
        </span>
      ))}
    </span>
  );
}

export const ToolBlock = memo(function ToolBlock({ entry }) {
  const [expanded, setExpanded] = useState(false);

  const {
    isTaskComplete = false,
    isDone = false,
    success = null,
    title = '',
    tool = '',
    displayOverride = '',
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
  } = entry || {};
  const attachmentItems = useMemo(() => normalizeAttachmentList(attachments), [attachments]);

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
  const outputPreview = useMemo(
    () => compactOneLinePreview(output || currentPartial || tailLines.join('\n') || title || displayOverride || tool),
    [currentPartial, displayOverride, output, tailLines, title, tool],
  );

  // diff2html 渲染:先把 hunks 转 unified diff,再交给 diff2html。空 hunks 时
  // 不构造,避免每次 render 浪费。
  const diffHtml = useMemo(() => {
    if (!Array.isArray(hunks) || hunks.length === 0) return '';
    const file = (summary && summary.object) || displayOverride || 'change';
    const unified = hunksToUnifiedDiff(hunks, file);
    if (!unified) return '';
    try {
      return Diff2Html.html(unified, {
        drawFileList: false,
        outputFormat: 'line-by-line',
        matching: 'lines',
      });
    } catch {
      return '';
    }
  }, [hunks, summary, displayOverride]);

  if (isTaskComplete) {
    const text = (summary && summary.object) || '完成';
    return (
      <div className="flex items-center gap-2 px-2.5 py-1 my-0.5 text-[12px] font-medium text-ok">
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
        className={clsx(
          'rounded-md font-mono text-[12px] my-0.5 transition',
          ok ? 'bg-ok-bg border border-ok-border text-ok' : 'bg-danger-bg border border-danger/30 text-danger',
        )}
      >
        <button
          type="button"
          className="w-full min-w-0 overflow-hidden text-left flex items-center gap-1.5 px-2.5 py-[5px] cursor-pointer whitespace-nowrap"
          title={expanded ? '收起' : '展开'}
          aria-label={expanded ? '收起' : '展开'}
          onClick={() => setExpanded((v) => !v)}
        >
          <ToolSummaryIcon icon={summary.icon} ok={ok} className="shrink-0" />
          <span className="font-medium shrink-0">{summary.verb || ''}</span>
          {summary.object && <span className="text-fg-2 flex-1 min-w-0 truncate">· {summary.object}</span>}
          <MetricList metrics={summary.metrics} />
          {!ok && output && <span className="text-fg-mute min-w-0 truncate">· {outputPreview}</span>}
          <span className="ml-auto opacity-60 flex items-center shrink-0">
            <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
          </span>
        </button>
        {expanded && (
          <div className="px-3 pb-2 pt-1">
            {tool === 'bash' && displayOverride && (
              <div className="text-[11px] text-fg-mute font-mono opacity-70 mb-1">
                $ {displayOverride}
              </div>
            )}
            {diffHtml ? (
              <div
                className="ace-diff"
                dangerouslySetInnerHTML={{ __html: diffHtml }}
              />
            ) : output ? (
              <CopyableCodeFrame text={output}>
                <pre className="m-0 text-[11px] text-fg-2 whitespace-pre-wrap break-all max-h-[280px] overflow-y-auto" data-code-copy-source="true">{output}</pre>
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
        className={clsx(
          'rounded-md font-mono text-[12px] my-0.5 transition',
          ok ? 'bg-ok-bg border border-ok-border text-ok' : 'bg-danger-bg border border-danger/30 text-danger',
        )}
      >
        <button
          type="button"
          className="w-full min-w-0 overflow-hidden text-left flex items-center gap-1.5 px-2.5 py-[5px] cursor-pointer whitespace-nowrap"
          title={outputPreview || (expanded ? '收起' : '展开')}
          aria-label={expanded ? '收起' : '展开'}
          onClick={() => setExpanded((v) => !v)}
        >
          <VsIcon name={ok ? 'ok' : 'warning'} size={13} mono={false} className="shrink-0" />
          <span className="font-medium flex-1 min-w-0 truncate">{title || tool || '工具完成'}</span>
          {output && <span className="text-fg-mute min-w-0 truncate">· {outputPreview}</span>}
          <span className="ml-auto opacity-60 flex items-center shrink-0">
            <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
          </span>
        </button>
        {attachmentItems.length > 0 && (
          <div className="px-3 pb-2 pt-1">
            <AttachmentStrip attachments={attachmentItems} align="left" compact />
          </div>
        )}
        {expanded && output && (
          <div className="px-3 pb-2 pt-1">
            <CopyableCodeFrame text={output}>
              <pre className="m-0 text-[11px] text-fg-2 whitespace-pre-wrap break-all max-h-[280px] overflow-y-auto" data-code-copy-source="true">{output}</pre>
            </CopyableCodeFrame>
          </div>
        )}
      </div>
    );
  }

  // 进度模式
  const hidden = Math.max(0, totalLines - tailLines.length);
  return (
    <div className="rounded-md border border-border bg-surface my-0.5 font-mono text-[11px] overflow-hidden">
      <button
        type="button"
        className="w-full min-w-0 overflow-hidden px-2.5 py-1.5 flex items-center gap-2 text-left text-fg hover:bg-surface-hi transition whitespace-nowrap"
        title={outputPreview || (expanded ? '收起' : '展开')}
        aria-label={expanded ? '收起' : '展开'}
        onClick={() => setExpanded((v) => !v)}
      >
        <span className="ace-spinner w-3 h-3 shrink-0" />
        <span className="font-semibold flex-1 min-w-0 truncate">{title}</span>
        <span className="text-fg-mute text-[10px] shrink-0">{totalLines} 行</span>
        <span className="text-fg-mute text-[10px] shrink-0">{formatBytes(totalBytes)}</span>
        <span className="text-fg-mute text-[10px] shrink-0">{formatElapsed(liveElapsed)}</span>
        <span className="ml-auto opacity-60 flex items-center shrink-0">
          <VsIcon name={expanded ? 'expandUp' : 'expandDown'} size={12} />
        </span>
      </button>
      {expanded && (
        <div className="px-2.5 pb-1.5">
          {hidden > 0 && (
            <div className="text-fg-mute text-[10px] mt-1">... +{hidden} 行已折叠</div>
          )}
          {tailLines.length > 0 && (
            <CopyableCodeFrame text={tailLines.join('\n')} className="mt-1">
              <pre className="m-0 text-fg-2 whitespace-pre-wrap break-all max-h-[100px] overflow-hidden" data-code-copy-source="true">{tailLines.join('\n')}</pre>
            </CopyableCodeFrame>
          )}
          {currentPartial && (
            <div className="text-fg-mute opacity-70 truncate">{currentPartial}</div>
          )}
        </div>
      )}
    </div>
  );
});
