import { compactOneLinePreview } from './compactMessagePreview.js';
import { completionSummaryMarkdown } from './taskCompleteSummary.js';

export const MIN_CONVERSATION_TURN_SCRUBBER_TURNS = 5;
export const RUNNING_ANSWER_PREVIEW = '正在回答...';

const EMPTY_ANSWER_PREVIEW = '暂无回答';
const ATTACHMENT_QUESTION_PREVIEW = '附件消息';

function compactPreview(value, limit, fallback) {
  const text = String(value ?? '').trim();
  if (!text) return fallback;
  return compactOneLinePreview(text, limit);
}

function questionText(item) {
  const displayText = item?.metadata?.display_text;
  if (typeof displayText === 'string' && displayText.trim()) {
    return displayText;
  }
  if (typeof item?.content === 'string' && item.content.trim()) {
    return item.content;
  }
  if (Array.isArray(item?.contentParts) && item.contentParts.length > 0) {
    return ATTACHMENT_QUESTION_PREVIEW;
  }
  return '';
}

function stableTurnId(item, index) {
  return String(item?.id || item?.messageId || `conversation-turn-${index}`);
}

export function buildConversationTurnPreviews(
  items,
  {
    busy = false,
    questionLimit = 120,
    answerLimit = 240,
  } = {},
) {
  if (!Array.isArray(items) || items.length === 0) return [];

  const turns = [];
  let currentTurn = null;

  items.forEach((item, itemIndex) => {
    if (item?.kind === 'msg' && item.role === 'user') {
      currentTurn = {
        itemId: stableTurnId(item, itemIndex),
        messageId: String(item.messageId || ''),
        messageOrdinal: turns.length + 1,
        question: compactPreview(
          questionText(item),
          questionLimit,
          ATTACHMENT_QUESTION_PREVIEW,
        ),
        assistantAnswer: '',
        completionAnswer: '',
      };
      turns.push(currentTurn);
      return;
    }

    if (!currentTurn) return;
    if (item?.kind === 'msg' && item.role === 'assistant') {
      const answer = compactPreview(item.content, answerLimit, '');
      if (answer && item.streaming !== true) {
        currentTurn.assistantAnswer = answer;
      }
      return;
    }
    if (item?.kind === 'completion_summary') {
      currentTurn.completionAnswer = compactPreview(
        completionSummaryMarkdown(item, ''),
        answerLimit,
        '',
      );
    }
  });

  return turns.map((turn, index) => {
    const isRunningTurn = busy && index === turns.length - 1;
    return {
      itemId: turn.itemId,
      messageId: turn.messageId,
      messageOrdinal: turn.messageOrdinal,
      question: turn.question,
      answer: turn.completionAnswer
        || turn.assistantAnswer
        || (isRunningTurn ? RUNNING_ANSWER_PREVIEW : EMPTY_ANSWER_PREVIEW),
      running: isRunningTurn && !turn.completionAnswer && !turn.assistantAnswer,
    };
  });
}

export function shouldShowConversationTurnScrubber(
  turns,
  minimumTurns = MIN_CONVERSATION_TURN_SCRUBBER_TURNS,
) {
  return Array.isArray(turns) && turns.length >= Math.max(1, Number(minimumTurns) || 1);
}

export function conversationTurnMarkerLayout(
  count,
  availableHeight,
  {
    idealGap = 18,
    edgePadding = 12,
    minimumHitHeight = 12,
  } = {},
) {
  const markerCount = Math.max(0, Math.floor(Number(count) || 0));
  const height = Math.max(0, Number(availableHeight) || 0);
  if (markerCount === 0 || height === 0) return [];

  const padding = Math.min(
    Math.max(0, Number(edgePadding) || 0),
    height / 2,
  );
  if (markerCount === 1) {
    return [{
      index: 0,
      centerY: height / 2,
      zoneTop: 0,
      zoneHeight: height,
    }];
  }

  const usableHeight = Math.max(0, height - padding * 2);
  const span = Math.min(
    Math.max(0, (markerCount - 1) * Math.max(0, Number(idealGap) || 0)),
    usableHeight,
  );
  const gap = markerCount > 1 ? span / (markerCount - 1) : 0;
  const startY = (height - span) / 2;
  const centers = Array.from(
    { length: markerCount },
    (_, index) => startY + gap * index,
  );
  const hitHeight = Math.max(
    Math.max(0, Number(minimumHitHeight) || 0),
    gap || usableHeight,
  );

  return centers.map((centerY, index) => {
    const previousBoundary = index === 0
      ? centerY - hitHeight / 2
      : (centers[index - 1] + centerY) / 2;
    const nextBoundary = index === markerCount - 1
      ? centerY + hitHeight / 2
      : (centerY + centers[index + 1]) / 2;
    const zoneTop = Math.max(0, previousBoundary);
    const zoneBottom = Math.min(height, nextBoundary);
    return {
      index,
      centerY,
      zoneTop,
      zoneHeight: Math.max(1, zoneBottom - zoneTop),
    };
  });
}

export function nearestConversationTurnIndex(
  pointerY,
  layout,
  maximumDistance = Number.POSITIVE_INFINITY,
) {
  if (!Array.isArray(layout) || layout.length === 0) return -1;
  const y = Number(pointerY);
  if (!Number.isFinite(y)) return -1;

  let nearest = null;
  layout.forEach((marker) => {
    const distance = Math.abs(y - marker.centerY);
    if (!nearest || distance < nearest.distance) {
      nearest = { index: marker.index, distance };
    }
  });
  return nearest && nearest.distance <= maximumDistance ? nearest.index : -1;
}

export function conversationTurnMarkerDisplacement(
  markerIndex,
  expandedIndex,
  markerCount,
  { outwardOffset = 8 } = {},
) {
  const count = Math.max(0, Math.floor(Number(markerCount) || 0));
  const marker = Math.floor(Number(markerIndex));
  const expanded = Math.floor(Number(expandedIndex));
  if (
    count === 0
    || !Number.isFinite(marker)
    || !Number.isFinite(expanded)
    || marker < 0
    || marker >= count
    || expanded < 0
    || expanded >= count
    || marker === expanded
  ) {
    return 0;
  }

  const offset = Math.max(0, Number(outwardOffset) || 0);
  return marker < expanded ? -offset : offset;
}

export function activeConversationTurnIndex(
  turns,
  rowMetrics,
  scrollTop,
  { probeOffset = 48 } = {},
) {
  if (!Array.isArray(turns) || turns.length === 0) return -1;
  if (!Array.isArray(rowMetrics) || rowMetrics.length === 0) return -1;

  const metricById = new Map(
    rowMetrics.map((metric) => [String(metric?.id || ''), metric]),
  );
  const probeY = Math.max(0, Number(scrollTop) || 0)
    + Math.max(0, Number(probeOffset) || 0);
  let activeIndex = -1;

  turns.forEach((turn, index) => {
    const metric = metricById.get(String(turn?.itemId || ''));
    if (metric && Number(metric.top) <= probeY) {
      activeIndex = index;
    }
  });

  return activeIndex >= 0 ? activeIndex : 0;
}

export function activatedConversationTurnIndex(
  turns,
  activation,
  scrollTop,
  { scrollTolerance = 0.75 } = {},
) {
  if (!Array.isArray(turns) || turns.length === 0) return -1;

  const itemId = String(activation?.itemId || '');
  const activatedScrollTop = Number(activation?.scrollTop);
  const currentScrollTop = Number(scrollTop);
  if (
    !itemId
    || !Number.isFinite(activatedScrollTop)
    || !Number.isFinite(currentScrollTop)
  ) {
    return -1;
  }

  const tolerance = Math.max(0, Number(scrollTolerance) || 0);
  if (Math.abs(currentScrollTop - activatedScrollTop) > tolerance) {
    return -1;
  }

  return turns.findIndex((turn) => String(turn?.itemId || '') === itemId);
}

export function conversationTurnPreviewTop(
  markerCenterY,
  railHeight,
  cardHeight,
  margin = 8,
) {
  const height = Math.max(0, Number(railHeight) || 0);
  const previewHeight = Math.max(0, Number(cardHeight) || 0);
  const edge = Math.max(0, Number(margin) || 0);
  const preferred = (Number(markerCenterY) || 0) - previewHeight / 2;
  return Math.min(
    Math.max(edge, preferred),
    Math.max(edge, height - previewHeight - edge),
  );
}
