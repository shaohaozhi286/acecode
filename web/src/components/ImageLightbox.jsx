import { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  clampLightboxTransform,
  LIGHTBOX_MAX_SCALE,
  LIGHTBOX_MIN_SCALE,
  lightboxCanPan,
  panLightboxTransform,
  zoomLightboxTransform,
} from '../lib/imageLightboxTransform.js';
import {
  registerMermaidExportTarget,
  unregisterMermaidExportTarget,
} from '../lib/mermaidExport.js';
import { VsIcon } from './Icon.jsx';

const FIT_TRANSFORM = Object.freeze({ scale: 1, x: 0, y: 0 });
const EMPTY_METRICS = Object.freeze({
  imageWidth: 0,
  imageHeight: 0,
  viewportWidth: 0,
  viewportHeight: 0,
});
const BUTTON_ZOOM_FACTOR = 1.25;
const WHEEL_ZOOM_FACTOR = 1.18;

function sameMetrics(left, right) {
  return left.imageWidth === right.imageWidth
    && left.imageHeight === right.imageHeight
    && left.viewportWidth === right.viewportWidth
    && left.viewportHeight === right.viewportHeight;
}

export function ImageLightbox({ preview, onClose, contextMenuAttrs }) {
  const lightboxRef = useRef(null);
  const viewportRef = useRef(null);
  const imageRef = useRef(null);
  const transformRef = useRef({ ...FIT_TRANSFORM });
  const metricsRef = useRef({ ...EMPTY_METRICS });
  const dragRef = useRef(null);
  const [transform, setTransformState] = useState({ ...FIT_TRANSFORM });
  const [metrics, setMetrics] = useState({ ...EMPTY_METRICS });
  const [dragging, setDragging] = useState(false);
  const previewSource = preview?.src || '';
  const previewWidth = Number.isFinite(preview?.width) && preview.width > 0
    ? Math.ceil(preview.width)
    : undefined;
  const previewHeight = Number.isFinite(preview?.height) && preview.height > 0
    ? Math.ceil(preview.height)
    : undefined;
  const previewCanvasColor = typeof preview?.canvasColor === 'string'
    ? preview.canvasColor
    : undefined;
  const mermaidExport = preview?.mermaidExport || null;

  const updateTransform = useCallback((nextValue) => {
    setTransformState((current) => {
      const next = typeof nextValue === 'function' ? nextValue(current) : nextValue;
      transformRef.current = next;
      return next;
    });
  }, []);

  const measure = useCallback(() => {
    const viewport = viewportRef.current;
    const image = imageRef.current;
    if (!viewport || !image) return metricsRef.current;
    const next = {
      imageWidth: image.offsetWidth,
      imageHeight: image.offsetHeight,
      viewportWidth: viewport.clientWidth,
      viewportHeight: viewport.clientHeight,
    };
    metricsRef.current = next;
    setMetrics((current) => (sameMetrics(current, next) ? current : next));
    updateTransform((current) => clampLightboxTransform(current, next));
    return next;
  }, [updateTransform]);

  useEffect(() => {
    dragRef.current = null;
    setDragging(false);
    metricsRef.current = { ...EMPTY_METRICS };
    setMetrics({ ...EMPTY_METRICS });
    updateTransform({ ...FIT_TRANSFORM });
  }, [previewSource, updateTransform]);

  useEffect(() => {
    if (!previewSource) return undefined;
    const onKeyDown = (event) => {
      if (event.key !== 'Escape') return;
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => window.removeEventListener('keydown', onKeyDown, true);
  }, [onClose, previewSource]);

  useEffect(() => {
    if (!previewSource) return undefined;
    let frame = window.requestAnimationFrame(measure);
    const onResize = () => {
      window.cancelAnimationFrame(frame);
      frame = window.requestAnimationFrame(measure);
    };
    window.addEventListener('resize', onResize);
    return () => {
      window.cancelAnimationFrame(frame);
      window.removeEventListener('resize', onResize);
    };
  }, [measure, previewSource]);

  const zoomTo = useCallback((nextScale, clientPoint = null) => {
    const viewport = viewportRef.current;
    const nextMetrics = measure();
    const rect = viewport?.getBoundingClientRect?.();
    const anchor = clientPoint && rect
      ? { x: clientPoint.x - rect.left, y: clientPoint.y - rect.top }
      : undefined;
    updateTransform((current) => zoomLightboxTransform(
      current,
      nextScale,
      nextMetrics,
      anchor,
    ));
  }, [measure, updateTransform]);

  const resetToFit = useCallback(() => {
    updateTransform({ ...FIT_TRANSFORM });
  }, [updateTransform]);

  const handleWheel = useCallback((event) => {
    if (event.deltaY === 0) return;
    event.preventDefault();
    event.stopPropagation();
    const factor = event.deltaY < 0 ? WHEEL_ZOOM_FACTOR : (1 / WHEEL_ZOOM_FACTOR);
    zoomTo(transformRef.current.scale * factor, { x: event.clientX, y: event.clientY });
  }, [zoomTo]);

  useEffect(() => {
    if (!previewSource) return undefined;
    const viewport = viewportRef.current;
    if (!viewport) return undefined;
    const options = { passive: false };
    viewport.addEventListener('wheel', handleWheel, options);
    return () => viewport.removeEventListener('wheel', handleWheel, options);
  }, [handleWheel, previewSource]);

  const startPan = useCallback((event) => {
    if (event.button !== 0) return;
    const nextMetrics = measure();
    if (!lightboxCanPan(transformRef.current, nextMetrics)) return;
    event.preventDefault();
    event.stopPropagation();
    try {
      event.currentTarget.setPointerCapture?.(event.pointerId);
    } catch {
      // Best effort; regular pointer events continue while the pointer remains over the image.
    }
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      transform: transformRef.current,
    };
    setDragging(true);
  }, [measure]);

  const movePan = useCallback((event) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    event.preventDefault();
    event.stopPropagation();
    updateTransform(panLightboxTransform(drag.transform, {
      x: event.clientX - drag.startX,
      y: event.clientY - drag.startY,
    }, metricsRef.current));
  }, [updateTransform]);

  const stopPan = useCallback((event) => {
    const drag = dragRef.current;
    if (!drag || (event.pointerId != null && drag.pointerId !== event.pointerId)) return;
    dragRef.current = null;
    setDragging(false);
    event.stopPropagation?.();
    try {
      if (event.currentTarget?.hasPointerCapture?.(drag.pointerId)) {
        event.currentTarget.releasePointerCapture?.(drag.pointerId);
      }
    } catch {
      // Pointer may already have been released by the browser.
    }
  }, []);

  useEffect(() => {
    const element = lightboxRef.current;
    if (!element || !mermaidExport) return undefined;
    if (!registerMermaidExportTarget(element, mermaidExport)) return undefined;
    return () => unregisterMermaidExportTarget(element);
  }, [mermaidExport]);

  if (!previewSource) return null;

  const canPan = lightboxCanPan(transform, metrics);
  const controlClass = 'h-8 min-w-8 px-2 rounded-md text-fg border border-border '
    + 'bg-surface hover:bg-surface-hi disabled:opacity-40 disabled:cursor-default '
    + 'flex items-center justify-center';

  return createPortal(
    <div
      ref={lightboxRef}
      className="fixed inset-0 z-[80] bg-black/70"
      data-mermaid-export-target={mermaidExport ? 'true' : undefined}
      role="dialog"
      aria-modal="true"
      aria-label="图片预览"
      onClick={() => onClose?.()}
    >
      <div
        ref={viewportRef}
        className="absolute inset-6 overflow-hidden flex items-center justify-center"
      >
        <img
          ref={imageRef}
          src={previewSource}
          alt={preview.alt || ''}
          width={previewWidth}
          height={previewHeight}
          className="max-w-full max-h-full object-contain shadow-xl select-none"
          {...contextMenuAttrs}
          draggable="false"
          onLoad={measure}
          onDragStart={(event) => event.preventDefault()}
          onClick={(event) => event.stopPropagation()}
          onPointerDown={startPan}
          onPointerMove={movePan}
          onPointerUp={stopPan}
          onPointerCancel={stopPan}
          onLostPointerCapture={stopPan}
          style={{
            cursor: dragging ? 'grabbing' : (canPan ? 'grab' : 'zoom-in'),
            backgroundColor: previewCanvasColor,
            touchAction: 'none',
            transform: `translate3d(${transform.x}px, ${transform.y}px, 0) scale(${transform.scale})`,
            transformOrigin: 'center center',
            transition: dragging ? 'none' : 'transform 80ms ease-out',
            willChange: 'transform',
          }}
        />
      </div>

      <div
        className="absolute top-3 left-1/2 -translate-x-1/2 z-10 flex items-center gap-1 p-1 rounded-lg bg-surface border border-border shadow-lg"
        onClick={(event) => event.stopPropagation()}
      >
        <button
          type="button"
          className={controlClass}
          aria-label="缩小预览"
          title="缩小"
          disabled={transform.scale <= LIGHTBOX_MIN_SCALE}
          onClick={() => zoomTo(transformRef.current.scale / BUTTON_ZOOM_FACTOR)}
        >
          <span aria-hidden="true" className="text-lg leading-none">-</span>
        </button>
        <button
          type="button"
          className={`${controlClass} min-w-[58px] text-xs tabular-nums`}
          aria-label="适应窗口"
          title="适应窗口"
          onClick={resetToFit}
        >
          {Math.round(transform.scale * 100)}%
        </button>
        <button
          type="button"
          className={controlClass}
          aria-label="放大预览"
          title="放大"
          disabled={transform.scale >= LIGHTBOX_MAX_SCALE}
          onClick={() => zoomTo(transformRef.current.scale * BUTTON_ZOOM_FACTOR)}
        >
          <VsIcon name="add" size={15} />
        </button>
      </div>

      <button
        type="button"
        className="absolute top-3 right-3 z-10 w-8 h-8 rounded-md bg-surface text-fg border border-border flex items-center justify-center"
        aria-label="关闭预览"
        title="关闭"
        onClick={(event) => {
          event.stopPropagation();
          onClose?.();
        }}
      >
        <VsIcon name="close" size={15} />
      </button>

      <div className="pointer-events-none absolute bottom-3 left-1/2 -translate-x-1/2 z-10 px-2 py-1 rounded-md bg-surface border border-border text-xs text-fg-2">
        滚轮缩放 | 按住左键拖动
      </div>
    </div>,
    document.body,
  );
}
