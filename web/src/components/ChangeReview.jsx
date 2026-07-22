import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import * as Diff2Html from 'diff2html';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { joinWorkspacePath } from '../lib/desktopContextMenu.js';
import { summarizeChangeGroups } from '../lib/sessionChanges.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { todoChecklistPresentation } from '../lib/todoChecklist.js';
import { clsx, formatCount } from '../lib/format.js';
import { ChangeReviewDetails } from './ChangeReviewDetails.jsx';
import { VsIcon } from './Icon.jsx';

function safeGroups(groups) {
  return Array.isArray(groups) ? groups : [];
}

function normalizedSummary(groups, summary) {
  return summary && typeof summary === 'object'
    ? summary
    : summarizeChangeGroups(groups);
}

export function ChangeTotals({ summary, compact = false }) {
  if (!summary?.hasChanges) return null;
  return (
    <span className={clsx('ace-change-totals', compact && 'ace-change-totals-compact')}>
      <span>{formatCount(summary.fileCount, 'filesChanged')}</span>
      <span className="ace-change-add">+{summary.totalAdditions}</span>
      <span className="ace-change-del">-{summary.totalDeletions}</span>
    </span>
  );
}

function TodoProgressInline({ checklist }) {
  if (!checklist?.visible) return null;
  const currentStep = checklist.currentStep ?? 0;
  const progressRatio = Math.max(0, Math.min(1, Number(checklist.progressRatio) || 0));
  const progressDegrees = `${Math.round(progressRatio * 360)}deg`;
  const label = currentStep > 0
    ? `第 ${currentStep} / ${checklist.total} 步`
    : `0 / ${checklist.total} 步`;
  return (
    <span className="ace-change-glass-todo-progress">
      <span
        className="ace-change-glass-todo-ring"
        style={{ '--ace-todo-progress-deg': progressDegrees }}
        aria-hidden="true"
      />
      <span>{label}</span>
    </span>
  );
}

function TodoChecklistPopover({ checklist, anchorRef, open, onPointerEnter, onPointerLeave }) {
  const [position, setPosition] = useState(null);

  useLayoutEffect(() => {
    if (!open || !checklist?.visible || typeof window === 'undefined') {
      setPosition(null);
      return undefined;
    }

    const updatePosition = () => {
      const anchor = anchorRef?.current;
      if (!anchor) return;
      const rect = anchor.getBoundingClientRect();
      const viewportWidth = window.innerWidth || 0;
      const viewportHeight = window.innerHeight || 0;
      const width = Math.min(700, Math.max(280, viewportWidth - 48));
      const center = rect.left + (rect.width / 2);
      const left = Math.round(Math.min(
        viewportWidth - 24 - (width / 2),
        Math.max(24 + (width / 2), center),
      ));
      setPosition({
        left,
        bottom: Math.max(8, Math.round(viewportHeight - rect.top + 5)),
        width: Math.round(width),
        maxHeight: `${Math.max(96, Math.round(rect.top - 16))}px`,
      });
    };

    updatePosition();
    window.addEventListener('resize', updatePosition);
    document.addEventListener('scroll', updatePosition, true);
    return () => {
      window.removeEventListener('resize', updatePosition);
      document.removeEventListener('scroll', updatePosition, true);
    };
  }, [anchorRef, checklist?.total, checklist?.done, checklist?.items?.length, checklist?.visible, open]);

  if (!checklist?.visible || !open || !position || typeof document === 'undefined') return null;
  return createPortal(
    <div
      className="ace-change-glass-todo-popover is-open"
      onPointerEnter={onPointerEnter}
      onPointerLeave={onPointerLeave}
      style={{
        left: position.left,
        bottom: position.bottom,
        width: position.width,
        '--ace-change-todo-popover-max-height': position.maxHeight,
      }}
    >
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
      </div>
    </div>,
    document.body,
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
  showChanges = true,
  onReview,
  onDismiss,
  dockRef,
  todos = [],
  todoSummary = null,
}) {
  const localDockRef = useRef(null);
  const rootRef = dockRef || localDockRef;
  const glassDockRef = useRef(null);
  const todoPopoverCloseTimerRef = useRef(null);
  const [todoPopoverOpen, setTodoPopoverOpen] = useState(false);
  const todoChecklist = todoChecklistPresentation(todos, todoSummary);
  const hasVisibleChanges = !!showChanges && !!summary?.hasChanges;

  const clearTodoPopoverCloseTimer = () => {
    if (!todoPopoverCloseTimerRef.current) return;
    window.clearTimeout(todoPopoverCloseTimerRef.current);
    todoPopoverCloseTimerRef.current = null;
  };

  const openTodoPopover = () => {
    if (!todoChecklist.visible) return;
    clearTodoPopoverCloseTimer();
    setTodoPopoverOpen(true);
  };

  const scheduleTodoPopoverClose = () => {
    clearTodoPopoverCloseTimer();
    todoPopoverCloseTimerRef.current = window.setTimeout(() => {
      todoPopoverCloseTimerRef.current = null;
      setTodoPopoverOpen(false);
    }, 140);
  };

  useEffect(() => {
    if (todoChecklist.visible) return undefined;
    setTodoPopoverOpen(false);
    return undefined;
  }, [todoChecklist.visible]);

  useEffect(() => {
    return () => {
      if (todoPopoverCloseTimerRef.current) {
        window.clearTimeout(todoPopoverCloseTimerRef.current);
        todoPopoverCloseTimerRef.current = null;
      }
    };
  }, []);

  if (!hasVisibleChanges && !todoChecklist.visible) return null;
  const summaryContent = (
    <span className="ace-change-glass-summary">
      <TodoProgressInline checklist={todoChecklist} />
      {hasVisibleChanges && <ChangeTotals summary={summary} compact />}
    </span>
  );
  return (
    <div ref={rootRef} className="ace-change-glass-wrap" data-change-region="composer">
      <div
        ref={glassDockRef}
        className="ace-change-glass-dock"
        onPointerEnter={openTodoPopover}
        onPointerLeave={scheduleTodoPopoverClose}
      >
        {hasVisibleChanges ? (
          <button
            type="button"
            className="ace-change-glass-main"
            onClick={(event) => {
              event.stopPropagation();
              onReview?.();
            }}
            title="打开右侧审查面板"
          >
            {summaryContent}
            <span className="ace-change-glass-action">查看变更</span>
          </button>
        ) : (
          <div className="ace-change-glass-main ace-change-glass-main-static" role="status">
            {summaryContent}
          </div>
        )}
        <TodoChecklistPopover
          checklist={todoChecklist}
          anchorRef={glassDockRef}
          open={todoPopoverOpen}
          onPointerEnter={openTodoPopover}
          onPointerLeave={scheduleTodoPopoverClose}
        />
        {hasVisibleChanges && onDismiss && (
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
  dataRegion = 'side-panel',
  initialExpandedFileRevision = 0,
  reloadRevision = 0,
  onSelectFile,
  onOpenFile,
  onRefresh,
}) {
  const list = safeGroups(groups);
  const changeSummary = normalizedSummary(list, summary);
  const rows = useMemo(() => list.map((group) => {
    const additions = Number(group.totalAdditions || 0);
    const deletions = Number(group.totalDeletions || 0);
    return {
      path: group.file,
      status: group.status || 'M',
      additions,
      deletions,
      statLabel: `+${additions} -${deletions}`,
      diff: {
        state: 'ready',
        text: hunksToUnifiedDiff(group.hunks, group.file),
      },
    };
  }), [list]);
  const diffByPath = useMemo(
    () => new Map(rows.map((row) => [normalizeTreePath(row.path), row.diff.text])),
    [rows],
  );
  const getFileDiffText = useCallback(
    (path) => diffByPath.get(normalizeTreePath(path)) || '',
    [diffByPath],
  );
  const getAllDiffText = useCallback(
    () => rows.map((row) => row.diff.text).filter(Boolean).join('\n\n'),
    [rows],
  );
  const contentRevision = useMemo(
    () => ({ list, reloadRevision }),
    [list, reloadRevision],
  );

  return (
    <ChangeReviewDetails
      rows={rows}
      ready
      summaryLabel={formatCount(changeSummary.fileCount || 0, 'filesChanged')}
      fileCount={changeSummary.fileCount || 0}
      totalAdditions={changeSummary.totalAdditions || 0}
      totalDeletions={changeSummary.totalDeletions || 0}
      cwd={cwd}
      dataRegion={dataRegion}
      initialExpandedFile={initialExpandedFile}
      initialExpandedFileRevision={initialExpandedFileRevision}
      initialOpenFirst
      selectedFile={initialExpandedFile}
      emptyMessage="本会话暂无文件变更"
      emptyDetail="仅显示 file_edit / file_write 工具的改动"
      onSelectFile={onSelectFile}
      onOpenFile={onOpenFile}
      getFileDiffText={getFileDiffText}
      getAllDiffText={getAllDiffText}
      onRefresh={onRefresh}
      contentRevision={contentRevision}
    />
  );
}

export function SessionChangeDetails({
  groups,
  summary,
  cwd = '',
  expandedFile = '',
  expandedFileRevision = 0,
  reloadRevision = 0,
  onSelectFile,
  onOpenFilePreview,
  onRefresh,
}) {
  return (
    <ChangeReviewPanel
      groups={groups}
      summary={summary}
      cwd={cwd}
      initialExpandedFile={expandedFile}
      initialExpandedFileRevision={expandedFileRevision}
      reloadRevision={reloadRevision}
      onSelectFile={onSelectFile}
      onOpenFile={onOpenFilePreview}
      onRefresh={onRefresh}
      dataRegion="preview-panel"
    />
  );
}
