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
  MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
  centeredConversationTurnWindowStart,
  conversationTurnMarkerDisplacement,
  conversationTurnMarkerLayout,
  conversationTurnPageControlTop,
  conversationTurnPreviewTop,
  conversationTurnSteppedWindowStart,
  conversationTurnWheelImpulse,
  conversationTurnWindow,
  conversationTurnWindowStartContainingIndex,
  nearestConversationTurnIndex,
  nextConversationTurnHoldInterval,
} from '../lib/conversationTurnScrubber.js';

const PREVIEW_DISMISS_DELAY_MS = 120;
const PREVIEW_ESTIMATED_HEIGHT = 118;
const WHEEL_INERTIA_TRANSFER = 0.2;
const WHEEL_INERTIA_FRICTION = 0.84;
const WHEEL_INERTIA_STOP_VELOCITY = 0.02;
const WHEEL_MAX_VELOCITY = 0.9;
const WHEEL_MIN_STEP_INTERVAL_MS = 72;
const ARROW_HOLD_DELAY_MS = 320;
const ARROW_HOLD_INITIAL_INTERVAL_MS = 140;

export function ConversationTurnScrubber({
  turns,
  activeIndex = -1,
  onJump,
}) {
  const railRef = useRef(null);
  const previewRef = useRef(null);
  const dismissTimerRef = useRef(0);
  const pointerFocusIndexRef = useRef(-1);
  const turnsIdentityRef = useRef('');
  const pagingAvailabilityRef = useRef({ hasPrevious: false, hasNext: false });
  const wheelMotionCancelRef = useRef(null);
  const arrowHoldRef = useRef(null);
  const suppressPageClickRef = useRef(false);
  const previewId = useId();
  const [railHeight, setRailHeight] = useState(0);
  const [hoveredIndex, setHoveredIndex] = useState(-1);
  const [focusedIndex, setFocusedIndex] = useState(-1);
  const [previewHeight, setPreviewHeight] = useState(PREVIEW_ESTIMATED_HEIGHT);
  const [windowStart, setWindowStart] = useState(() => (
    centeredConversationTurnWindowStart(
      activeIndex,
      turns.length,
      MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
    )
  ));

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

  const turnsIdentity = `${turns[0]?.itemId || ''}:${turns[turns.length - 1]?.itemId || ''}:${turns.length}`;

  useEffect(() => {
    const identityChanged = turnsIdentityRef.current !== turnsIdentity;
    turnsIdentityRef.current = turnsIdentity;
    setWindowStart((currentStart) => (
      identityChanged
        ? centeredConversationTurnWindowStart(
          activeIndex,
          turns.length,
          MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
        )
        : conversationTurnWindowStartContainingIndex(
          currentStart,
          activeIndex,
          turns.length,
          MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
        )
    ));
  }, [activeIndex, turns.length, turnsIdentity]);

  const visibleWindow = useMemo(
    () => conversationTurnWindow(
      turns.length,
      windowStart,
      MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
    ),
    [turns.length, windowStart],
  );
  pagingAvailabilityRef.current = {
    hasPrevious: visibleWindow.hasPrevious,
    hasNext: visibleWindow.hasNext,
  };
  const markerLayout = useMemo(
    () => conversationTurnMarkerLayout(
      visibleWindow.visibleCount,
      railHeight,
      { edgePadding: visibleWindow.paginated ? 42 : 12 },
    ).map((marker) => ({
      ...marker,
      index: visibleWindow.start + marker.index,
    })),
    [
      railHeight,
      visibleWindow.paginated,
      visibleWindow.start,
      visibleWindow.visibleCount,
    ],
  );
  const previousPageTop = markerLayout.length > 0
    ? conversationTurnPageControlTop(
      markerLayout[0].centerY,
      -1,
      railHeight,
    )
    : 0;
  const nextPageTop = markerLayout.length > 0
    ? conversationTurnPageControlTop(
      markerLayout[markerLayout.length - 1].centerY,
      1,
      railHeight,
    )
    : 0;
  const indexIsVisible = useCallback((index) => (
    index >= visibleWindow.start && index < visibleWindow.end
  ), [visibleWindow.end, visibleWindow.start]);
  const previewIndex = indexIsVisible(hoveredIndex) ? hoveredIndex : -1;
  const expandedCandidate = hoveredIndex >= 0
    ? hoveredIndex
    : (focusedIndex >= 0 ? focusedIndex : activeIndex);
  const expandedIndex = indexIsVisible(expandedCandidate)
    ? expandedCandidate
    : -1;
  const previewTurn = previewIndex >= 0 ? turns[previewIndex] : null;
  const previewMarker = previewIndex >= 0
    ? markerLayout.find((marker) => marker.index === previewIndex)
    : null;

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
    if (event.target?.closest?.('[data-conversation-turn-page-control]')) {
      setHoveredIndex(-1);
      return;
    }

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
    wheelMotionCancelRef.current?.();
    setWindowStart(centeredConversationTurnWindowStart(
      index,
      turns.length,
      MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
    ));
    onJump?.(turn, index);
  }, [onJump, turns.length]);

  const stepWindow = useCallback((direction) => {
    clearDismissTimer();
    setHoveredIndex(-1);
    setFocusedIndex(-1);
    setWindowStart((currentStart) => (
      conversationTurnSteppedWindowStart(
        currentStart,
        direction,
        turns.length,
        MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
      )
    ));
  }, [clearDismissTimer, turns.length]);

  const canStepWindow = useCallback((direction) => {
    const availability = pagingAvailabilityRef.current;
    if (direction < 0) return availability.hasPrevious;
    if (direction > 0) return availability.hasNext;
    return false;
  }, []);

  const stopArrowHold = useCallback((suppressTrailingClick = false) => {
    const hold = arrowHoldRef.current;
    if (!hold) return;
    arrowHoldRef.current = null;
    if (hold.timer) window.clearTimeout(hold.timer);
    if (suppressTrailingClick && hold.repeated) {
      suppressPageClickRef.current = true;
      window.setTimeout(() => {
        suppressPageClickRef.current = false;
      }, 0);
    }
  }, []);

  const startArrowHold = useCallback((event, direction) => {
    if (event.button !== 0 || !canStepWindow(direction)) return;
    wheelMotionCancelRef.current?.();
    stopArrowHold(false);
    event.currentTarget.setPointerCapture?.(event.pointerId);

    const hold = {
      direction,
      interval: ARROW_HOLD_INITIAL_INTERVAL_MS,
      repeated: false,
      timer: 0,
    };
    arrowHoldRef.current = hold;

    const repeat = () => {
      if (arrowHoldRef.current !== hold) return;
      if (!canStepWindow(hold.direction)) {
        hold.timer = 0;
        return;
      }
      hold.repeated = true;
      stepWindow(hold.direction);
      hold.interval = nextConversationTurnHoldInterval(hold.interval);
      hold.timer = window.setTimeout(repeat, hold.interval);
    };
    hold.timer = window.setTimeout(repeat, ARROW_HOLD_DELAY_MS);
  }, [canStepWindow, stepWindow, stopArrowHold]);

  const activatePageControl = useCallback((direction) => {
    if (suppressPageClickRef.current) {
      suppressPageClickRef.current = false;
      return;
    }
    wheelMotionCancelRef.current?.();
    stepWindow(direction);
  }, [stepWindow]);

  useEffect(() => {
    const finishHold = () => stopArrowHold(true);
    const cancelHold = () => stopArrowHold(false);
    window.addEventListener('pointerup', finishHold);
    window.addEventListener('pointercancel', cancelHold);
    window.addEventListener('blur', cancelHold);
    return () => {
      window.removeEventListener('pointerup', finishHold);
      window.removeEventListener('pointercancel', cancelHold);
      window.removeEventListener('blur', cancelHold);
      stopArrowHold(false);
      suppressPageClickRef.current = false;
    };
  }, [stopArrowHold, turnsIdentity]);

  useEffect(() => {
    const rail = railRef.current;
    if (!rail || !visibleWindow.paginated) return undefined;

    let frame = 0;
    let velocity = 0;
    let carry = 0;
    let lastStepAt = Number.NEGATIVE_INFINITY;

    const stopWheelMotion = () => {
      if (frame) window.cancelAnimationFrame(frame);
      frame = 0;
      velocity = 0;
      carry = 0;
      lastStepAt = Number.NEGATIVE_INFINITY;
    };

    const advanceWheelMotion = (timestamp) => {
      frame = 0;
      carry += velocity;
      if (
        Math.abs(carry) >= 1
        && timestamp - lastStepAt >= WHEEL_MIN_STEP_INTERVAL_MS
      ) {
        const direction = Math.sign(carry);
        if (!canStepWindow(direction)) {
          stopWheelMotion();
          return;
        }
        stepWindow(direction);
        carry -= direction;
        lastStepAt = timestamp;
      }

      velocity *= WHEEL_INERTIA_FRICTION;
      if (Math.abs(velocity) < WHEEL_INERTIA_STOP_VELOCITY) velocity = 0;
      if (velocity !== 0 || Math.abs(carry) >= 1) {
        frame = window.requestAnimationFrame(advanceWheelMotion);
      } else {
        carry = 0;
      }
    };

    wheelMotionCancelRef.current = stopWheelMotion;

    const handleWheel = (event) => {
      if (event.ctrlKey || event.metaKey) return;
      const impulse = conversationTurnWheelImpulse(
        event.deltaY,
        event.deltaX,
        event.deltaMode,
        rail.clientHeight,
      );
      if (impulse === 0) return;

      event.preventDefault();
      event.stopPropagation();
      stopArrowHold(true);

      const direction = Math.sign(impulse);
      const currentDirection = Math.sign(velocity || carry);
      if (currentDirection !== 0 && currentDirection !== direction) {
        velocity = 0;
        carry = 0;
        lastStepAt = Number.NEGATIVE_INFINITY;
      }
      carry += impulse;
      velocity = Math.min(
        WHEEL_MAX_VELOCITY,
        Math.max(
          -WHEEL_MAX_VELOCITY,
          velocity + impulse * WHEEL_INERTIA_TRANSFER,
        ),
      );
      if (!frame) frame = window.requestAnimationFrame(advanceWheelMotion);
    };

    rail.addEventListener('wheel', handleWheel, { passive: false });
    return () => {
      rail.removeEventListener('wheel', handleWheel);
      stopWheelMotion();
      if (wheelMotionCancelRef.current === stopWheelMotion) {
        wheelMotionCancelRef.current = null;
      }
    };
  }, [canStepWindow, stepWindow, stopArrowHold, turnsIdentity, visibleWindow.paginated]);

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
      data-window-start={visibleWindow.start}
      data-window-end={visibleWindow.end}
      data-window-size={visibleWindow.visibleCount}
      aria-label="对话问题轨道"
      onPointerMove={selectFromPointer}
      onPointerLeave={dismissPreviewSoon}
    >
      {visibleWindow.paginated && markerLayout.length > 0 && (
        <button
          type="button"
          className="ace-conversation-turn-page-button"
          data-conversation-turn-page-control="previous"
          style={{ top: previousPageTop }}
          aria-label="向上查看更早的问题"
          disabled={!visibleWindow.hasPrevious}
          onPointerEnter={() => {
            clearDismissTimer();
            setHoveredIndex(-1);
          }}
          onPointerDown={(event) => startArrowHold(event, -1)}
          onPointerUp={() => stopArrowHold(true)}
          onPointerCancel={() => stopArrowHold(false)}
          onLostPointerCapture={() => stopArrowHold(false)}
          onClick={() => activatePageControl(-1)}
        >
          <svg viewBox="0 0 20 20" aria-hidden="true">
            <path d="m5 12 5-5 5 5" />
          </svg>
        </button>
      )}

      {markerLayout.map((marker) => {
        const turn = turns[marker.index];
        const previewed = previewIndex === marker.index;
        const expanded = expandedIndex === marker.index;
        const active = activeIndex === marker.index;
        const displacement = conversationTurnMarkerDisplacement(
          marker.index,
          expandedIndex,
          turns.length,
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

      {visibleWindow.paginated && markerLayout.length > 0 && (
        <button
          type="button"
          className="ace-conversation-turn-page-button"
          data-conversation-turn-page-control="next"
          style={{ top: nextPageTop }}
          aria-label="向下查看更晚的问题"
          disabled={!visibleWindow.hasNext}
          onPointerEnter={() => {
            clearDismissTimer();
            setHoveredIndex(-1);
          }}
          onPointerDown={(event) => startArrowHold(event, 1)}
          onPointerUp={() => stopArrowHold(true)}
          onPointerCancel={() => stopArrowHold(false)}
          onLostPointerCapture={() => stopArrowHold(false)}
          onClick={() => activatePageControl(1)}
        >
          <svg viewBox="0 0 20 20" aria-hidden="true">
            <path d="m5 8 5 5 5-5" />
          </svg>
        </button>
      )}

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
