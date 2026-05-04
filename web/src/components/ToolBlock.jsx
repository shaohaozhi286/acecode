// 工具调用块:三态显示
//   - 进度模式: tool_start 之后,显示 5-line tail + 状态行(行数/字节/已耗时)
//   - summary 模式: tool_end 后,绿/红 chip(icon · verb · object · metrics)
//   - 失败折叠: success=false 时 summary 行下显示前 3 行 stderr,可展开看完整 output
// 用户点 chip 可切换"展开/收起"。task_complete 用 Done: <summary> 渲染。
//
// hunks 字段(file_edit / file_write):展开区走 diff2html 渲染,而不是
// 纯 <pre>{output}</pre>。bash 工具的展开区头部加 `$ <command>` prompt 行。

import { memo, useMemo, useState } from 'react';
import { clsx, formatBytes, formatElapsed } from '../lib/format.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { ToolSummaryIcon, VsIcon } from './Icon.jsx';
import * as Diff2Html from 'diff2html';

function MetricList({ metrics }) {
  if (!metrics || !metrics.length) return null;
  return (
    <span className="text-fg-mute">
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
    summary = null,
    output = '',
    hunks = [],
  } = entry || {};

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
          className="w-full text-left flex items-center gap-1.5 px-2.5 py-[5px] cursor-pointer"
          onClick={() => setExpanded((v) => !v)}
        >
          <ToolSummaryIcon icon={summary.icon} ok={ok} />
          <span className="font-medium">{summary.verb || ''}</span>
          {summary.object && <span className="text-fg-2 truncate">· {summary.object}</span>}
          <MetricList metrics={summary.metrics} />
          <span className="ml-auto text-[10px] opacity-60 flex items-center gap-1">
            {expanded ? '收起' : '展开'}
            <VsIcon name={expanded ? 'glyphUp' : 'glyphDown'} size={9} />
          </span>
        </button>
        {!ok && output && !expanded && (
          <div className="px-3 pb-1.5 text-fg-mute text-[11px] font-mono whitespace-pre-wrap break-all">
            {output.split('\n').slice(0, 3).join('\n')}
          </div>
        )}
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
              <pre className="m-0 text-[11px] text-fg-2 whitespace-pre-wrap break-all max-h-[280px] overflow-y-auto">
                {output}
              </pre>
            ) : null}
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
          'rounded-md font-mono text-[12px] my-0.5 px-2.5 py-1.5',
          ok ? 'bg-ok-bg border border-ok-border text-ok' : 'bg-danger-bg border border-danger/30 text-danger',
        )}
      >
        <div className="font-medium truncate">{title}</div>
        {output && (
          <pre className="m-0 mt-1 text-[11px] text-fg-2 whitespace-pre-wrap break-all max-h-[200px] overflow-y-auto">
            {output}
          </pre>
        )}
      </div>
    );
  }

  // 进度模式
  const hidden = Math.max(0, totalLines - tailLines.length);
  return (
    <div className="rounded-md border border-border bg-surface my-0.5 px-2.5 py-1.5 font-mono text-[11px]">
      <div className="flex items-center gap-2 text-fg">
        <span className="ace-spinner w-3 h-3" />
        <span className="font-semibold truncate">{title}</span>
      </div>
      <div className="text-fg-mute text-[10px] mt-0.5 flex gap-3">
        <span>{totalLines} 行</span>
        <span>{formatBytes(totalBytes)}</span>
        <span>{formatElapsed(elapsed)}</span>
      </div>
      {hidden > 0 && (
        <div className="text-fg-mute text-[10px] mt-1">... +{hidden} 行已折叠</div>
      )}
      {tailLines.length > 0 && (
        <pre className="m-0 mt-1 text-fg-2 whitespace-pre-wrap break-all max-h-[100px] overflow-hidden">
          {tailLines.join('\n')}
        </pre>
      )}
      {currentPartial && (
        <div className="text-fg-mute opacity-70 truncate">{currentPartial}</div>
      )}
    </div>
  );
});
