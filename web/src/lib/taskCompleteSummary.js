const SUMMARY_LABEL_RE = /^(?:总结|summary)\s*[:：]\s*/i;
const STRUCTURED_MARKDOWN_RE = /(?:^|\n)\s*(?:[-*+]|#{1,6}\s|>|\|)/;
const FENCE_RE = /```/;
const NUMBERED_MARKER_RE = /(^|[\s,，;；:：。])(\d{1,2})[).、]\s*/g;

export function stripCompletionSummaryLabel(value) {
  return String(value ?? '').trim().replace(SUMMARY_LABEL_RE, '').trim();
}

function numberedMarkers(text) {
  const markers = [];
  NUMBERED_MARKER_RE.lastIndex = 0;
  for (const match of text.matchAll(NUMBERED_MARKER_RE)) {
    const markerStart = match.index + match[1].length;
    markers.push({
      number: match[2],
      start: markerStart,
      end: match.index + match[0].length,
    });
  }
  return markers;
}

function expandCompactNumberedList(text) {
  if (!text || FENCE_RE.test(text)) {
    return text;
  }
  if (text.includes('\n')) return text;
  if (STRUCTURED_MARKDOWN_RE.test(text)) return text;

  const markers = numberedMarkers(text);
  if (markers.length < 2) return text;

  const prefix = text.slice(0, markers[0].start).trim().replace(/[，,;；]\s*$/, '');
  const items = markers.map((marker, index) => {
    const next = markers[index + 1];
    const body = text
      .slice(marker.end, next ? next.start : text.length)
      .trim()
      .replace(/^[，,;；:：。]\s*/, '')
      .replace(/[;；]\s*$/, '')
      .trim();
    return body ? `${marker.number}. ${body}` : '';
  }).filter(Boolean);

  if (items.length < 2) return text;
  return [prefix, items.join('\n')].filter(Boolean).join('\n\n');
}

export function normalizeTaskCompleteMarkdown(value, fallback = '已完成') {
  const text = stripCompletionSummaryLabel(value);
  if (!text) return fallback;
  return expandCompactNumberedList(text);
}

export function completionSummaryMarkdown(item, fallback = '已完成') {
  if (typeof item?.summary === 'string' && item.summary.trim()) {
    return normalizeTaskCompleteMarkdown(item.summary, fallback);
  }
  const title = typeof item?.title === 'string' ? item.title.trim() : '';
  if (!title) return fallback;
  return normalizeTaskCompleteMarkdown(title, fallback);
}
