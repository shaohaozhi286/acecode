import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import * as Diff2Html from 'diff2html';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  joinWorkspacePath,
} from '../lib/desktopContextMenu.js';
import { summarizeChangeGroups } from '../lib/sessionChanges.js';
import { reconcileOpenFiles, restoredScrollTop } from '../lib/changeReviewStability.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { todoChecklistPresentation } from '../lib/todoChecklist.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const REVIEW_SIDE_BY_SIDE_MIN_WIDTH = 640;

function safeGroups(groups) {
  return Array.isArray(groups) ? groups : [];
}

function normalizedSummary(groups, summary) {
  return summary && typeof summary === 'object'
    ? summary
    : summarizeChangeGroups(groups);
}

function matchingGroupFile(groups, file) {
  const target = normalizeTreePath(file);
  if (!target) return '';
  const group = safeGroups(groups).find((item) => normalizeTreePath(item?.file) === target);
  return group?.file || '';
}

async function copyDiffText(text, okText) {
  if (!text) {
    toast({ kind: 'info', text: '没有可复制的 diff' });
    return;
  }
  const result = await copyTextToSystemClipboard(text);
  if (result.ok) toast({ kind: 'ok', text: okText });
  else toast({ kind: 'err', text: '复制失败:' + (result.error || '') });
}

export function ChangeTotals({ summary, compact = false }) {
  if (!summary?.hasChanges) return null;
  return (
    <span className={clsx('ace-change-totals', compact && 'ace-change-totals-compact')}>
      <span>{summary.fileCount} 个文件已更改</span>
      <span className="ace-change-add">+{summary.totalAdditions}</span>
      <span className="ace-change-del">-{summary.totalDeletions}</span>
    </span>
  );
}

function TodoProgressInline({ checklist }) {
  if (!checklist?.visible) return null;
  const hasActive = checklist.items.some((item) => item.status === 'in_progress');
  const currentStep = Math.min(
    checklist.total,
    Math.max(hasActive ? 1 : 0, checklist.done + (hasActive ? 1 : 0)),
  );
  const label = currentStep > 0
    ? `第 ${currentStep} / ${checklist.total} 步`
    : `0 / ${checklist.total} 步`;
  return (
    <span className="ace-change-glass-todo-progress">
      <span className="ace-change-glass-todo-spinner" aria-hidden="true" />
      <span>{label}</span>
    </span>
  );
}

function TodoChecklistPopover({ checklist, onClear, clearing = false }) {
  if (!checklist?.visible) return null;
  return (
    <div className="ace-change-glass-todo-popover">
      <div className="ace-todo-glass-dock" role="group" aria-label={`待办事项 (${checklist.done}/${checklist.total})`}>
        <div className="ace-todo-glass-content">
          <div className="ace-todo-glass-title">
            待办事项 ({checklist.done}/{checklist.total})
          </div>
          <div className="ace-todo-glass-list">
            {checklist.items.map((item) => {
              return (
                <div
                  key={item.key}
                  className="ace-todo-glass-row"
                  data-todo-status={item.status}
                >
                  <span
                    className={clsx('ace-todo-glass-marker', item.markerClassName)}
                    title={item.markerLabel}
                    aria-label={item.markerLabel}
                  >
                    {item.icon === 'check' && <VsIcon name="ok" size={10} mono={false} />}
                    {item.icon === 'dot' && <span className="h-1.5 w-1.5 rounded-full bg-warn" />}
                    {item.icon === 'dash' && <span className="h-px w-2 bg-fg-mute" />}
                  </span>
                  <span className={clsx('ace-todo-glass-text', item.textClassName)}>
                    {item.content}
                  </span>
                </div>
              );
            })}
          </div>
        </div>
        <button
          type="button"
          className="ace-todo-glass-clear"
          onClick={onClear}
          disabled={!onClear || clearing}
          title="清空待办事项"
          aria-label="清空待办事项"
        >
          <VsIcon name="clearAll" size={16} alt="清空待办事项" />
        </button>
      </div>
    </div>
  );
}

export function DiffPreview({ group, outputFormat = 'line-by-line', className = '' }) {
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

  // 防塌缩:流式重算瞬时得到空结果(hunks 异常 / diff2html 解析失败)时,
  // 若此前渲染过非空 diff 则继续展示上一次内容。空态 div 远矮于 diff 表格,
  // 塌缩会让滚动容器 overflow 消失、scrollTop 被浏览器钳到 0,内容恢复后
  // 用户停在顶部(fix-preview-scroll-during-stream 的直接症状之一)。
  const lastHtmlRef = useRef('');
  if (diffHtml) lastHtmlRef.current = diffHtml;
  const html = diffHtml || lastHtmlRef.current;

  if (!html) {
    return (
      <div className="ace-change-empty-diff">
        此文件没有可渲染的 diff 片段
      </div>
    );
  }

  return (
    <div
      className={clsx('ace-diff', className)}
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
}

function ChangeFileButton({ group, open, onClick, selected = false, cwd = '' }) {
  return (
    <button
      type="button"
      data-desktop-review-kind="file"
      data-desktop-review-file={group.file || undefined}
      data-desktop-review-absolute-path={cwd ? joinWorkspacePath(cwd, group.file) : undefined}
      data-desktop-review-additions={String(group.totalAdditions || 0)}
      data-desktop-review-deletions={String(group.totalDeletions || 0)}
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

function splitPath(path) {
  const parts = String(path || '').split(/[\\/]/).filter(Boolean);
  const name = parts.pop() || String(path || '');
  return {
    name,
    parent: parts.join('/'),
  };
}

export function ChangeCompactList({
  groups,
  summary,
  cwd = '',
  selectedFile = '',
  selectedFileRevision = 0,
  onOpenFile,
}) {
  const list = safeGroups(groups);
  const changeSummary = normalizedSummary(list, summary);
  const listRef = useRef(null);
  const selectedNormalizedFile = normalizeTreePath(selectedFile);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'review') return;
      if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE && target.file) {
        detail.handled = true;
        onOpenFile?.(target.file);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onOpenFile]);

  useLayoutEffect(() => {
    if (!selectedNormalizedFile) return;
    const el = listRef.current;
    if (!el) return;
    const row = Array.from(el.querySelectorAll('[data-change-compact-file]'))
      .find((node) => normalizeTreePath(node.getAttribute('data-change-compact-file')) === selectedNormalizedFile);
    if (!row) return;
    const listRect = el.getBoundingClientRect();
    const rowRect = row.getBoundingClientRect();
    el.scrollTop = Math.max(0, el.scrollTop + rowRect.top - listRect.top);
  }, [selectedNormalizedFile, selectedFileRevision]);

  if (!changeSummary.hasChanges) {
    return (
      <div className="ace-empty-state">
        <div>本会话暂无可审查的结构化文件变更</div>
        <div className="text-[10px] opacity-70">shell / script 等无结构化 hunks 的改动可能不会出现在这里</div>
      </div>
    );
  }

  return (
    <div className="ace-change-compact-panel" data-change-region="side-panel">
      <div
        className="ace-change-compact-summary"
        data-desktop-review-kind="summary"
        data-desktop-review-additions={String(changeSummary.totalAdditions || 0)}
        data-desktop-review-deletions={String(changeSummary.totalDeletions || 0)}
        data-desktop-review-file-count={String(changeSummary.fileCount || 0)}
      >
        <div className="ace-review-title">
          <VsIcon name="editWindow" size={15} />
          <span>变更</span>
        </div>
        <ChangeTotals summary={changeSummary} compact />
      </div>
      <div className="ace-change-compact-list" ref={listRef}>
        {list.map((group) => {
          const pathParts = splitPath(group.file);
          const selected = selectedNormalizedFile
            && normalizeTreePath(group.file) === selectedNormalizedFile;
          return (
            <button
              key={group.file}
              type="button"
              data-change-compact-file={group.file}
              data-desktop-review-kind="file"
              data-desktop-review-file={group.file || undefined}
              data-desktop-review-absolute-path={cwd ? joinWorkspacePath(cwd, group.file) : undefined}
              data-desktop-review-additions={String(group.totalAdditions || 0)}
              data-desktop-review-deletions={String(group.totalDeletions || 0)}
              className={clsx('ace-change-compact-row', selected && 'is-selected')}
              onClick={() => onOpenFile?.(group.file)}
              title={group.file}
            >
              <VsIcon name="file" size={14} mono={false} />
              <span className="ace-change-compact-file">
                <span className="ace-change-compact-name">{pathParts.name}</span>
                {pathParts.parent && <span className="ace-change-compact-parent">{pathParts.parent}</span>}
              </span>
              <span className="ace-change-file-counts">
                {group.totalAdditions > 0 && <span className="ace-change-add">+{group.totalAdditions}</span>}
                {group.totalDeletions > 0 && <span className="ace-change-del">-{group.totalDeletions}</span>}
              </span>
            </button>
          );
        })}
      </div>
    </div>
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

export function ChangeGlassDock({
  summary,
  onReview,
  onDismiss,
  dockRef,
  scrollRef,
  todos = [],
  todoSummary = null,
  onClearTodos,
  todoClearing = false,
}) {
  const localDockRef = useRef(null);
  const rootRef = dockRef || localDockRef;
  const backdropRef = useRef(null);
  const todoChecklist = todoChecklistPresentation(todos, todoSummary);

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
          <span className="ace-change-glass-summary">
            <TodoProgressInline checklist={todoChecklist} />
            <ChangeTotals summary={summary} compact />
          </span>
          <span className="ace-change-glass-action">查看变更</span>
        </button>
        <TodoChecklistPopover
          checklist={todoChecklist}
          onClear={onClearTodos}
          clearing={todoClearing}
        />
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

export function ChangeReviewPanel({
  groups,
  summary,
  cwd = '',
  initialExpandedFile = '',
  title = '审查',
  dataRegion = 'side-panel',
  initialExpandedFileRevision = 0,
  onSelectFile,
}) {
  const list = safeGroups(groups);
  const changeSummary = normalizedSummary(list, summary);
  const initialOpenFile = matchingGroupFile(list, initialExpandedFile)
    || (initialExpandedFile ? '' : (list[0]?.file || ''));
  const [openFiles, setOpenFiles] = useState(() => new Set(
    initialOpenFile ? [initialOpenFile] : [],
  ));
  const [scrollRequest, setScrollRequest] = useState(0);
  const panelRef = useRef(null);
  // 面板太窄时双栏 diff 列宽被挤到 ~150px,文本断行严重不可读;低于阈值切回 line-by-line。
  // 默认 line-by-line 是因为 ResizeObserver 第一帧前没数据,先保守渲染避免初始闪烁。
  const [outputFormat, setOutputFormat] = useState('line-by-line');

  useEffect(() => {
    setOpenFiles((prev) => {
      // 集合内容不变时保留原 Set 身份,setState 经 Object.is
      // 直接跳过 —— 流式期间(groups 引用已被上游按签名稳定)不再每帧多渲染一次。
      const files = list.map((g) => g.file);
      if (!initialExpandedFile) return reconcileOpenFiles(prev, files);
      const previous = prev instanceof Set ? prev : new Set();
      const valid = new Set(files.filter(Boolean));
      const next = new Set([...previous].filter((file) => valid.has(file)));
      if (next.size === previous.size && [...next].every((file) => previous.has(file))) {
        return previous;
      }
      return next;
    });
  }, [initialExpandedFile, list]);

  // 滚动位置兜底:diff 内容替换让浏览器钳制了 scrollTop 时(内容瞬时塌缩
  // 会直接钳到 0),在 paint 与钳制 scroll 事件派发之前恢复到用户位置。
  // 依赖 [list] 是精确的内容键:groups 引用经上游签名稳定化,只在 diff
  // 内容真实变化时换引用;本地状态渲染(折叠切换等)不会触发本效应,
  // 折叠引发的钳制因此被视为用户意图、正常记录。
  const fileListRef = useRef(null);
  const savedScrollTopRef = useRef(0);
  const suppressScrollRecordRef = useRef(false);
  const pendingScrollFileRef = useRef('');
  const syncedExpandedRequestRef = useRef('');

  const requestFileFocus = (file) => {
    if (!file) return;
    pendingScrollFileRef.current = file;
    setOpenFiles((prev) => {
      const current = prev instanceof Set ? prev : new Set();
      if (current.has(file)) return current;
      const next = new Set(current);
      next.add(file);
      return next;
    });
    setScrollRequest((prev) => prev + 1);
  };
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
  }, [list]);

  useLayoutEffect(() => {
    const requestKey = `${initialExpandedFile}\u0000${initialExpandedFileRevision}`;
    if (!initialExpandedFile) {
      syncedExpandedRequestRef.current = '';
      return;
    }
    if (syncedExpandedRequestRef.current === requestKey) return;
    const file = matchingGroupFile(list, initialExpandedFile);
    if (!file) return;
    syncedExpandedRequestRef.current = requestKey;
    requestFileFocus(file);
  }, [initialExpandedFile, initialExpandedFileRevision, list]);

  useLayoutEffect(() => {
    const file = pendingScrollFileRef.current;
    const el = fileListRef.current;
    if (!file || !el || !openFiles.has(file)) return;
    const target = Array.from(el.querySelectorAll('[data-review-file-section]'))
      .find((node) => node.getAttribute('data-review-file-section') === file);
    if (!target) return;
    pendingScrollFileRef.current = '';
    suppressScrollRecordRef.current = true;
    const listRect = el.getBoundingClientRect();
    const targetRect = target.getBoundingClientRect();
    el.scrollTop = Math.max(0, el.scrollTop + targetRect.top - listRect.top);
    savedScrollTopRef.current = el.scrollTop;
    const frame = window.requestAnimationFrame(() => {
      suppressScrollRecordRef.current = false;
    });
    return () => {
      window.cancelAnimationFrame(frame);
      suppressScrollRecordRef.current = false;
    };
  }, [openFiles, scrollRequest]);

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

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'review') return;

      if (action === DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF) {
        detail.handled = true;
        const group = list.find((item) => item.file === target.file);
        copyDiffText(group ? hunksToUnifiedDiff(group.hunks, group.file) : '', '已复制文件 diff');
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS) {
        detail.handled = true;
        const text = list
          .map((group) => hunksToUnifiedDiff(group.hunks, group.file))
          .filter(Boolean)
          .join('\n\n');
        copyDiffText(text, '已复制全部 diff');
      } else if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS) {
        detail.handled = true;
        setOpenFiles(new Set(list.map((group) => group.file).filter(Boolean)));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS) {
        detail.handled = true;
        setOpenFiles(new Set());
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [list]);

  if (!changeSummary.hasChanges) {
    return (
      <div className="ace-empty-state">
        <div>本会话暂无文件变更</div>
        <div className="text-[10px] opacity-70">仅显示 file_edit / file_write 工具的改动</div>
      </div>
    );
  }

  const toggleFile = (file, currentlyOpen) => {
    if (currentlyOpen) {
      setOpenFiles((prev) => {
        const next = new Set(prev instanceof Set ? prev : []);
        next.delete(file);
        return next;
      });
      return;
    }
    onSelectFile?.(file);
    requestFileFocus(file);
  };

  return (
    <div className="ace-review-panel" data-change-region={dataRegion} ref={panelRef}>
      <div
        className="ace-review-summary"
        data-desktop-review-kind="summary"
        data-desktop-review-additions={String(changeSummary.totalAdditions || 0)}
        data-desktop-review-deletions={String(changeSummary.totalDeletions || 0)}
        data-desktop-review-file-count={String(changeSummary.fileCount || 0)}
      >
        <div className="ace-review-title">
          <VsIcon name="editWindow" size={15} />
          <span>{title}</span>
        </div>
        <ChangeTotals summary={changeSummary} />
      </div>
      <div
        className="ace-review-file-list"
        ref={fileListRef}
        onScroll={(event) => {
          // 内容替换那一帧浏览器钳制/恢复产生的 scroll 事件不算用户意图,
          // 不写入 savedScrollTopRef,否则用户位置会被钳制值(0)覆盖,
          // 后续帧就无从恢复了。
          if (suppressScrollRecordRef.current) return;
          savedScrollTopRef.current = event.currentTarget.scrollTop;
        }}
      >
        {list.map((group) => {
          const open = openFiles.has(group.file);
          return (
            <section
              key={group.file}
              className="ace-review-file"
              data-review-file-section={group.file}
            >
              <ChangeFileButton
                group={group}
                open={open}
                selected={open}
                cwd={cwd}
                onClick={() => toggleFile(group.file, open)}
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

export function SessionChangeDetails({
  groups,
  summary,
  cwd = '',
  expandedFile = '',
  expandedFileRevision = 0,
  onSelectFile,
}) {
  return (
    <ChangeReviewPanel
      groups={groups}
      summary={summary}
      cwd={cwd}
      initialExpandedFile={expandedFile}
      initialExpandedFileRevision={expandedFileRevision}
      onSelectFile={onSelectFile}
      title="会话变更"
      dataRegion="preview-panel"
    />
  );
}
