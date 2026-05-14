import { useEffect, useMemo, useRef, useState } from 'react';
import * as Diff2Html from 'diff2html';

const REVIEW_SIDE_BY_SIDE_MIN_WIDTH = 640;
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { summarizeChangeGroups } from '../lib/sessionChanges.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

function safeGroups(groups) {
  return Array.isArray(groups) ? groups : [];
}

function normalizedSummary(groups, summary) {
  return summary && typeof summary === 'object'
    ? summary
    : summarizeChangeGroups(groups);
}

function ChangeTotals({ summary, compact = false }) {
  if (!summary?.hasChanges) return null;
  return (
    <span className={clsx('ace-change-totals', compact && 'ace-change-totals-compact')}>
      <span>{summary.fileCount} 个文件已更改</span>
      <span className="ace-change-add">+{summary.totalAdditions}</span>
      <span className="ace-change-del">-{summary.totalDeletions}</span>
    </span>
  );
}

function DiffPreview({ group, outputFormat = 'line-by-line', className = '' }) {
  const diffHtml = useMemo(() => {
    if (!group) return '';
    const unified = hunksToUnifiedDiff(group.hunks, group.file);
    if (!unified) return '';
    try {
      return Diff2Html.html(unified, {
        drawFileList: false,
        outputFormat,
        matching: 'lines',
      });
    } catch {
      return '';
    }
  }, [group, outputFormat]);

  if (!diffHtml) {
    return (
      <div className="ace-change-empty-diff">
        此文件没有可渲染的 diff 片段
      </div>
    );
  }

  return (
    <div
      className={clsx('ace-diff', className)}
      dangerouslySetInnerHTML={{ __html: diffHtml }}
    />
  );
}

function ChangeFileButton({ group, open, onClick, selected = false }) {
  return (
    <button
      type="button"
      className={clsx('ace-change-file-row', selected && 'is-selected')}
      onClick={onClick}
      title={group.file}
    >
      <VsIcon name={open ? 'glyphDown' : 'expandRight'} size={9} />
      <span className="ace-change-file-path">{group.file}</span>
      <span className="ace-change-file-counts">
        {group.totalAdditions > 0 && <span className="ace-change-add">+{group.totalAdditions}</span>}
        {group.totalDeletions > 0 && <span className="ace-change-del">-{group.totalDeletions}</span>}
      </span>
    </button>
  );
}

export function ChangeConversationCard({
  groups,
  summary,
  title = '',
  onReview,
  onRevert,
  reverting = false,
  restored = false,
}) {
  const list = safeGroups(groups);
  const changeSummary = normalizedSummary(list, summary);
  const [collapsed, setCollapsed] = useState(false);
  const [expandedFile, setExpandedFile] = useState(list[0]?.file || '');

  useEffect(() => {
    if (!list.length) return;
    setExpandedFile((prev) => list.some((g) => g.file === prev) ? prev : list[0].file);
  }, [list]);

  if (!changeSummary.hasChanges) return null;

  return (
    <div className="ace-change-card" data-change-region="conversation">
      <div className="ace-change-card-header">
        <div className="ace-change-card-title">
          <VsIcon name="edit" size={14} />
          <div className="ace-change-card-title-stack">
            <ChangeTotals summary={changeSummary} />
            {title && <span className="ace-change-card-subtitle">{title}</span>}
          </div>
        </div>
        <div className="ace-change-card-actions">
          {onRevert && (
            <button
              type="button"
              className="ace-change-ghost-btn"
              onClick={onRevert}
              disabled={reverting || restored}
              title={restored ? '本轮文件改动已撤销' : '撤销本轮文件改动'}
            >
              {restored ? '已撤销' : (reverting ? '撤销中' : '撤销')}
            </button>
          )}
          <button
            type="button"
            className="ace-change-ghost-btn"
            onClick={onReview}
            title="在右侧审查面板中查看"
          >
            审查
          </button>
          <button
            type="button"
            className="ace-change-icon-btn"
            onClick={() => setCollapsed((v) => !v)}
            title={collapsed ? '展开变更摘要' : '收起变更摘要'}
            aria-expanded={!collapsed}
          >
            <VsIcon name={collapsed ? 'glyphDown' : 'glyphUp'} size={9} />
          </button>
        </div>
      </div>

      {!collapsed && (
        <div className="ace-change-card-body">
          {list.map((group) => {
            const open = expandedFile === group.file;
            return (
              <div key={group.file} className="ace-change-card-file">
                <ChangeFileButton
                  group={group}
                  open={open}
                  selected={open}
                  onClick={() => setExpandedFile((prev) => prev === group.file ? '' : group.file)}
                />
                {open && (
                  <div className="ace-change-inline-diff">
                    <DiffPreview group={group} outputFormat="line-by-line" className="ace-change-diff-compact" />
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

export function ChangeGlassDock({ summary, onReview, onDismiss, dockRef, scrollRef }) {
  const localDockRef = useRef(null);
  const rootRef = dockRef || localDockRef;
  const backdropRef = useRef(null);

  useEffect(() => {
    const source = scrollRef?.current;
    const root = rootRef.current;
    const backdrop = backdropRef.current;
    const crop = backdrop?.querySelector('.ace-change-glass-blur-crop');
    if (!source || !root || !backdrop || !crop) return undefined;

    let raf = 0;
    const dock = root.querySelector('.ace-change-glass-dock');

    const positionClone = () => {
      const clone = crop.firstElementChild;
      if (!clone || !dock) return;
      const sourceRect = source.getBoundingClientRect();
      const dockRect = dock.getBoundingClientRect();
      clone.style.left = `${sourceRect.left - dockRect.left}px`;
      clone.style.top = `${sourceRect.top - dockRect.top - source.scrollTop}px`;
      clone.style.width = `${sourceRect.width}px`;
      clone.style.minHeight = `${source.scrollHeight}px`;
    };

    const rebuildClone = () => {
      const clone = source.cloneNode(true);
      clone.removeAttribute('id');
      clone.setAttribute('aria-hidden', 'true');
      clone.classList.add('ace-change-glass-source-clone');
      clone.style.position = 'absolute';
      clone.style.height = 'auto';
      clone.style.maxHeight = 'none';
      clone.style.overflow = 'visible';
      clone.style.pointerEvents = 'none';
      clone.style.margin = '0';
      clone.scrollTop = 0;
      crop.replaceChildren(clone);
      positionClone();
    };

    const scheduleRebuild = () => {
      if (raf) cancelAnimationFrame(raf);
      raf = requestAnimationFrame(() => {
        raf = 0;
        rebuildClone();
      });
    };

    const syncPosition = () => {
      if (raf) return;
      raf = requestAnimationFrame(() => {
        raf = 0;
        positionClone();
      });
    };

    rebuildClone();
    source.addEventListener('scroll', syncPosition, { passive: true });
    window.addEventListener('resize', syncPosition);

    const mutationObserver = typeof MutationObserver !== 'undefined'
      ? new MutationObserver(scheduleRebuild)
      : null;
    mutationObserver?.observe(source, {
      childList: true,
      subtree: true,
      characterData: true,
      attributes: true,
      attributeFilter: ['class', 'style'],
    });

    const resizeObserver = typeof ResizeObserver !== 'undefined'
      ? new ResizeObserver(syncPosition)
      : null;
    resizeObserver?.observe(source);
    if (dock) resizeObserver?.observe(dock);

    return () => {
      if (raf) cancelAnimationFrame(raf);
      source.removeEventListener('scroll', syncPosition);
      window.removeEventListener('resize', syncPosition);
      mutationObserver?.disconnect();
      resizeObserver?.disconnect();
      crop.replaceChildren();
    };
  }, [rootRef, scrollRef, summary?.fileCount, summary?.totalAdditions, summary?.totalDeletions]);

  if (!summary?.hasChanges) return null;
  return (
    <div ref={rootRef} className="ace-change-glass-wrap" data-change-region="composer">
      <div className="ace-change-glass-dock">
        <div ref={backdropRef} className="ace-change-glass-blur-backdrop" aria-hidden="true">
          <div className="ace-change-glass-blur-crop" />
        </div>
        <button
          type="button"
          className="ace-change-glass-main"
          onClick={(event) => {
            event.stopPropagation();
            onReview?.();
          }}
          title="打开右侧审查面板"
        >
          <ChangeTotals summary={summary} compact />
          <span className="ace-change-glass-action">查看更改</span>
        </button>
        {onDismiss && (
          <button
            type="button"
            className="ace-change-glass-close"
            onClick={(event) => {
              event.stopPropagation();
              onDismiss();
            }}
            title="关闭变更摘要"
            aria-label="关闭变更摘要"
          >
            <VsIcon name="close" size={12} />
          </button>
        )}
      </div>
    </div>
  );
}

export function ChangeReviewPanel({ groups, summary }) {
  const list = safeGroups(groups);
  const changeSummary = normalizedSummary(list, summary);
  const [openFiles, setOpenFiles] = useState(() => new Set(list[0]?.file ? [list[0].file] : []));
  const panelRef = useRef(null);
  // 面板太窄时双栏 diff 列宽被挤到 ~150px,文本断行严重不可读;低于阈值切回 line-by-line。
  // 默认 line-by-line 是因为 ResizeObserver 第一帧前没数据,先保守渲染避免初始闪烁。
  const [outputFormat, setOutputFormat] = useState('line-by-line');

  useEffect(() => {
    if (!list.length) {
      setOpenFiles(new Set());
      return;
    }
    setOpenFiles((prev) => {
      const valid = new Set(list.map((g) => g.file));
      const next = new Set([...prev].filter((file) => valid.has(file)));
      if (next.size === 0) next.add(list[0].file);
      return next;
    });
  }, [list]);

  useEffect(() => {
    const el = panelRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width;
        setOutputFormat(w >= REVIEW_SIDE_BY_SIDE_MIN_WIDTH ? 'side-by-side' : 'line-by-line');
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  if (!changeSummary.hasChanges) {
    return (
      <div className="ace-empty-state">
        <div>本会话暂无文件变更</div>
        <div className="text-[10px] opacity-70">仅显示 file_edit / file_write 工具的改动</div>
      </div>
    );
  }

  const toggleFile = (file) => {
    setOpenFiles((prev) => {
      const next = new Set(prev);
      if (next.has(file)) next.delete(file);
      else next.add(file);
      return next;
    });
  };

  return (
    <div className="ace-review-panel" data-change-region="side-panel" ref={panelRef}>
      <div className="ace-review-summary">
        <div className="ace-review-title">
          <VsIcon name="editWindow" size={15} />
          <span>审查</span>
        </div>
        <ChangeTotals summary={changeSummary} />
      </div>
      <div className="ace-review-file-list">
        {list.map((group) => {
          const open = openFiles.has(group.file);
          return (
            <section key={group.file} className="ace-review-file">
              <ChangeFileButton
                group={group}
                open={open}
                selected={open}
                onClick={() => toggleFile(group.file)}
              />
              {open && (
                <div className="ace-review-diff-scroll">
                  <DiffPreview group={group} outputFormat={outputFormat} className="ace-review-diff" />
                </div>
              )}
            </section>
          );
        })}
      </div>
    </div>
  );
}
