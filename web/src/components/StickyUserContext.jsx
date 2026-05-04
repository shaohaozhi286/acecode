import { VsIcon } from './Icon.jsx';

export function StickyUserContext({ context }) {
  if (!context || !context.content) return null;
  return (
    <div className="ace-sticky-user-context-wrap" aria-live="polite">
      <div
        className="ace-sticky-user-context"
        title={context.content}
        aria-label={`当前问题:${context.content}`}
      >
        <div className="ace-sticky-user-context-label">
          <VsIcon name="document" size={13} />
          <span>当前问题</span>
        </div>
        <div className="ace-sticky-user-context-text">
          {context.content}
        </div>
      </div>
    </div>
  );
}
