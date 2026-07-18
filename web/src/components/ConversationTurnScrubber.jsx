import {
  useCallback,
  useEffect,
  useId,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import {
  conversationTurnMarkerDisplacement,
  conversationTurnMarkerLayout,
  conversationTurnPreviewTop,
  nearestConversationTurnIndex,
} from '../lib/conversationTurnScrubber.js';

const PREVIEW_DISMISS_DELAY_MS = 120;
const PREVIEW_ESTIMATED_HEIGHT = 118;

export function ConversationTurnScrubber({
  turns,
  activeIndex = -1,
  onJump,
}) {
  const railRef = useRef(null);
  const previewRef = useRef(null);
  const dismissTimerRef = useRef(0);
  const pointerFocusIndexRef = useRef(-1);
  const previewId = useId();
  const [railHeight, setRailHeight] = useState(0);
  const [hoveredIndex, setHoveredIndex] = useState(-1);
  const [focusedIndex, setFocusedIndex] = useState(-1);
  const [previewHeight, setPreviewHeight] = useState(PREVIEW_ESTIMATED_HEIGHT);

  const clearDismissTimer = useCallback(() => {
    if (!dismissTimerRef.current) return;
    window.clearTimeout(dismissTimerRef.current);
    dismissTimerRef.current = 0;
  }, []);

  const dismissPreviewSoon = useCallback(() => {
    clearDismissTimer();
    dismissTimerRef.current = window.setTimeout(() => {
      dismissTimerRef.current = 0;
      setHoveredIndex(-1);
    }, PREVIEW_DISMISS_DELAY_MS);
  }, [clearDismissTimer]);

  useLayoutEffect(() => {
    const rail = railRef.current;
    if (!rail) return undefined;

    const measure = () => {
      const nextHeight = rail.getBoundingClientRect().height;
      setRailHeight((previous) => (
        Math.abs(previous - nextHeight) < 0.5 ? previous : nextHeight
      ));
    };
    measure();

    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', measure);
      return () => window.removeEventListener('resize', measure);
    }
    const observer = new ResizeObserver(measure);
    observer.observe(rail);
    return () => observer.disconnect();
  }, []);

  useEffect(() => () => clearDismissTimer(), [clearDismissTimer]);

  const markerLayout = useMemo(
    () => conversationTurnMarkerLayout(turns.length, railHeight),
    [railHeight, turns.length],
  );
  const previewIndex = hoveredIndex;
  const expandedIndex = hoveredIndex >= 0
    ? hoveredIndex
    : (focusedIndex >= 0 ? focusedIndex : activeIndex);
  const previewTurn = previewIndex >= 0 ? turns[previewIndex] : null;
  const previewMarker = previewIndex >= 0 ? markerLayout[previewIndex] : null;

  useLayoutEffect(() => {
    if (!previewTurn || !previewRef.current) return;
    const measuredHeight = previewRef.current.getBoundingClientRect().height;
    if (measuredHeight > 0) {
      setPreviewHeight((previous) => (
        Math.abs(previous - measuredHeight) < 0.5 ? previous : measuredHeight
      ));
    }
  }, [previewTurn]);

  const selectFromPointer = useCallback((event) => {
    const rail = railRef.current;
    if (!rail || markerLayout.length === 0) return;
    clearDismissTimer();

    const rect = rail.getBoundingClientRect();
    const y = event.clientY - rect.top;
    const gap = markerLayout.length > 1
      ? Math.abs(markerLayout[1].centerY - markerLayout[0].centerY)
      : 18;
    setHoveredIndex(nearestConversationTurnIndex(
      y,
      markerLayout,
      Math.max(9, gap / 2),
    ));
  }, [clearDismissTimer, markerLayout]);

  const jumpToTurn = useCallback((turn, index) => {
    if (!turn) return;
    onJump?.(turn, index);
  }, [onJump]);

  const previewTop = previewMarker
    ? conversationTurnPreviewTop(
      previewMarker.centerY,
      railHeight,
      previewHeight,
    )
    : 0;

  return (
    <div
      ref={railRef}
      className="ace-conversation-turn-scrubber"
      data-conversation-turn-scrubber="true"
      aria-label="对话问题轨道"
      onPointerMove={selectFromPointer}
      onPointerLeave={dismissPreviewSoon}
    >
      {markerLayout.map((marker) => {
        const turn = turns[marker.index];
        const previewed = previewIndex === marker.index;
        const expanded = expandedIndex === marker.index;
        const active = activeIndex === marker.index;
        const displacement = conversationTurnMarkerDisplacement(
          marker.index,
          expandedIndex,
          markerLayout.length,
        );
        return (
          <button
            key={turn.itemId}
            type="button"
            className="ace-conversation-turn-scrubber-hit"
            style={{
              top: marker.zoneTop,
              height: marker.zoneHeight,
              '--ace-conversation-turn-marker-shift-y': `${displacement}px`,
            }}
            aria-label={`第 ${turn.messageOrdinal} 个问题：${turn.question}`}
            aria-current={active ? 'step' : undefined}
            aria-describedby={previewed ? previewId : undefined}
            data-active={active ? 'true' : 'false'}
            data-expanded={expanded ? 'true' : 'false'}
            data-previewed={previewed ? 'true' : 'false'}
            onPointerEnter={() => {
              clearDismissTimer();
              setHoveredIndex(marker.index);
            }}
            onPointerDown={() => {
              pointerFocusIndexRef.current = marker.index;
              setFocusedIndex(-1);
            }}
            onPointerUp={() => {
              pointerFocusIndexRef.current = -1;
            }}
            onPointerCancel={() => {
              pointerFocusIndexRef.current = -1;
            }}
            onFocus={() => {
              clearDismissTimer();
              const focusedByPointer = pointerFocusIndexRef.current === marker.index;
              pointerFocusIndexRef.current = -1;
              setFocusedIndex(focusedByPointer ? -1 : marker.index);
            }}
            onBlur={() => {
              setFocusedIndex(-1);
            }}
            onClick={() => jumpToTurn(turn, marker.index)}
            onKeyDown={(event) => {
              if (event.key !== 'Enter' && event.key !== ' ') return;
              event.preventDefault();
              jumpToTurn(turn, marker.index);
            }}
          >
            <span className="ace-conversation-turn-scrubber-dot" aria-hidden="true" />
          </button>
        );
      })}

      {previewTurn && previewMarker && (
        <div
          id={previewId}
          ref={previewRef}
          className="ace-conversation-turn-preview"
          style={{ '--ace-conversation-turn-preview-y': `${previewTop}px` }}
          role="tooltip"
        >
          <div key={previewTurn.itemId} className="ace-conversation-turn-preview-content">
            <div className="ace-conversation-turn-preview-question">
              <strong>{previewTurn.question}</strong>
            </div>
            <div
              className="ace-conversation-turn-preview-answer"
              data-running={previewTurn.running ? 'true' : 'false'}
            >
              <p>{previewTurn.answer}</p>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default ConversationTurnScrubber;
