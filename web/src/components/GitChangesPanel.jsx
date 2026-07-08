// SidePanel「变更」tab 的 git 级视图(openspec redesign-sidepanel-git-changes)。
//
// git 仓库会话下整体替换旧的会话级 hunks 聚合(用户拍板"彻底替换"):
//   - 头部:变更 <branch> → <base ▾> · N 个文件已更改 +A -D + 手动刷新
//   - 列表:numstat 文件行(状态徽标 + ±行数);点击行内展开懒加载的 diff
//   - 基线 ▾ 只切换比较对象(查看用,绝不 checkout)
// 性能(design D1/D5):列表一条 numstat;diff 正文点谁拉谁;结果按 (cwd,base)
// 缓存,失效 = 回合结束 / checkout 成功事件 / 手动刷新 / 基线切换;面板隐藏
// 时只标脏不请求。
// 纯状态逻辑在 lib/gitChanges.js(Node 单测),这里做编排与渲染。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import * as Diff2Html from 'diff2html';
import {
  buildBaseCandidates,
  buildChangeRow,
  buildSummaryLabel,
  createChangesCache,
} from '../lib/gitChanges.js';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

// 模块级缓存:SidePanel 卸载/重挂(视图切换)不丢;markStale 按 cwd 收口。
const changesCache = createChangesCache();

function renderPatchHtml(patch) {
  if (!patch) return '';
  try {
    return Diff2Html.html(patch, {
      drawFileList: false,
      outputFormat: 'line-by-line',
      matching: 'lines',
    });
  } catch {
    return '';
  }
}

function statusBadgeClass(status) {
  switch (status) {
    case 'A': return 'text-ok';
    case 'D': return 'text-danger';
    case 'R': return 'text-warn';
    default:  return 'text-fg-mute';
  }
}

export function GitChangesPanel({ api, cwd, gitInfo, busy = false, visible = true }) {
  const { candidates, initial } = useMemo(
    () => buildBaseCandidates(gitInfo),
    [gitInfo],
  );
  const [base, setBase] = useState(initial);
  const [baseOpen, setBaseOpen] = useState(false);
  const [list, setList] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');      // 'timeout' | 其它文案
  const [expandedPath, setExpandedPath] = useState('');
  const [patchState, setPatchState] = useState(null); // {path, html?, error?}
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;
  const baseRef = useRef(base);
  baseRef.current = base;
  const visibleRef = useRef(visible);
  visibleRef.current = visible;

  // cwd / gitInfo 变化时重置基线选择。
  useEffect(() => { setBase(initial); setExpandedPath(''); }, [cwd, initial]);

  const fetchList = useCallback((force = false) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!targetCwd || !targetBase) return;
    if (!force) {
      const cached = changesCache.getList(targetCwd, targetBase);
      if (cached) { setList(cached); setError(''); return; }
    }
    // 重拉期间先展示 stale 旧数据,避免闪空白。
    const staleData = changesCache.getListEvenIfStale(targetCwd, targetBase);
    if (staleData) setList(staleData);
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
    setExpandedPath('');
    setPatchState(null);
    if (visibleRef.current) fetchList(true);
  }, [busy, fetchList]);

  // checkout 成功(GitSessionPill 广播)→ 同上。
  useEffect(() => {
    const handler = (event) => {
      const changedCwd = event?.detail?.cwd || '';
      if (changedCwd && changedCwd !== cwdRef.current) return;
      changesCache.markStale(cwdRef.current);
      setExpandedPath('');
      setPatchState(null);
      if (visibleRef.current) fetchList(true);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [fetchList]);

  const toggleFile = useCallback((path) => {
    if (expandedPath === path) {
      setExpandedPath('');
      setPatchState(null);
      return;
    }
    setExpandedPath(path);
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    const cached = changesCache.getPatch(targetCwd, targetBase, path);
    if (cached != null) {
      setPatchState({ path, html: renderPatchHtml(cached) });
      return;
    }
    setPatchState({ path });
    api.gitFileDiff(targetCwd, path, targetBase)
      .then((r) => {
        const patch = r?.patch || '';
        changesCache.putPatch(targetCwd, targetBase, path, patch);
        setPatchState((prev) => (prev?.path === path
          ? { path, html: renderPatchHtml(patch) }
          : prev));
      })
      .catch((e) => {
        const msg = e?.status === 413 ? 'diff 过大,请在终端查看'
          : e?.status === 504 ? 'git 超时,点击重试'
          : '加载 diff 失败';
        setPatchState((prev) => (prev?.path === path ? { path, error: msg } : prev));
      });
  }, [api, expandedPath]);

  if (!gitInfo?.is_repo) return null;

  const rows = (list?.files || []).map(buildChangeRow);
  const summaryLabel = buildSummaryLabel(list);

  return (
    <div className="ace-change-compact-panel" data-change-region="side-panel-git">
      <div className="ace-change-compact-summary">
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
                      onClick={() => { setBaseOpen(false); setBase(c); setExpandedPath(''); setPatchState(null); }}
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
          <div key={row.path}>
            <button
              type="button"
              data-change-compact-file={row.path}
              className={clsx('ace-change-compact-row', expandedPath === row.path && 'is-selected')}
              onClick={() => toggleFile(row.path)}
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
            </button>
            {expandedPath === row.path && (
              <div className="ace-git-inline-diff">
                {patchState?.path === row.path && patchState.html && (
                  <div className="ace-diff" dangerouslySetInnerHTML={{ __html: patchState.html }} />
                )}
                {patchState?.path === row.path && patchState.error && (
                  <div className="ace-empty-state text-danger text-[11px]">{patchState.error}</div>
                )}
                {patchState?.path === row.path && !patchState.html && !patchState.error && (
                  <div className="ace-empty-state text-[11px]">加载 diff 中…</div>
                )}
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}
