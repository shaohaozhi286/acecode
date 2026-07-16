import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import * as Diff2Html from 'diff2html';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  joinWorkspacePath,
} from '../lib/desktopContextMenu.js';
import { restoredScrollTop } from '../lib/changeReviewStability.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const REVIEW_SIDE_BY_SIDE_MIN_WIDTH = 640;

function safeRows(rows) {
  return Array.isArray(rows) ? rows : [];
}

function matchingRowPath(rows, path) {
  const target = normalizeTreePath(path);
  if (!target) return '';
  return safeRows(rows).find((row) => normalizeTreePath(row?.path) === target)?.path || '';
}

function sameSet(left, right) {
  if (left.size !== right.size) return false;
  return [...left].every((value) => right.has(value));
}

function splitPath(path) {
  const normalized = String(path || '').replace(/\\/g, '/');
  const index = normalized.lastIndexOf('/');
  if (index < 0) return { name: normalized, parent: '' };
  return {
    name: normalized.slice(index + 1),
    parent: normalized.slice(0, index),
  };
}

function statusBadgeClass(status) {
  switch (status) {
    case 'A': return 'text-ok';
    case 'D': return 'text-danger';
    case 'R': return 'text-warn';
    default: return 'text-fg-mute';
  }
}

function renderPatchHtml(patch, outputFormat) {
  if (!patch) return '';
  try {
    return Diff2Html.html(patch, {
      drawFileList: false,
      outputFormat,
      matching: 'lines',
    });
  } catch {
    return '';
  }
}

function PatchDiff({ text, outputFormat }) {
  const html = useMemo(() => renderPatchHtml(text, outputFormat), [text, outputFormat]);
  if (!html) {
    return <div className="ace-change-empty-diff">此文件没有可渲染的 diff 片段</div>;
  }
  return <div className="ace-diff ace-review-diff" dangerouslySetInnerHTML={{ __html: html }} />;
}

export async function copyDiffText(text, okText) {
  if (!text) {
    toast({ kind: 'info', text: '没有可复制的 diff' });
    return;
  }
  const result = await copyTextToSystemClipboard(text);
  if (result.ok) toast({ kind: 'ok', text: okText });
  else toast({ kind: 'err', text: '复制失败:' + (result.error || '') });
}

function ChangeReviewFileRow({ row, open, selected, cwd, onToggle, onOpenFile }) {
  const pathParts = splitPath(row.path);
  const additions = typeof row.additions === 'number' ? row.additions : Number.NaN;
  const deletions = typeof row.deletions === 'number' ? row.deletions : Number.NaN;
  const hasNumericStats = !row.binary && Number.isFinite(additions) && Number.isFinite(deletions);
  const status = row.status || 'M';

  return (
    <button
      type="button"
      data-change-compact-file={row.path}
      data-desktop-review-kind="file"
      data-desktop-review-file={row.path || undefined}
      data-desktop-review-absolute-path={cwd ? joinWorkspacePath(cwd, row.path) : undefined}
      data-desktop-review-additions={Number.isFinite(additions) ? String(additions) : undefined}
      data-desktop-review-deletions={Number.isFinite(deletions) ? String(deletions) : undefined}
      className={clsx('ace-change-file-row', selected && 'is-selected')}
      onClick={onToggle}
      title={row.path}
    >
      <VsIcon name={open ? 'glyphDown' : 'expandRight'} size={9} className="shrink-0" />
      <span className={clsx('text-[10px] font-bold w-3 shrink-0 text-center', statusBadgeClass(status))}>
        {status}
      </span>
      <span className="ace-change-compact-file flex-1 min-w-0">
        <span className="ace-change-compact-name">{pathParts.name}</span>
        {pathParts.parent && <span className="ace-change-compact-parent">{pathParts.parent}</span>}
      </span>
      <span className="ace-change-file-counts">
        {hasNumericStats ? (
          <>
            <span className="ace-change-add">+{additions}</span>
            <span className="ace-change-del">-{deletions}</span>
          </>
        ) : (
          <span className="text-fg-mute">{row.statLabel || ''}</span>
        )}
      </span>
      {!!onOpenFile && (
        <span
          role="button"
          tabIndex={-1}
          className={clsx('ace-change-row-action', status === 'D' && 'is-hidden')}
          title="转到文件"
          aria-label={`转到文件 ${row.path}`}
          onPointerDown={(event) => event.stopPropagation()}
          onMouseDown={(event) => event.stopPropagation()}
          onClick={(event) => {
            event.preventDefault();
            event.stopPropagation();
            if (status !== 'D') onOpenFile(row.path);
          }}
        >
          <VsIcon name="openFile" size={13} />
        </span>
      )}
    </button>
  );
}

export function ChangeReviewDetails({
  rows = [],
  ready = true,
  loading = false,
  summaryLabel = '',
  fileCount = 0,
  totalAdditions = 0,
  totalDeletions = 0,
  cwd = '',
  dataRegion = 'preview-panel',
  initialExpandedFile = '',
  initialExpandedFileRevision = 0,
  initialOpenFirst = false,
  selectedFile = '',
  errorMessage = '',
  emptyMessage = '暂无文件变更',
  emptyDetail = '',
  onRetryList,
  onSelectFile,
  onOpenFile,
  onEnsureDiff,
  getFileDiffText,
  getAllDiffText,
  onRefresh,
  contentRevision = null,
  ensureDiffRevision = null,
}) {
  const list = safeRows(rows);
  const pathsKey = list.map((row) => row.path || '').join('\u0000');
  const firstPath = list[0]?.path || '';
  const initialPath = matchingRowPath(list, initialExpandedFile)
    || (!initialExpandedFile && initialOpenFirst ? firstPath : '');
  const [openFiles, setOpenFiles] = useState(() => new Set(initialPath ? [initialPath] : []));
  const [scrollRequest, setScrollRequest] = useState(0);
  const [outputFormat, setOutputFormat] = useState('line-by-line');
  const panelRef = useRef(null);
  const fileListRef = useRef(null);
  const savedScrollTopRef = useRef(0);
  const suppressScrollRecordRef = useRef(false);
  const pendingScrollFileRef = useRef('');
  const syncedExpandedRequestRef = useRef('');

  useEffect(() => {
    const valid = new Set(pathsKey ? pathsKey.split('\u0000').filter(Boolean) : []);
    setOpenFiles((previousValue) => {
      const previous = previousValue instanceof Set ? previousValue : new Set();
      const next = new Set([...previous].filter((path) => valid.has(path)));
      if (!initialExpandedFile && initialOpenFirst && next.size === 0 && firstPath) {
        next.add(firstPath);
      }
      return sameSet(previous, next) ? previous : next;
    });
  }, [firstPath, initialExpandedFile, initialOpenFirst, pathsKey]);

  useEffect(() => {
    if (!onEnsureDiff) return;
    for (const path of openFiles) onEnsureDiff(path, false);
  }, [ensureDiffRevision, onEnsureDiff, openFiles]);

  useEffect(() => {
    const el = panelRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return undefined;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        setOutputFormat(
          entry.contentRect.width >= REVIEW_SIDE_BY_SIDE_MIN_WIDTH
            ? 'side-by-side'
            : 'line-by-line',
        );
      }
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, []);

  useLayoutEffect(() => {
    const el = fileListRef.current;
    if (!el) return undefined;
    suppressScrollRecordRef.current = true;
    const target = restoredScrollTop({
      savedScrollTop: savedScrollTopRef.current,
      currentScrollTop: el.scrollTop,
      scrollHeight: el.scrollHeight,
      clientHeight: el.clientHeight,
    });
    if (target != null) el.scrollTop = target;
    const frame = window.requestAnimationFrame(() => {
      suppressScrollRecordRef.current = false;
    });
    return () => {
      window.cancelAnimationFrame(frame);
      suppressScrollRecordRef.current = false;
    };
  }, [contentRevision]);

  useLayoutEffect(() => {
    const requestKey = `${initialExpandedFile}\u0000${initialExpandedFileRevision}`;
    if (!initialExpandedFile) {
      syncedExpandedRequestRef.current = '';
      return;
    }
    if (syncedExpandedRequestRef.current === requestKey) return;
    const path = matchingRowPath(list, initialExpandedFile);
    if (!path) return;
    syncedExpandedRequestRef.current = requestKey;
    pendingScrollFileRef.current = path;
    setOpenFiles((previousValue) => {
      const previous = previousValue instanceof Set ? previousValue : new Set();
      if (previous.has(path)) return previous;
      const next = new Set(previous);
      next.add(path);
      return next;
    });
    onEnsureDiff?.(path, false);
    setScrollRequest((value) => value + 1);
  }, [initialExpandedFile, initialExpandedFileRevision, list, onEnsureDiff, pathsKey]);

  useLayoutEffect(() => {
    const path = pendingScrollFileRef.current;
    const el = fileListRef.current;
    if (!path || !el || !openFiles.has(path)) return;
    const target = Array.from(el.querySelectorAll('[data-review-file-section]'))
      .find((node) => node.getAttribute('data-review-file-section') === path);
    if (!target) return;
    suppressScrollRecordRef.current = true;
    const listRect = el.getBoundingClientRect();
    const targetRect = target.getBoundingClientRect();
    el.scrollTop = Math.max(0, el.scrollTop + targetRect.top - listRect.top);
    savedScrollTopRef.current = el.scrollTop;
    const row = list.find((item) => item.path === path);
    if (!row?.diff || row.diff.state !== 'loading') pendingScrollFileRef.current = '';
    const frame = window.requestAnimationFrame(() => {
      suppressScrollRecordRef.current = false;
    });
    return () => {
      window.cancelAnimationFrame(frame);
      suppressScrollRecordRef.current = false;
    };
  }, [contentRevision, list, openFiles, scrollRequest]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'review') return;

      if (action === DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS) {
        detail.handled = true;
        onRefresh?.();
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF && getFileDiffText) {
        detail.handled = true;
        Promise.resolve()
          .then(() => getFileDiffText(target.file))
          .then((text) => copyDiffText(text, '已复制文件 diff'))
          .catch(() => copyDiffText('', '已复制文件 diff'));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS && (getAllDiffText || getFileDiffText)) {
        detail.handled = true;
        const request = getAllDiffText
          ? Promise.resolve().then(() => getAllDiffText())
          : Promise.all(list.map((row) => getFileDiffText(row.path)))
            .then((texts) => texts.filter(Boolean).join('\n\n'));
        request
          .then((text) => copyDiffText(text, '已复制全部 diff'))
          .catch(() => copyDiffText('', '已复制全部 diff'));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS) {
        detail.handled = true;
        setOpenFiles(new Set(list.map((row) => row.path).filter(Boolean)));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS) {
        detail.handled = true;
        setOpenFiles(new Set());
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [getAllDiffText, getFileDiffText, list, onRefresh]);

  const toggleFile = (path, currentlyOpen) => {
    if (currentlyOpen) {
      setOpenFiles((previousValue) => {
        const next = new Set(previousValue instanceof Set ? previousValue : []);
        next.delete(path);
        return next;
      });
      return;
    }
    onSelectFile?.(path);
    onEnsureDiff?.(path, false);
    pendingScrollFileRef.current = path;
    setOpenFiles((previousValue) => {
      const next = new Set(previousValue instanceof Set ? previousValue : []);
      next.add(path);
      return next;
    });
    setScrollRequest((value) => value + 1);
  };

  const selectedPath = normalizeTreePath(selectedFile || initialExpandedFile);
  const showSummary = ready || loading;

  return (
    <div
      className="ace-review-panel"
      data-change-region={dataRegion}
      data-desktop-review-kind="summary"
      data-desktop-review-refresh="true"
      ref={panelRef}
    >
      {showSummary && (
        <div
          className="px-3 py-1 text-[11px] text-fg-mute border-b border-border/50 flex items-center gap-2 shrink-0"
          data-desktop-review-kind="summary"
          data-desktop-review-additions={String(totalAdditions || 0)}
          data-desktop-review-deletions={String(totalDeletions || 0)}
          data-desktop-review-file-count={String(fileCount || 0)}
        >
          <span className="truncate">{summaryLabel}</span>
          {loading && <span className="ace-spinner w-3 h-3 shrink-0" />}
          <span className="ml-auto shrink-0 inline-flex items-center gap-1.5">
            {ready && (
              <span>
                <span className="ace-change-add">+{totalAdditions || 0}</span>{' '}
                <span className="ace-change-del">-{totalDeletions || 0}</span>
              </span>
            )}
            <button
              type="button"
              className="ace-review-collapse-all-btn"
              title="折叠全部 diff"
              aria-label="折叠全部 diff"
              onClick={() => setOpenFiles(new Set())}
            >
              <VsIcon name="collapseAll" size={14} />
            </button>
          </span>
        </div>
      )}

      <div
        className="ace-review-file-list"
        ref={fileListRef}
        onScroll={(event) => {
          if (suppressScrollRecordRef.current) return;
          savedScrollTopRef.current = event.currentTarget.scrollTop;
        }}
      >
        {errorMessage && (
          <div className="ace-empty-state text-danger">
            <div>{errorMessage}</div>
            {!!onRetryList && (
              <button
                type="button"
                className="mt-1 px-2 py-1 text-[11px] rounded border border-border hover:bg-surface-hi transition-colors"
                onClick={onRetryList}
              >
                重试
              </button>
            )}
          </div>
        )}
        {!errorMessage && ready && list.length === 0 && (
          <div className="ace-empty-state">
            <div>{emptyMessage}</div>
            {emptyDetail && <div className="text-[10px] opacity-70">{emptyDetail}</div>}
          </div>
        )}
        {!errorMessage && list.map((row) => {
          const open = openFiles.has(row.path);
          const selected = selectedPath
            ? normalizeTreePath(row.path) === selectedPath
            : open;
          return (
            <section
              key={row.path}
              className="ace-review-file"
              data-review-file-section={row.path}
              data-desktop-review-kind="file"
              data-desktop-review-file={row.path || undefined}
            >
              <ChangeReviewFileRow
                row={row}
                open={open}
                selected={selected}
                cwd={cwd}
                onToggle={() => toggleFile(row.path, open)}
                onOpenFile={onOpenFile}
              />
              {open && (
                <div className="ace-review-diff-scroll">
                  {row.diff?.state === 'error' ? (
                    <div className="ace-empty-state text-danger text-[11px]">
                      <button
                        type="button"
                        onClick={() => onEnsureDiff?.(row.path, true)}
                        title="重试"
                      >
                        {row.diff.message || '加载 diff 失败'}
                      </button>
                    </div>
                  ) : row.diff?.state === 'ready' ? (
                    <PatchDiff text={row.diff.text || ''} outputFormat={outputFormat} />
                  ) : (
                    <div className="ace-empty-state text-[11px]">加载 diff 中…</div>
                  )}
                </div>
              )}
            </section>
          );
        })}
      </div>
    </div>
  );
}
