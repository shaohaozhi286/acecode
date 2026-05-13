// permission_request 弹框。Allow / AllowAlways / Deny 三按钮。
// 多 request 排队由 App 层管(本组件每次只显示一个)。

import { useMemo, useRef } from 'react';
import { connection } from '../lib/connection.js';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';

const MAX_PREVIEW_CHARS = 3200;
const MAX_STRING_CHARS = 900;
const MAX_OBJECT_KEYS = 24;
const MAX_ARRAY_ITEMS = 16;
const MAX_DEPTH = 4;

function truncateText(text, limit) {
  const value = String(text ?? '');
  if (value.length <= limit) return { text: value, truncated: 0 };
  return {
    text: value.slice(0, limit) + `\n... 已截断 ${value.length - limit} 个字符`,
    truncated: value.length - limit,
  };
}

function compactValue(value, depth = 0) {
  if (typeof value === 'string') return truncateText(value, MAX_STRING_CHARS).text;
  if (value === null || typeof value !== 'object') return value;

  if (depth >= MAX_DEPTH) {
    if (Array.isArray(value)) return `[Array(${value.length})]`;
    return '{...}';
  }

  if (Array.isArray(value)) {
    const items = value.slice(0, MAX_ARRAY_ITEMS).map((item) => compactValue(item, depth + 1));
    if (value.length > MAX_ARRAY_ITEMS) items.push(`... 还有 ${value.length - MAX_ARRAY_ITEMS} 项`);
    return items;
  }

  const out = {};
  const entries = Object.entries(value).slice(0, MAX_OBJECT_KEYS);
  for (const [key, item] of entries) out[key] = compactValue(item, depth + 1);
  const rest = Object.keys(value).length - entries.length;
  if (rest > 0) out['...'] = `还有 ${rest} 个字段`;
  return out;
}

function formatArgsPreview(args) {
  if (typeof args === 'string') return truncateText(args, MAX_PREVIEW_CHARS);
  let text = '{}';
  try {
    text = JSON.stringify(compactValue(args || {}), null, 2);
  } catch {
    text = String(args ?? '');
  }
  return truncateText(text, MAX_PREVIEW_CHARS);
}

export function PermissionModal({ request, onResolve }) {
  const preview = useMemo(() => formatArgsPreview(request?.args), [request?.args]);
  const resolvedRef = useRef(false);

  const respond = (choice, close) => {
    resolvedRef.current = true;
    connection.sendDecision(request.request_id, choice, request.session_id);
    close();
    setTimeout(() => onResolve?.(), 220);
  };

  // Escape key or backdrop click: send deny immediately so the backend is
  // unblocked instead of waiting up to 5 minutes for the timeout.
  const handleModalClose = () => {
    if (!resolvedRef.current) {
      connection.sendDecision(request.request_id, 'deny', request.session_id);
    }
    onResolve?.();
  };

  return (
    <Modal width={520} onClose={handleModalClose}>
      {({ close }) => (
        <>
          <div className="px-4.5 py-3 bg-warn/10 border-b border-border flex items-center gap-2">
            <VsIcon name="warning" size={16} mono={false} />
            <h3 className="text-[14px] font-semibold">权限请求</h3>
          </div>
          <div className="px-4.5 py-4 flex flex-col gap-3">
            <p className="text-[13px] text-fg-2 leading-relaxed">
              Agent 请求执行以下操作:
            </p>
            <div className="rounded-md bg-surface-alt border border-border font-mono overflow-hidden">
              <div className="px-3.5 py-2 border-b border-border text-[12px] font-semibold text-fg flex items-center justify-between gap-2">
                <span className="truncate">{request.tool || 'tool'}</span>
                {preview.truncated > 0 && (
                  <span className="shrink-0 text-[10px] font-normal text-warn">仅显示预览</span>
                )}
              </div>
              <pre
                className="px-3.5 py-2.5 text-[11px] leading-relaxed text-fg-2 m-0 whitespace-pre-wrap break-words overflow-auto overscroll-contain"
                style={{ maxHeight: 'min(48vh, 360px)' }}
              >
                {preview.text}
              </pre>
            </div>
          </div>
          <div className="px-4.5 pb-2 flex justify-end gap-2">
            <button
              type="button"
              onClick={() => respond('deny', close)}
              className="px-4 h-8 rounded-md bg-surface-hi text-fg-2 text-[12px] font-medium hover:bg-border transition"
            >
              拒绝
            </button>
            <button
              type="button"
              onClick={() => respond('allow_session', close)}
              className="px-4 h-8 rounded-md border border-accent text-accent bg-transparent text-[12px] font-medium hover:bg-accent-bg transition"
            >
              本次会话允许
            </button>
            <button
              type="button"
              onClick={() => respond('allow', close)}
              className="px-4 h-8 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 transition"
            >
              允许一次
            </button>
          </div>
          <div className="px-4.5 pb-3.5 text-[11px] text-fg-mute">
            提示:可在设置中切换为 Yolo 模式跳过所有确认。
          </div>
        </>
      )}
    </Modal>
  );
}
