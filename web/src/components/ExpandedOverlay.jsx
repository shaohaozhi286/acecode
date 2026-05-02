// 点击宫格里的 MiniSession 后展开为完整会话覆盖层。
// 接住整个主区,内嵌一个简化版 ChatView(直接重用 ChatView 组件,有 sessionId 即可)。

import { useEffect, useState } from 'react';
import { ChatView } from './ChatView.jsx';
import { clsx } from '../lib/format.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';

export function ExpandedOverlay({ session, onClose }) {
  const [show, setShow] = useState(false);
  useEffect(() => { requestAnimationFrame(() => setShow(true)); }, []);
  const close = () => { setShow(false); setTimeout(onClose, 240); };

  return (
    <div
      className={clsx(
        'absolute inset-0 z-[100] flex items-center justify-center p-5 transition-colors duration-250',
        show ? 'bg-black/30' : 'bg-black/0',
      )}
      onClick={close}
    >
      <div
        className={clsx(
          'w-full h-full max-w-[1100px] rounded-xl bg-surface border-2 border-accent ace-shadow-lg overflow-hidden flex flex-col transition-all duration-250',
          show ? 'opacity-100 scale-100' : 'opacity-0 scale-[.92]',
        )}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="h-9 px-3.5 flex items-center gap-2 bg-accent-bg border-b border-border shrink-0">
          <span className="flex-1 text-[13px] font-semibold text-accent truncate">
            {sessionDisplayTitle(session, session.id)}
          </span>
          <button
            type="button"
            onClick={close}
            className="px-3 h-6 rounded border border-border bg-surface-alt text-fg-2 text-[11px] hover:bg-surface-hi transition"
          >
            返回宫格
          </button>
          <button
            type="button"
            onClick={close}
            className="w-6 h-6 rounded border border-border bg-surface-alt text-fg-mute hover:text-fg hover:bg-surface-hi transition"
          >✕</button>
        </div>
        <div className="flex-1 flex overflow-hidden">
          <ChatView
            sessionRef={{
              sessionId: session.sessionId || session.id,
              port: session.port,
              token: session.token,
              contextId: session.contextId,
              workspaceHash: session.workspaceHash,
              title: session.title,
              summary: session.summary,
              message_count: session.message_count,
              created_at: session.created_at,
              updated_at: session.updated_at,
            }}
            onSessionPromoted={() => {}}
            health={null}
            onPermissionRequest={() => {}}
            onQuestionRequest={() => {}}
          />
        </div>
      </div>
    </div>
  );
}
