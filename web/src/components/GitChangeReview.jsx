// 中间预览详情栏里的 git 级「变更」页签内容(git-changes 类型页签)。
//
// 与 SessionChangeDetails(会话级 hunks 审查)是同一交互模型的 git 版:
// 顶部汇总 + 可滚动文件列表,每个文件点开懒加载 diff。数据来自 git 后端
// (numstat + 单文件 patch),经共享缓存 `changesCache` 与 SidePanel 的
// GitChangesPanel 复用同一份,失效同步。
//
// 为什么单独一个组件而不是塞进 ChangeReviewPanel:会话审查的 diff 全在内存
// hunks 里同步可得,git diff 是按文件懒加载的异步 patch,状态机不同,强行合并
// 反而更绕。视觉上复用同一套 ace-review-* class 保持一致。

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import * as Diff2Html from 'diff2html';
import { buildChangeRow, buildSummaryLabel } from '../lib/gitChanges.js';
import { changesCache } from '../lib/gitChangesCache.js';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { copyDiffText } from './ChangeReview.jsx';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

// 与 ChangeReview.jsx 的 REVIEW_SIDE_BY_SIDE_MIN_WIDTH 同值:面板太窄时双栏
// diff 列宽被挤到 ~150px 断行不可读,低于阈值退回 line-by-line。
const SIDE_BY_SIDE_MIN_WIDTH = 640;

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

// state 里存 patch 原文而不是渲染好的 html,拉宽/缩窄窗口切换 outputFormat
// 时已展开的 diff 能就地重渲染(会话级 ChangeReview 的双栏行为在 git 版丢过
// 一次 —— 旧实现把 line-by-line 的 html 在加载时写死进 state)。
function GitPatchDiff({ text, outputFormat }) {
  const html = useMemo(() => renderPatchHtml(text, outputFormat), [text, outputFormat]);
  if (!html) return <div className="ace-change-empty-diff">此文件没有可渲染的 diff 片段</div>;
  return <div className="ace-diff ace-review-diff" dangerouslySetInnerHTML={{ __html: html }} />;
}

function statusBadgeClass(status) {
  switch (status) {
    case 'A': return 'text-ok';
    case 'D': return 'text-danger';
    case 'R': return 'text-warn';
    default:  return 'text-fg-mute';
  }
}

function GitChangeFileRow({ row, open, selected, onToggle }) {
  const name = row.path.split(/[\\/]/).pop();
  const parent = row.path.includes('/') ? row.path.slice(0, row.path.lastIndexOf('/')) : '';
  return (
    <button
      type="button"
      data-change-compact-file={row.path}
      data-desktop-review-kind="file"
      data-desktop-review-file={row.path || undefined}
      className={clsx('ace-change-file-row', selected && 'is-selected')}
      onClick={onToggle}
      title={row.path}
    >
      <VsIcon name={open ? 'glyphDown' : 'expandRight'} size={9} className="shrink-0" />
      <span className={clsx('text-[10px] font-bold w-3 shrink-0 text-center', statusBadgeClass(row.status))}>
        {row.status}
      </span>
      <span className="ace-change-compact-file flex-1 min-w-0">
        <span className="ace-change-compact-name">{name}</span>
        {parent && <span className="ace-change-compact-parent">{parent}</span>}
      </span>
      <span className="ace-change-file-counts">
        {row.statLabel.startsWith('+') ? (
          <>
            <span className="ace-change-add">{row.statLabel.split(' ')[0]}</span>
            <span className="ace-change-del">{row.statLabel.split(' ')[1]}</span>
          </>
        ) : (
          <span className="text-fg-mute">{row.statLabel}</span>
        )}
      </span>
    </button>
  );
}

export function GitChangeDetails({
  api,
  cwd = '',
  base = '',
  expandedFile = '',
  expandedFileRevision = 0,
  busy = false,
  onSelectFile,
}) {
  const [list, setList] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');       // 'timeout' | 其它文案
  const [openFiles, setOpenFiles] = useState(() => new Set());
  const [patches, setPatches] = useState({});    // path → {text?|error?}(缺省=加载中)
  // 默认 line-by-line:ResizeObserver 第一帧前没宽度数据,先保守渲染避免闪烁。
  const [outputFormat, setOutputFormat] = useState('line-by-line');
  const panelRef = useRef(null);
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;
  const baseRef = useRef(base);
  baseRef.current = base;
  const fileListRef = useRef(null);
  const pendingScrollFileRef = useRef('');

  const fetchList = useCallback((force = false) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!targetCwd || !targetBase) return;
    if (!force) {
      const cached = changesCache.getList(targetCwd, targetBase);
      if (cached) { setList(cached); setError(''); return; }
    }
    const stale = changesCache.getListEvenIfStale(targetCwd, targetBase);
    if (stale) setList(stale);
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

  // cwd / base 变(基线切换 = base 变)→ 重拉;已展开文件与 patch 清空重来。
  useEffect(() => {
    setOpenFiles(new Set());
    setPatches({});
    fetchList(false);
  }, [cwd, base, fetchList]);

  // 回合结束(busy true→false)→ 标脏重拉,diff 跟着更新。
  const prevBusyRef = useRef(busy);
  useEffect(() => {
    const was = prevBusyRef.current;
    prevBusyRef.current = busy;
    if (!was || busy) return;
    if (!cwdRef.current) return;
    changesCache.markStale(cwdRef.current);
    setPatches({});
    fetchList(true);
  }, [busy, fetchList]);

  // checkout 成功(GitSessionPill 广播)→ 同上。
  useEffect(() => {
    const handler = (event) => {
      const changedCwd = event?.detail?.cwd || '';
      if (changedCwd && changedCwd !== cwdRef.current) return;
      changesCache.markStale(cwdRef.current);
      setPatches({});
      fetchList(true);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [fetchList]);

  // 面板宽度驱动 diff 单/双栏(与会话级 ChangeReview 同阈值同行为)。
  useEffect(() => {
    const el = panelRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width;
        setOutputFormat(w >= SIDE_BY_SIDE_MIN_WIDTH ? 'side-by-side' : 'line-by-line');
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const loadPatch = useCallback((path) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    const cached = changesCache.getPatch(targetCwd, targetBase, path);
    if (cached != null) {
      setPatches((prev) => ({ ...prev, [path]: { text: cached } }));
      return;
    }
    setPatches((prev) => ({ ...prev, [path]: {} })); // 加载中
    api.gitFileDiff(targetCwd, path, targetBase)
      .then((r) => {
        const patch = r?.patch || '';
        changesCache.putPatch(targetCwd, targetBase, path, patch);
        if (cwdRef.current !== targetCwd || baseRef.current !== targetBase) return;
        setPatches((prev) => ({ ...prev, [path]: { text: patch } }));
      })
      .catch((e) => {
        const msg = e?.status === 413 ? 'diff 过大,请在终端查看'
          : e?.status === 504 ? 'git 超时,点击重试'
          : '加载 diff 失败';
        if (cwdRef.current !== targetCwd || baseRef.current !== targetBase) return;
        setPatches((prev) => ({ ...prev, [path]: { error: msg } }));
      });
  }, [api]);

  const toggleFile = useCallback((path, currentlyOpen) => {
    if (currentlyOpen) {
      setOpenFiles((prev) => {
        const next = new Set(prev);
        next.delete(path);
        return next;
      });
      return;
    }
    onSelectFile?.(path);
    pendingScrollFileRef.current = path;
    setOpenFiles((prev) => {
      const next = new Set(prev);
      next.add(path);
      return next;
    });
    if (patches[path] === undefined) loadPatch(path);
  }, [loadPatch, onSelectFile, patches]);

  // 右键菜单动作:与会话级 ChangeReviewPanel 的处理器同构(复制此文件 diff /
  // 复制全部 diff / 展开·折叠全部 diff),只是数据源换成 git 懒加载 patch —— 缓存
  // 优先,未命中即按需拉取。COPY_RELATIVE_PATH / PREVIEW_FILE / LOCATE_IN_FILE_TREE
  // 走通用 runAction 与 SidePanel,这里不重复;菜单项本身由共享的 desktopContextMenu
  // 依 data-desktop-review-* 属性构建(DRY:git 与非 git 复用同一套菜单)。
  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'review') return;
      const targetCwd = cwdRef.current;
      const targetBase = baseRef.current;

      // 单文件 patch:命中缓存直接用,否则拉一次并回填缓存(失败按空串处理,
      // 复制时会提示"没有可复制的 diff")。
      const patchFor = (path) => {
        const cached = changesCache.getPatch(targetCwd, targetBase, path);
        if (cached != null) return Promise.resolve(cached);
        return api.gitFileDiff(targetCwd, path, targetBase)
          .then((r) => {
            const patch = r?.patch || '';
            changesCache.putPatch(targetCwd, targetBase, path, patch);
            return patch;
          })
          .catch(() => '');
      };
      const allPaths = () => (list?.files || []).map((f) => f.path).filter(Boolean);

      if (action === DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF) {
        detail.handled = true;
        patchFor(target.file).then((text) => copyDiffText(text, '已复制文件 diff'));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS) {
        detail.handled = true;
        Promise.all(allPaths().map(patchFor))
          .then((texts) => copyDiffText(texts.filter(Boolean).join('\n\n'), '已复制全部 diff'));
      } else if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS) {
        detail.handled = true;
        const paths = allPaths();
        setOpenFiles(new Set(paths));
        paths.forEach((p) => { if (patches[p] === undefined) loadPatch(p); });
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS) {
        detail.handled = true;
        setOpenFiles(new Set());
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [api, list, patches, loadPatch]);

  // 从 SidePanel 导航列表点文件(expandedFile + revision 变)→ 展开 + 滚动到位。
  useEffect(() => {
    const file = normalizeTreePath(expandedFile);
    if (!file) return;
    pendingScrollFileRef.current = file;
    setOpenFiles((prev) => {
      if (prev.has(file)) return prev;
      const next = new Set(prev);
      next.add(file);
      return next;
    });
    if (patches[file] === undefined) loadPatch(file);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [expandedFile, expandedFileRevision]);

  // 展开目标文件后滚动到它(内容加载完高度会变,故也在 patches 更新后再对齐一次)。
  useLayoutEffect(() => {
    const file = pendingScrollFileRef.current;
    const el = fileListRef.current;
    if (!file || !el || !openFiles.has(file)) return;
    const target = Array.from(el.querySelectorAll('[data-review-file-section]'))
      .find((node) => node.getAttribute('data-review-file-section') === file);
    if (!target) return;
    const listRect = el.getBoundingClientRect();
    const targetRect = target.getBoundingClientRect();
    el.scrollTop = Math.max(0, el.scrollTop + targetRect.top - listRect.top);
    if (patches[file] && 'text' in patches[file]) pendingScrollFileRef.current = '';
  }, [openFiles, patches]);

  const rows = (list?.files || []).map(buildChangeRow);
  const selectedFile = normalizeTreePath(expandedFile);

  return (
    <div className="ace-review-panel" data-change-region="preview-panel" ref={panelRef}>
      <div className="ace-review-summary" data-desktop-review-kind="summary">
        <div className="ace-review-title min-w-0">
          <VsIcon name="editWindow" size={15} />
          <span>变更</span>
          {(list?.branch) && (
            <>
              <span className="text-fg-mute font-normal truncate max-w-[120px]" title={list.branch}>{list.branch}</span>
              <span className="text-fg-mute opacity-60" aria-hidden="true">→</span>
            </>
          )}
          <span className="text-fg-mute font-normal truncate max-w-[140px]" title={`比较基线:${base}`}>{base}</span>
        </div>
        <div className="flex items-center gap-2 shrink-0">
          {loading && <span className="ace-spinner w-3 h-3" />}
          {list && (
            <span className="text-[11px]">
              <span className="ace-change-add">+{list.total_additions ?? 0}</span>{' '}
              <span className="ace-change-del">-{list.total_deletions ?? 0}</span>
            </span>
          )}
          <button
            type="button"
            className="ace-side-panel-refresh-btn !static"
            title="刷新变更"
            aria-label="刷新变更"
            onClick={() => { changesCache.markStale(cwdRef.current); setPatches({}); fetchList(true); }}
          >
            <VsIcon name="refresh" size={14} />
          </button>
        </div>
      </div>

      {list && rows.length > 0 && (
        <div className="px-3 py-1 text-[11px] text-fg-mute border-b border-border/50 truncate">
          {buildSummaryLabel(list)}
        </div>
      )}

      <div className="ace-review-file-list" ref={fileListRef}>
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
        {!error && rows.map((row) => {
          const open = openFiles.has(row.path);
          const selected = selectedFile && normalizeTreePath(row.path) === selectedFile;
          const patch = patches[row.path];
          return (
            <section
              key={row.path}
              className="ace-review-file"
              data-review-file-section={row.path}
            >
              <GitChangeFileRow
                row={row}
                open={open}
                selected={selected}
                onToggle={() => toggleFile(row.path, open)}
              />
              {open && (
                <div className="ace-review-diff-scroll">
                  {patch?.error ? (
                    <div className="ace-empty-state text-danger text-[11px]">
                      <button type="button" onClick={() => loadPatch(row.path)} title="重试">{patch.error}</button>
                    </div>
                  ) : patch && 'text' in patch ? (
                    <GitPatchDiff text={patch.text} outputFormat={outputFormat} />
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
