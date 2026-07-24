// SidePanel「变更」tab 的 git 级视图(openspec redesign-sidepanel-git-changes)。
//
// git 仓库会话下整体替换旧的会话级 hunks 聚合(用户拍板"彻底替换"):
//   - 头部:变更 <branch> → <base ▾> · N 个文件已更改 +A -D + 手动刷新
//   - 列表:numstat 文件行(状态徽标 + ±行数);点击 → 在中间预览详情栏开
//     / 聚焦「变更」页签并展开该文件的 diff(GitChangeReview),而不是行内展开。
//     这是复刻会话级变更旧行为(点击进详情栏页签),与 file 预览一致。
//   - 基线 ▾ 只切换比较对象(查看用,绝不 checkout)
// 性能(design D1/D5):列表一条 numstat;diff 正文由详情栏点谁拉谁;结果按
// (cwd,base) 缓存在共享的 changesCache(与详情栏 GitChangeReview 复用同一份),
// 失效 = 回合结束 / checkout 成功事件 / 手动刷新 / 基线切换;面板隐藏时只标脏
// 不请求。纯状态逻辑在 lib/gitChanges.js(Node 单测),这里做编排与渲染。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  buildBaseCandidates,
  buildChangeRow,
  buildSummaryLabel,
} from '../lib/gitChanges.js';
import { changesCache } from '../lib/gitChangesCache.js';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { joinWorkspacePath } from '../lib/desktopContextMenu.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

function statusBadgeClass(status) {
  switch (status) {
    case 'A': return 'text-ok';
    case 'D': return 'text-danger';
    case 'R': return 'text-warn';
    default:  return 'text-fg-mute';
  }
}

export function GitChangesPanel({
  api,
  cwd,
  gitInfo,
  busy = false,
  visible = true,
  selectedFile = '',
  onOpenFile,
  onOpenFilePreview,
  onBaseChange,
}) {
  const { candidates, initial } = useMemo(
    () => buildBaseCandidates(gitInfo),
    [gitInfo],
  );
  const [base, setBase] = useState(initial);
  const [baseOpen, setBaseOpen] = useState(false);
  const [list, setList] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');      // 'timeout' | 其它文案
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;
  const baseRef = useRef(base);
  baseRef.current = base;
  const listRef = useRef(list);
  listRef.current = list;
  const visibleRef = useRef(visible);
  visibleRef.current = visible;
  const selectedNormalized = normalizeTreePath(selectedFile);

  // cwd / gitInfo 变化时重置基线选择。
  useEffect(() => { setBase(initial); }, [cwd, initial]);

  const fetchList = useCallback((force = false) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!targetCwd || !targetBase) return;
    if (!force) {
      const cached = changesCache.getList(targetCwd, targetBase);
      if (cached) { setList(cached); setError(''); return; }
    }
    // 重拉期间先展示同 (cwd,base) 的 stale 旧数据,避免闪空白;该键从未
    // 成功拉过则清空 —— 否则基线切换失败时,上一个基线的列表/汇总行会和
    // 新基线的错误文案混排(「6 个文件已更改」+「加载失败」同屏)。
    const staleData = changesCache.getListEvenIfStale(targetCwd, targetBase);
    setList(staleData || null);
    setLoading(true);
    api.gitChanges(targetCwd, targetBase)
      .then((data) => {
        if (cwdRef.current !== targetCwd || baseRef.current !== targetBase) return;
        changesCache.putList(targetCwd, targetBase, data);
        setList(data);
        setError('');
      })
      .catch((e) => {
        if (cwdRef.current !== targetCwd || baseRef.current !== targetBase) return;
        setError(e?.status === 504 ? 'timeout' : (e?.body?.error || e?.message || 'error'));
      })
      .finally(() => setLoading(false));
  }, [api]);

  // 可见 + (基线 / cwd 变化) → 拉取(缓存命中则零请求)。
  useEffect(() => {
    if (!visible || !cwd || !gitInfo?.is_repo) return;
    fetchList(false);
  }, [visible, cwd, base, gitInfo?.is_repo, fetchList]);

  // 回合结束(busy true→false)→ 标脏;可见则立即重拉。
  const prevBusyRef = useRef(busy);
  useEffect(() => {
    const was = prevBusyRef.current;
    prevBusyRef.current = busy;
    if (!was || busy) return;
    if (!cwdRef.current) return;
    changesCache.markStale(cwdRef.current);
    if (visibleRef.current) fetchList(true);
  }, [busy, fetchList]);

  // checkout 成功(GitSessionPill 广播)→ 同上。
  useEffect(() => {
    const handler = (event) => {
      const changedCwd = event?.detail?.cwd || '';
      if (changedCwd && changedCwd !== cwdRef.current) return;
      changesCache.markStale(cwdRef.current);
      if (visibleRef.current) fetchList(true);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [fetchList]);

  // 点击文件行 → 在中间详情栏打开/聚焦「变更」页签并展开该文件的 diff。
  // 一并带当前基线与文件总数,让页签标签显示「变更(N 文件)」。
  const openFile = useCallback((path) => {
    const l = listRef.current;
    const fileCount = l?.total_count ?? (l?.files?.length ?? 0);
    onOpenFile?.(path, baseRef.current, fileCount);
  }, [onOpenFile]);

  const selectBase = useCallback((next) => {
    setBaseOpen(false);
    setBase(next);
    onBaseChange?.(next);
  }, [onBaseChange]);

  if (!gitInfo?.is_repo) return null;

  const rows = (list?.files || []).map(buildChangeRow);
  const summaryLabel = buildSummaryLabel(list);

  return (
    <div className="ace-change-compact-panel" data-change-region="side-panel-git">
      <div className="ace-change-compact-summary" data-desktop-review-kind="summary">
        <div className="ace-review-title min-w-0">
          <VsIcon name="editWindow" size={15} />
          <span>变更</span>
          <span className="text-fg-mute font-normal truncate max-w-[72px]" title={list?.branch || gitInfo.branch}>
            {list?.branch || gitInfo.branch}
          </span>
          <span className="text-fg-mute opacity-60" aria-hidden="true">→</span>
          <div className="relative min-w-0">
            <button
              type="button"
              className="inline-flex items-center gap-0.5 text-fg-mute hover:text-fg transition-colors min-w-0"
              onClick={() => setBaseOpen(!baseOpen)}
              title={`比较基线:${base}(仅切换查看对象,不会 checkout)`}
            >
              <span className="truncate max-w-[110px]">{base}</span>
              <VsIcon name="expandDown" size={11} className="opacity-60 shrink-0" />
            </button>
            {baseOpen && (
              <>
                <div className="fixed inset-0 z-40" onClick={() => setBaseOpen(false)} />
                <div className="absolute top-full left-0 mt-1 min-w-[160px] bg-surface border border-border ace-shadow rounded-lg z-50 py-1">
                  {candidates.map((c) => (
                    <button
                      key={c}
                      type="button"
                      className={clsx(
                        'w-full text-left px-2.5 py-1 text-[12px] transition-colors',
                        c === base ? 'bg-accent/10 text-accent font-medium' : 'text-fg hover:bg-surface-hi',
                      )}
                      onClick={() => selectBase(c)}
                    >
                      {c}
                    </button>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>
        <div className="flex items-center gap-1.5 shrink-0">
          {loading && <span className="ace-spinner w-3 h-3" />}
          <button
            type="button"
            className="ace-side-panel-refresh-btn !static"
            title="刷新变更列表"
            aria-label="刷新变更列表"
            onClick={() => { changesCache.markStale(cwdRef.current); fetchList(true); }}
          >
            <VsIcon name="refresh" size={14} />
          </button>
        </div>
      </div>

      {list && (
        <div className="px-2.5 py-1 text-[11px] text-fg-mute flex items-center gap-2 border-b border-border/50">
          <span className="truncate">{summaryLabel}</span>
          <span className="ml-auto shrink-0">
            <span className="ace-change-add">+{list.total_additions ?? 0}</span>{' '}
            <span className="ace-change-del">-{list.total_deletions ?? 0}</span>
          </span>
        </div>
      )}

      <div className="ace-change-compact-list">
        {error === 'timeout' && (
          <div className="ace-empty-state">
            <div>git 响应超时(仓库可能很大)</div>
            <button
              type="button"
              className="mt-1 px-2 py-1 text-[11px] rounded border border-border hover:bg-surface-hi transition-colors"
              onClick={() => fetchList(true)}
            >
              重试
            </button>
          </div>
        )}
        {error && error !== 'timeout' && (
          <div className="ace-empty-state text-danger">加载失败:{error}</div>
        )}
        {!error && list && rows.length === 0 && (
          <div className="ace-empty-state">工作区相对 {base} 无变更</div>
        )}
        {!error && rows.map((row) => (
          <button
            key={row.path}
            type="button"
            data-change-compact-file={row.path}
            data-desktop-review-kind="file"
            data-desktop-review-file={row.path || undefined}
            data-desktop-review-absolute-path={cwd ? joinWorkspacePath(cwd, row.path) : undefined}
            data-desktop-review-can-reveal={row.status === 'D' ? 'false' : 'true'}
            className={clsx(
              'ace-change-compact-row',
              selectedNormalized && normalizeTreePath(row.path) === selectedNormalized && 'is-selected',
            )}
            onClick={() => openFile(row.path)}
            title={row.path}
          >
            <span className={clsx('text-[10px] font-bold w-3 shrink-0', statusBadgeClass(row.status))}>
              {row.status}
            </span>
            <span className="ace-change-compact-file">
              <span className="ace-change-compact-name">{row.path.split(/[\\/]/).pop()}</span>
              {row.path.includes('/') && (
                <span className="ace-change-compact-parent">
                  {row.path.slice(0, row.path.lastIndexOf('/'))}
                </span>
              )}
            </span>
            <span className="ace-change-file-counts">
              {row.statLabel.startsWith('+')
                ? (
                  <>
                    <span className="ace-change-add">{row.statLabel.split(' ')[0]}</span>
                    <span className="ace-change-del">{row.statLabel.split(' ')[1]}</span>
                  </>
                )
                : <span className="text-fg-mute">{row.statLabel}</span>}
            </span>
            {!!onOpenFilePreview && (
              // 已删除文件磁盘上不存在,按钮隐形占位保持各行 ± 计数对齐。
              <span
                role="button"
                tabIndex={-1}
                className={clsx('ace-change-row-action', row.status === 'D' && 'is-hidden')}
                title="转到文件"
                aria-label={`转到文件 ${row.path}`}
                onPointerDown={(event) => event.stopPropagation()}
                onMouseDown={(event) => event.stopPropagation()}
                onClick={(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  if (row.status !== 'D') onOpenFilePreview(row.path);
                }}
              >
                <VsIcon name="openFile" size={13} />
              </span>
            )}
          </button>
        ))}
      </div>
    </div>
  );
}
