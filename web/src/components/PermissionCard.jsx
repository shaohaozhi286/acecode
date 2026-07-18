import { useMemo } from 'react';
import { planPermissionPresentation } from '../lib/permissionRequestPresentation.js';
import { buildPermissionPreview } from '../lib/permissionPreview.js';
import { PERMISSION_REQUEST_STATUS } from '../lib/permissionRequestQueue.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

function resolvedLabel(choice) {
  if (choice === 'allow') return '已允许一次';
  if (choice === 'allow_session') return '本次会话已允许';
  return '已拒绝';
}

function resolvedReason(reason) {
  if (reason === 'timeout') return '请求已超时';
  if (reason === 'abort') return '任务已中止';
  if (reason === 'permission_mode_change') return '权限模式已变更';
  return '';
}

export function PermissionCard({ request, onDecision, originLabel = '' }) {
  const preview = useMemo(() => buildPermissionPreview(request), [request]);
  const {
    isPlanEnter,
    isPlanApproval,
    planText,
    planFilePath,
    hideAllowSession,
    title,
    body,
    primaryLabel,
  } = planPermissionPresentation(request);
  const status = request?.status || PERMISSION_REQUEST_STATUS.PENDING;
  const pending = status === PERMISSION_REQUEST_STATUS.PENDING;
  const submitting = status === PERMISSION_REQUEST_STATUS.SUBMITTING;
  const resolved = status === PERMISSION_REQUEST_STATUS.RESOLVED;

  const decide = (choice) => {
    if (!pending || !request?.request_id) return;
    onDecision?.(request, choice);
  };

  if (resolved) {
    return (
      <article
        data-permission-card="resolved"
        className="w-full max-w-[720px] min-w-0 rounded-lg border border-border bg-surface-alt px-3.5 py-2.5 text-[12px] text-fg-2"
      >
        <div className="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
          <VsIcon name="check" size={14} className="shrink-0 text-ok" />
          <span className="min-w-0 truncate font-medium text-fg">{title}</span>
          <span className="shrink-0 rounded-full border border-ok-border bg-ok-bg px-2 py-[1px] text-[11px] font-medium text-ok">
            {resolvedLabel(request?.choice)}
          </span>
          {resolvedReason(request?.reason) && (
            <span className="text-[11px] text-fg-mute">{resolvedReason(request.reason)}</span>
          )}
        </div>
        {originLabel && (
          <div className="mt-1 truncate text-[11px] text-fg-mute" title={originLabel}>
            {originLabel}
          </div>
        )}
      </article>
    );
  }

  return (
    <article
      data-permission-card={status}
      aria-busy={submitting ? 'true' : undefined}
      className="w-full max-w-[720px] min-w-0 overflow-hidden rounded-lg border border-warn/50 bg-surface shadow-sm"
    >
      <div className="flex items-center gap-2 border-b border-border bg-warn-bg px-3.5 py-2.5 text-fg">
        <VsIcon name="warning" size={16} mono={false} className="shrink-0 text-warn" />
        <h3 className="min-w-0 truncate text-[13px] font-semibold">{title}</h3>
        {submitting && (
          <span className="ml-auto shrink-0 text-[11px] text-fg-mute">正在确认…</span>
        )}
      </div>

      <div className="flex min-w-0 flex-col gap-3 px-3.5 py-3">
        {originLabel && (
          <div className="truncate text-[11px] text-fg-mute" title={originLabel}>
            {originLabel}
          </div>
        )}
        <p className="m-0 text-[12px] leading-relaxed text-fg-2">{body}</p>
        {(isPlanApproval || isPlanEnter) && planFilePath && (
          <div className="break-all text-[11px] text-fg-mute">{planFilePath}</div>
        )}

        {isPlanApproval ? (
          <details open className="min-w-0 overflow-hidden rounded-md border border-border bg-surface-alt">
            <summary className="cursor-pointer select-none border-b border-border px-3 py-2 text-[12px] font-semibold text-fg">
              Plan
            </summary>
            <pre
              className="m-0 max-w-full overflow-auto overscroll-contain whitespace-pre-wrap break-words px-3 py-2.5 text-[11px] leading-relaxed text-fg-2"
              style={{ maxHeight: 'min(42vh, 320px)' }}
            >
              {planText.trim() ? planText : '计划文件为空'}
            </pre>
          </details>
        ) : !isPlanEnter && (
          <div className="min-w-0 overflow-hidden rounded-md border border-border bg-surface-alt font-mono">
            <div className="flex items-center justify-between gap-2 border-b border-border px-3 py-2 text-[12px] font-semibold text-fg">
              <span className="min-w-0 truncate">{preview.tool.toolLabel}</span>
              {preview.truncated > 0 && (
                <span className="shrink-0 text-[10px] font-normal text-warn">仅显示预览</span>
              )}
            </div>
            {preview.tool.kind === 'file' ? (
              <div className="min-w-0 px-3 py-2.5">
                <div className="break-all text-[12px] text-fg-2">{preview.tool.filePath}</div>
                <div className="mt-0.5 text-[11px] text-fg-mute">{preview.tool.detail}</div>
              </div>
            ) : (
              <pre
                className="m-0 max-w-full overflow-auto overscroll-contain whitespace-pre-wrap break-words px-3 py-2.5 text-[11px] leading-relaxed text-fg-2"
                style={{ maxHeight: 'min(38vh, 280px)' }}
              >
                {preview.text}
              </pre>
            )}
          </div>
        )}
      </div>

      <div className="flex flex-wrap justify-end gap-2 px-3.5 pb-3">
        <button
          type="button"
          disabled={!pending}
          onClick={() => decide('deny')}
          className="h-8 rounded-md bg-surface-hi px-3 text-[12px] font-medium text-fg-2 transition hover:bg-border disabled:cursor-wait disabled:opacity-50"
        >
          拒绝
        </button>
        {!hideAllowSession && (
          <button
            type="button"
            disabled={!pending}
            onClick={() => decide('allow_session')}
            className="h-8 rounded-md border border-accent bg-transparent px-3 text-[12px] font-medium text-accent transition hover:bg-accent-bg disabled:cursor-wait disabled:opacity-50"
          >
            本次会话允许
          </button>
        )}
        <button
          type="button"
          disabled={!pending}
          onClick={() => decide('allow')}
          className={clsx(
            'h-8 rounded-md bg-accent px-3 text-[12px] font-medium text-white transition',
            'hover:opacity-90 disabled:cursor-wait disabled:opacity-50',
          )}
        >
          {primaryLabel}
        </button>
      </div>
    </article>
  );
}
