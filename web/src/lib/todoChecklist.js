const STATUS_PRESENTATION = {
  pending: {
    markerLabel: 'pending',
    markerClassName: 'text-fg-mute border-border bg-surface',
    textClassName: 'text-fg-2',
    icon: 'none',
  },
  in_progress: {
    markerLabel: 'active',
    markerClassName: 'text-warn border-warn/60 bg-warn/10',
    textClassName: 'text-fg font-medium',
    icon: 'dot',
  },
  completed: {
    markerLabel: 'done',
    markerClassName: 'text-ok border-ok/60 bg-ok-bg',
    textClassName: 'text-fg-mute line-through',
    icon: 'check',
  },
  cancelled: {
    markerLabel: 'cancelled',
    markerClassName: 'text-fg-mute border-border bg-surface-hi',
    textClassName: 'text-fg-mute',
    icon: 'dash',
  },
};

function normalizeStatus(status) {
  return Object.prototype.hasOwnProperty.call(STATUS_PRESENTATION, status) ? status : 'pending';
}

function numericOrNull(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}

export function todoChecklistPresentation(todos = [], summary = null) {
  const sourceItems = Array.isArray(todos) ? todos : [];
  if (sourceItems.length === 0) {
    return { visible: false, done: 0, total: 0, items: [] };
  }

  const items = sourceItems.map((item, index) => {
    const status = normalizeStatus(item?.status);
    const presentation = STATUS_PRESENTATION[status];
    return {
      key: item?.id || String(index),
      status,
      content: item?.content || '(no description)',
      ...presentation,
    };
  });

  const completedFromSummary = numericOrNull(summary?.completed);
  const totalFromSummary = numericOrNull(summary?.total);
  return {
    visible: true,
    done: completedFromSummary ?? items.filter((item) => item.status === 'completed').length,
    total: totalFromSummary ?? items.length,
    items,
  };
}
