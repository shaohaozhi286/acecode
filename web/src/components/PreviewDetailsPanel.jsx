import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { usePreference } from '../lib/usePreference.js';
import { clsx } from '../lib/format.js';
import { PREVIEW_TAB_TYPES } from '../lib/previewTabs.js';
import { scrollLeftForVisibleTab } from '../lib/previewTabScroll.js';
import { FilePreviewContent } from './FilePreviewContent.jsx';
import { SessionChangeDetails } from './ChangeReview.jsx';
import { FileTypeIcon, VsIcon } from './Icon.jsx';

const FILE_PREVIEW_WRAP_STORAGE_KEY = 'acecode.filePreviewWrap.v1';

function validateBooleanPreference(value) {
  return typeof value === 'boolean';
}

function fileName(path) {
  const name = String(path || '').split(/[\\/]/).filter(Boolean).pop();
  return name || String(path || '文件');
}

function tabLabel(tab) {
  if (!tab) return '';
  if (tab.type === PREVIEW_TAB_TYPES.SESSION_CHANGES) {
    const count = Number(tab.fileCount || 0);
    return `会话变更(${count} 文件)`;
  }
  return tab.title || fileName(tab.path);
}

function wheelDeltaForTabs(event, pageWidth) {
  const deltaX = Number(event.deltaX) || 0;
  const deltaY = Number(event.deltaY) || 0;
  let delta = Math.abs(deltaX) > Math.abs(deltaY) ? deltaX : deltaY;
  if (!delta) return 0;
  if (event.deltaMode === 1) delta *= 16;
  else if (event.deltaMode === 2) delta *= Math.max(1, pageWidth);
  return delta;
}

function PreviewTabScrollbar({ scrollRef }) {
  const dragRef = useRef(null);
  const cleanupDragRef = useRef(null);
  const [metrics, setMetrics] = useState({ overflow: false, left: 0, width: 0 });

  const updateMetrics = useCallback(() => {
    const el = scrollRef.current;
    if (!el) return;
    const client = el.clientWidth;
    const scroll = el.scrollWidth;
    const overflow = scroll > client + 1;
    if (!overflow) {
      setMetrics((prev) => (
        !prev.overflow && prev.left === 0 && prev.width === 0
          ? prev
          : { overflow: false, left: 0, width: 0 }
      ));
      return;
    }
    const maxScroll = Math.max(1, scroll - client);
    const width = Math.max(24, Math.round((client / scroll) * client));
    const maxLeft = Math.max(0, client - width);
    const left = Math.round((el.scrollLeft / maxScroll) * maxLeft);
    setMetrics((prev) => (
      prev.overflow && prev.left === left && prev.width === width
        ? prev
        : { overflow: true, left, width }
    ));
  }, [scrollRef]);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;
    updateMetrics();
    el.addEventListener('scroll', updateMetrics, { passive: true });
    const resizeObserver = typeof ResizeObserver !== 'undefined'
      ? new ResizeObserver(updateMetrics)
      : null;
    resizeObserver?.observe(el);
    return () => {
      el.removeEventListener('scroll', updateMetrics);
      resizeObserver?.disconnect();
    };
  }, [scrollRef, updateMetrics]);

  useEffect(() => {
    updateMetrics();
  });

  useEffect(() => () => {
    cleanupDragRef.current?.();
  }, []);

  const onPointerDown = useCallback((event) => {
    const el = scrollRef.current;
    if (!el || !metrics.overflow) return;
    event.preventDefault();
    cleanupDragRef.current?.();
    const rect = event.currentTarget.getBoundingClientRect();
    const thumb = event.currentTarget.querySelector('.ace-preview-details-tab-scrollbar-thumb');
    const thumbRect = thumb?.getBoundingClientRect();
    const onThumb = thumbRect
      && event.clientX >= thumbRect.left
      && event.clientX <= thumbRect.right;
    const pointerOffset = onThumb && thumbRect
      ? event.clientX - thumbRect.left
      : metrics.width / 2;
    dragRef.current = {
      trackLeft: rect.left,
      trackWidth: rect.width,
      thumbWidth: metrics.width,
      pointerOffset,
    };
    document.body.classList.add('ace-tab-scroll-dragging');
    const onMove = (moveEvent) => {
      const drag = dragRef.current;
      if (!drag) return;
      const maxLeft = Math.max(1, drag.trackWidth - drag.thumbWidth);
      const nextLeft = Math.min(
        Math.max(0, moveEvent.clientX - drag.trackLeft - drag.pointerOffset),
        maxLeft,
      );
      const maxScroll = Math.max(1, el.scrollWidth - el.clientWidth);
      el.scrollLeft = (nextLeft / maxLeft) * maxScroll;
    };
    const onUp = () => {
      dragRef.current = null;
      document.body.classList.remove('ace-tab-scroll-dragging');
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onUp);
      cleanupDragRef.current = null;
    };
    cleanupDragRef.current = onUp;
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onUp);
    onMove(event);
  }, [metrics.overflow, metrics.width, scrollRef]);

  if (!metrics.overflow) return null;

  return (
    <div
      className="ace-preview-details-tab-scrollbar"
      aria-hidden="true"
      onPointerDown={onPointerDown}
    >
      <div
        className="ace-preview-details-tab-scrollbar-thumb"
        style={{
          transform: `translateX(${metrics.left}px)`,
          width: `${metrics.width}px`,
        }}
      />
    </div>
  );
}

export function PreviewDetailsPanel({
  api,
  cwd,
  tabs = [],
  activeTab = null,
  changeGroups = [],
  changeSummary = null,
  refreshToken = 0,
  maximized = false,
  onActivateTab,
  onCloseTab,
  onCloseAll,
  onToggleMaximize,
}) {
  const tabListRef = useRef(null);
  const [wrapPreview, setWrapPreview] = usePreference(
    FILE_PREVIEW_WRAP_STORAGE_KEY,
    false,
    validateBooleanPreference,
  );
  const active = activeTab || tabs[0] || null;
  const renderedBody = useMemo(() => {
    if (!active) return null;
    if (active.type === PREVIEW_TAB_TYPES.SESSION_CHANGES) {
      return (
        <SessionChangeDetails
          groups={changeGroups}
          summary={changeSummary}
          cwd={cwd}
          expandedFile={active.expandedFile || ''}
        />
      );
    }
    return (
      <FilePreviewContent
        api={api}
        cwd={cwd}
        path={active.path}
        wrapPreview={wrapPreview}
        onToggleWrapPreview={() => setWrapPreview((prev) => !prev)}
        refreshToken={refreshToken}
      />
    );
  }, [active, api, changeGroups, changeSummary, cwd, refreshToken, setWrapPreview, wrapPreview]);

  const handleTabWheel = useCallback((event) => {
    const el = tabListRef.current;
    if (!el) return;
    const maxScroll = Math.max(0, el.scrollWidth - el.clientWidth);
    if (maxScroll <= 1) return;
    const delta = wheelDeltaForTabs(event, el.clientWidth);
    if (!delta) return;
    const previous = el.scrollLeft;
    const next = Math.min(Math.max(0, previous + delta), maxScroll);
    if (next === previous) return;
    event.preventDefault();
    el.scrollLeft = next;
  }, []);

  useLayoutEffect(() => {
    const el = tabListRef.current;
    if (!el || !active?.key) return;
    const activeTabEl = el.querySelector('.ace-preview-details-tab[aria-selected="true"]');
    if (!activeTabEl) return;
    const next = scrollLeftForVisibleTab({
      scrollLeft: el.scrollLeft,
      clientWidth: el.clientWidth,
      scrollWidth: el.scrollWidth,
      tabOffsetLeft: activeTabEl.offsetLeft,
      tabOffsetWidth: activeTabEl.offsetWidth,
    });
    if (next !== el.scrollLeft) el.scrollLeft = next;
  }, [active?.key, tabs]);

  if (!active || tabs.length === 0) return null;

  return (
    <div className="ace-preview-details-panel" data-maximized={maximized ? 'true' : 'false'}>
      <div className="ace-preview-details-tabs">
        <div className="ace-preview-details-tab-scroll-shell" onWheel={handleTabWheel}>
          <div ref={tabListRef} className="ace-preview-details-tab-list" role="tablist" aria-label="预览标签页">
            {tabs.map((tab) => {
              const selected = active.key === tab.key;
              const label = tabLabel(tab);
              const isFileTab = tab.type === PREVIEW_TAB_TYPES.FILE;
              return (
                <button
                  key={tab.key}
                  type="button"
                  role="tab"
                  aria-selected={selected}
                  className={clsx('ace-preview-details-tab', selected && 'is-active')}
                  title={tab.path || label}
                  onClick={() => onActivateTab?.(tab.key)}
                >
                  {isFileTab && (
                    <FileTypeIcon
                      path={tab.path || label}
                      size={20}
                      className="ace-preview-details-tab-icon"
                    />
                  )}
                  <span className="ace-preview-details-tab-label">{label}</span>
                  <span
                    role="button"
                    tabIndex={-1}
                    className="ace-preview-details-tab-close"
                    title="关闭标签页"
                    aria-label={`关闭 ${label}`}
                    onClick={(event) => {
                      event.preventDefault();
                      event.stopPropagation();
                      onCloseTab?.(tab.key);
                    }}
                  >
                    <VsIcon name="close" size={18} />
                  </span>
                </button>
              );
            })}
          </div>
          <PreviewTabScrollbar scrollRef={tabListRef} />
        </div>
        <div className="ace-preview-details-actions">
          <button
            type="button"
            className="ace-preview-details-action"
            onClick={onToggleMaximize}
            title={maximized ? '还原预览面板' : '最大化预览面板'}
            aria-label={maximized ? '还原预览面板' : '最大化预览面板'}
            aria-pressed={maximized}
          >
            <VsIcon name={maximized ? 'screenNormal' : 'screenFull'} size={14} />
          </button>
          <button
            type="button"
            className="ace-preview-details-action"
            onClick={onCloseAll}
            title="关闭全部预览标签页"
            aria-label="关闭全部预览标签页"
          >
            <VsIcon name="close" size={14} />
          </button>
        </div>
      </div>
      <div className="ace-preview-details-body">
        {renderedBody}
      </div>
    </div>
  );
}
