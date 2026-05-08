// 点击宫格里的 MiniSession 后展开为完整会话覆盖层。
// 接住整个主区,内嵌一个简化版 ChatView(直接重用 ChatView 组件,有 sessionId 即可)。
//
// sessionRef 经 sessionRefFromGridPayload 规范化:server 给 Grid 的 payload
// 是 snake_case (workspace_hash / display_title / message_count),原先这里
// 直接读 camelCase 属性会全部 undefined,下游 ChatView 把 undefined 拼进
// `/api/workspaces/${workspaceHash}/...` URL → 404 workspace not found。
// 详见 openspec/changes/fix-desktop-grid-show-pinned-only。

import { useCallback, useEffect, useMemo, useState } from 'react';
import { ChatView } from './ChatView.jsx';
import { clsx } from '../lib/format.js';
import { sessionRefFromGridPayload } from '../lib/gridPinnedSessions.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import { VsIcon } from './Icon.jsx';

export function ExpandedOverlay({ session, onClose }) {
  const [show, setShow] = useState(false);
  // 用 useMemo 稳定 sessionRef 引用,避免 ChatView 内部依赖 ref 的
  // useMemo / useEffect 因父组件每次 render 新建对象而抖动。
  const sessionRef = useMemo(() => sessionRefFromGridPayload(session), [session]);
  useEffect(() => { requestAnimationFrame(() => setShow(true)); }, []);
  const close = useCallback(() => {
    setShow(false);
    setTimeout(onClose, 240);
  }, [onClose]);

  useEffect(() => {
    const onKeyDown = (event) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      close();
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [close]);

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
            {sessionDisplayTitle(session)}
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
            className="w-6 h-6 rounded border border-border bg-surface-alt text-fg-mute hover:text-fg hover:bg-surface-hi transition flex items-center justify-center"
          ><VsIcon name="close" size={12} /></button>
        </div>
        <div className="flex-1 flex overflow-hidden">
          {sessionRef && (
            <ChatView
              sessionRef={sessionRef}
              onSessionPromoted={() => {}}
              health={null}
              onPermissionRequest={() => {}}
              onQuestionRequest={() => {}}
            />
          )}
        </div>
      </div>
    </div>
  );
}
