import { useCallback, useEffect, useId, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { clsx } from '../lib/format.js';

const RING_COLOR = {
  safe: 'var(--ace-accent)',
  warning: 'var(--ace-warn)',
  danger: 'var(--ace-danger)',
  unknown: 'var(--ace-fg-mute)',
};

export function TokenBudgetRing({ budget, className = '' }) {
  const anchorRef = useRef(null);
  const tooltipId = useId();
  const [tip, setTip] = useState(null);

  const radius = 6;
  const circumference = 2 * Math.PI * radius;
  const known = !!budget?.known;
  const ratio = Math.min(1, Math.max(0, Number(budget?.usedRatio) || 0));
  const dashOffset = circumference * (1 - ratio);
  const color = RING_COLOR[budget?.severity] || RING_COLOR.unknown;
  const label = budget?.ariaLabel || budget?.title || 'Token usage unavailable';
  const tooltipText = budget?.title || label;

  const showTip = useCallback((mode = 'hover') => {
    if (!tooltipText) return;
    const el = anchorRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const viewportWidth = window.innerWidth || 0;
    const maxTipWidth = Math.min(320, Math.max(120, viewportWidth - 16));
    const center = r.left + (r.width / 2);
    const left = Math.round(Math.min(
      viewportWidth - 8 - (maxTipWidth / 2),
      Math.max(8 + (maxTipWidth / 2), center),
    ));
    const placement = r.top < 72 ? 'below' : 'above';
    setTip({
      mode,
      placement,
      left,
      top: placement === 'below' ? Math.round(r.bottom + 6) : undefined,
      bottom: placement === 'above' ? Math.round(window.innerHeight - r.top + 6) : undefined,
    });
  }, [tooltipText]);

  const closeTip = useCallback(() => setTip(null), []);
  const closeHoverTip = useCallback(() => {
    setTip((current) => (current?.mode === 'click' ? current : null));
  }, []);

  useEffect(() => {
    if (tip?.mode !== 'click') return undefined;
    const onPointerDown = (event) => {
      if (anchorRef.current?.contains(event.target)) return;
      closeTip();
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') closeTip();
    };
    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    window.addEventListener('resize', closeTip);
    document.addEventListener('scroll', closeTip, true);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('resize', closeTip);
      document.removeEventListener('scroll', closeTip, true);
    };
  }, [closeTip, tip?.mode]);

  if (!budget) return null;

  return (
    <button
      ref={anchorRef}
      type="button"
      className={clsx('ace-token-budget-button inline-flex items-center justify-center w-4 h-4 shrink-0', className)}
      aria-label={label}
      aria-describedby={tip ? tooltipId : undefined}
      onMouseEnter={() => showTip('hover')}
      onMouseLeave={closeHoverTip}
      onFocus={() => showTip('hover')}
      onBlur={closeTip}
      onPointerDown={(event) => {
        if (event.button !== 0) return;
        showTip('click');
      }}
      onClick={(event) => {
        event.stopPropagation();
        showTip('click');
      }}
      data-token-budget-severity={budget?.severity || 'unknown'}
    >
      <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true" className="block">
        <circle
          cx="8"
          cy="8"
          r={radius}
          fill="none"
          stroke="rgba(var(--ace-fg-mute-rgb), 0.28)"
          strokeWidth="2"
        />
        {known ? (
          <circle
            cx="8"
            cy="8"
            r={radius}
            fill="none"
            stroke={color}
            strokeWidth="2"
            strokeLinecap="round"
            strokeDasharray={`${circumference} ${circumference}`}
            strokeDashoffset={dashOffset}
            transform="rotate(-90 8 8)"
          />
        ) : (
          <circle
            cx="8"
            cy="8"
            r={radius}
            fill="none"
            stroke={color}
            strokeWidth="1.6"
            strokeLinecap="round"
            strokeDasharray="1.5 2.4"
            opacity="0.85"
          />
        )}
      </svg>
      {tip
        ? createPortal(
            <span
              id={tooltipId}
              className="ace-token-budget-tip"
              role="tooltip"
              data-placement={tip.placement}
              style={{
                left: tip.left,
                top: tip.placement === 'below' ? tip.top : undefined,
                bottom: tip.placement === 'above' ? tip.bottom : undefined,
              }}
            >
              {tooltipText}
            </span>,
            document.body,
          )
        : null}
    </button>
  );
}
