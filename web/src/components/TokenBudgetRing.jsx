import { clsx } from '../lib/format.js';

const RING_COLOR = {
  safe: 'var(--ace-accent)',
  warning: 'var(--ace-warn)',
  danger: 'var(--ace-danger)',
  unknown: 'var(--ace-fg-mute)',
};

export function TokenBudgetRing({ budget, className = '' }) {
  if (!budget) return null;

  const radius = 6;
  const circumference = 2 * Math.PI * radius;
  const known = !!budget.known;
  const ratio = Math.min(1, Math.max(0, Number(budget.usedRatio) || 0));
  const dashOffset = circumference * (1 - ratio);
  const color = RING_COLOR[budget.severity] || RING_COLOR.unknown;
  const label = budget.ariaLabel || budget.title || 'Token usage unavailable';

  return (
    <span
      className={clsx('inline-flex items-center justify-center w-4 h-4 shrink-0', className)}
      role="img"
      aria-label={label}
      title={budget.title || label}
      data-token-budget-severity={budget.severity || 'unknown'}
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
    </span>
  );
}
