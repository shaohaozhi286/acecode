// permission_request 弹框。Allow / AllowAlways / Deny 三按钮。
// 多 request 排队由 App 层管(本组件每次只显示一个)。

import { connection } from '../lib/connection.js';
import { Modal } from './Modal.jsx';

export function PermissionModal({ request, onResolve }) {
  const respond = (choice, close) => {
    connection.sendDecision(request.request_id, choice, request.session_id);
    close();
    setTimeout(() => onResolve?.(), 220);
  };

  return (
    <Modal width={460} onClose={onResolve}>
      {({ close }) => (
        <>
          <div className="px-4.5 py-3 bg-warn/10 border-b border-border flex items-center gap-2">
            <span className="text-base">⚠️</span>
            <h3 className="text-[14px] font-semibold">权限请求</h3>
          </div>
          <div className="px-4.5 py-4 flex flex-col gap-3">
            <p className="text-[13px] text-fg-2 leading-relaxed">
              Agent 请求执行以下操作:
            </p>
            <div className="px-3.5 py-2.5 rounded-md bg-surface-alt border border-border font-mono">
              <div className="text-[12px] font-semibold text-fg mb-1">{request.tool || 'tool'}</div>
              <pre className="text-[11px] text-fg-2 m-0 whitespace-pre-wrap break-all">
                {typeof request.args === 'string' ? request.args : JSON.stringify(request.args || {}, null, 2)}
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
