import { VsIcon } from './Icon.jsx';

export function StickyUserContext({ context, onJumpToSource }) {
  if (!context || !context.content) return null;
  const canJump = typeof onJumpToSource === 'function';
  const StickySurface = canJump ? 'button' : 'div';
  return (
    <div className="ace-sticky-user-context-wrap" aria-live="polite">
      <StickySurface
        type={canJump ? 'button' : undefined}
        className={`ace-sticky-user-context${canJump ? ' ace-sticky-user-context-clickable' : ''}`}
        title={context.content}
        aria-label={canJump ? `跳转到当前问题:${context.content}` : `当前问题:${context.content}`}
        onClick={canJump ? () => onJumpToSource(context) : undefined}
      >
        <div className="ace-sticky-user-context-label">
          <VsIcon name="document" size={13} />
          <span>当前问题</span>
        </div>
        <div className="ace-sticky-user-context-text">
          {context.content}
        </div>
      </StickySurface>
    </div>
  );
}
