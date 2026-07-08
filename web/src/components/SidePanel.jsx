// 单会话视图右侧固定 280px 工作区面板。设计稿:
// project/components/single-session.jsx::FilePanel + Wireframes v2.html L185-207。
//
// 右侧工作区面板只负责导航和紧凑信息列表。
//   - 变更: 当前 session 内 tool_end.hunks 前端聚合的紧凑文件列表
//   - 文件: lazy 加载文件树,点击文件后通知 ChatView 打开中间预览面板
// bash/shell 直接改文件但没有结构化 hunks 时不会进入"变更"列表,空态文案会明示。
//
// 切 session 时面板内部状态(tab/expanded/cache)**保留** — 同一 daemon
// 下所有 session 共享同一个 cwd,文件树没必要重新拉一遍。只有 cwd 真变(典型
// 场景: desktop 切 workspace → daemon 重启)才整面板 reset。
//
// 仅在 single 视图挂载(由 ChatView 的 showSidePanel prop 控制),grid 视图
// 整面板不渲染。

import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { ApiError, createApi } from '../lib/api.js';
import { aggregateHunksFromMessages, summarizeChangeGroups } from '../lib/sessionChanges.js';
import {
  containingWorkspacePath,
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  joinWorkspacePath,
} from '../lib/desktopContextMenu.js';
import {
  buildReviewStatusMap,
  entriesWithReviewRows,
  fileChangeStatusTitle,
  normalizeTreePath,
  normalizeWorkspaceRelativePath,
  statusForTreeEntry,
} from '../lib/fileTreeChangeStatus.js';
import { fileTreeReloadPaths } from '../lib/fileTreeRefresh.js';
import { clsx } from '../lib/format.js';
import {
  SIDE_PANEL_CONTEXT_EFFECTS,
  sidePanelContextActionEffect,
} from '../lib/sidePanelContextActions.js';
import { FileTypeIcon, PanelToggleIcon, VsIcon } from './Icon.jsx';
import { ChangeCompactList } from './ChangeReview.jsx';
import { GitChangesPanel } from './GitChangesPanel.jsx';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';

const TABS = [
  { key: 'changes', label: '变更' },
  { key: 'files',   label: '文件' },
];

// 切 cwd / 没有 cwd 时给 FileTree 传一个稳定的空 Map / Set,避免每次渲染
// 都 new 一份导致 useEffect deps 抖动 / 子组件 useCallback 失效。
const EMPTY_TREE_CACHE    = new Map();
const EMPTY_EXPANDED_DIRS = new Set();
const EMPTY_REVIEW_STATUS = new Map();

function treeParentPath(path) {
  const normalized = normalizeTreePath(path);
  const idx = normalized.lastIndexOf('/');
  return idx < 0 ? '' : normalized.slice(0, idx);
}

function isPathWithinTreeBranch(path, parentPath) {
  const normalized = normalizeTreePath(path);
  const parent = normalizeTreePath(parentPath);
  if (!normalized) return false;
  if (!parent) return false;
  return normalized === parent || normalized.startsWith(`${parent}/`);
}

function TreeIndent({ depth, activeGuideIndex = -1 }) {
  const count = Math.max(0, Number(depth) || 0);
  if (count === 0) return null;
  return (
    <span className="ace-file-tree-indent" aria-hidden="true">
      {Array.from({ length: count }, (_, index) => (
        <span
          key={index}
          className={clsx(
            'ace-file-tree-guide',
            index === activeGuideIndex && 'ace-file-tree-guide-active',
          )}
        />
      ))}
    </span>
  );
}

function TreeArrowIcon({ open }) {
  return (
    <svg
      className="ace-file-tree-arrow"
      width="14"
      height="14"
      viewBox="0 0 24 24"
      fill="none"
      aria-hidden="true"
      focusable="false"
    >
      <path
        d={open ? 'M6 9L12 15L18 9' : 'M9 18L15 12L9 6'}
        stroke="currentColor"
        strokeWidth="1"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}

function pathAncestors(path) {
  const parts = normalizeTreePath(path).split('/').filter(Boolean);
  const result = [];
  for (let i = 1; i < parts.length; i += 1) {
    result.push(parts.slice(0, i).join('/'));
  }
  return result;
}

// ────────────────────────────────────────────────────────────
// 文件 tab — lazy 文件树
// ────────────────────────────────────────────────────────────
// treeCache / expandedDirs 由父组件按 cwd-key 维护(见 SidePanel 主组件),
// 这样 tab 切换 / session 切换都能保留同一 cwd 的缓存,而 cwd 切换时直接读不到
// 不会有任何"清缓存"的并发写者 — 之前父子两边各写一次 treeCache 的 child-first
// effect 竞争(loadDir('') 守卫读到旧 treeCache 直接 bail,父级 setTreeCache(new Map())
// 又跑得更晚把刚拉的根清掉)在这个数据结构下不复存在。
function FileTree({ api, cwd, treeCache, setTreeCache, expandedDirs, setExpandedDirs,
                    selectedPath, onPickFile, onRefreshTree, refreshToken, reviewStatusByPath }) {
  const [loading, setLoading] = useState(new Set()); // path 集合,正在请求中
  const [errors, setErrors]   = useState(new Map()); // path → 错误文案
  const treeRef = useRef(null);
  const selectedNormalizedPath = normalizeTreePath(selectedPath);
  const selectedParentPath = selectedNormalizedPath ? treeParentPath(selectedNormalizedPath) : '';
  const selectedParentGuideIndex = selectedParentPath
    ? selectedParentPath.split('/').filter(Boolean).length - 1
    : -1;

  const loadDir = useCallback(async (path, options = {}) => {
    const force = !!options.force;
    if ((!force && treeCache.has(path)) || loading.has(path)) return;
    setLoading(prev => { const n = new Set(prev); n.add(path); return n; });
    try {
      const entries = await api.listFiles(cwd, path);
      setTreeCache(prev => { const n = new Map(prev); n.set(path, Array.isArray(entries) ? entries : []); return n; });
      setErrors(prev => { const n = new Map(prev); n.delete(path); return n; });
    } catch (err) {
      const msg = err instanceof ApiError ? `加载失败 (HTTP ${err.status})` : '加载失败';
      setErrors(prev => { const n = new Map(prev); n.set(path, msg); return n; });
    } finally {
      setLoading(prev => { const n = new Set(prev); n.delete(path); return n; });
    }
  }, [api, cwd, treeCache, loading, setTreeCache]);

  // 首次 mount + cwd 变 → 拉根。refreshToken 变时不清 UI 状态,而是后台重拉
  // 根和已展开目录,避免 agent 飙字 / tool 完成时把深层目录折回初始状态。
  const lastRefreshToken = useRef(refreshToken);
  useEffect(() => {
    const force = refreshToken !== lastRefreshToken.current;
    lastRefreshToken.current = refreshToken;
    if (!cwd) return;
    const paths = force ? fileTreeReloadPaths(expandedDirs) : [''];
    for (const path of paths) loadDir(path, { force });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cwd, refreshToken]);

  useEffect(() => {
    if (!cwd || expandedDirs.size === 0) return;
    for (const path of expandedDirs) loadDir(path);
  }, [cwd, expandedDirs, loadDir]);

  const toggleDir = useCallback((path) => {
    setExpandedDirs(prev => {
      const n = new Set(prev);
      if (n.has(path)) n.delete(path);
      else { n.add(path); loadDir(path); }
      return n;
    });
  }, [loadDir, setExpandedDirs]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'file') return;
      const path = target.relativePath || target.path || '';
      if (!path) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE && target.kind !== 'directory') {
        detail.handled = true;
        onPickFile?.({ path, name: path.split(/[\\/]/).pop() || path });
      } else if (action === DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE) {
        detail.handled = true;
        onRefreshTree?.();
      } else if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_DIRECTORY && target.kind === 'directory') {
        detail.handled = true;
        loadDir(path);
        setExpandedDirs((prev) => {
          const next = new Set(prev);
          next.add(path);
          return next;
        });
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_DIRECTORY && target.kind === 'directory') {
        detail.handled = true;
        setExpandedDirs((prev) => {
          const next = new Set(prev);
          next.delete(path);
          return next;
        });
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [loadDir, onPickFile, onRefreshTree, setExpandedDirs]);

  useLayoutEffect(() => {
    if (!selectedNormalizedPath) return;
    const tree = treeRef.current;
    if (!tree) return;
    const row = Array.from(tree.querySelectorAll('[data-desktop-file-path]'))
      .find((node) => normalizeTreePath(node.getAttribute('data-desktop-file-path')) === selectedNormalizedPath);
    if (!row) return;
    const treeRect = tree.getBoundingClientRect();
    const rowRect = row.getBoundingClientRect();
    const margin = 8;
    if (rowRect.top < treeRect.top) {
      tree.scrollTop = Math.max(0, tree.scrollTop + rowRect.top - treeRect.top - margin);
    } else if (rowRect.bottom > treeRect.bottom) {
      tree.scrollTop = Math.max(0, tree.scrollTop + rowRect.bottom - treeRect.bottom + margin);
    }
  }, [selectedNormalizedPath, treeCache, expandedDirs]);

  // 递归渲染 — 每个目录的子项展开时插入到该目录节点之后
  const renderEntries = (parentPath, depth) => {
    const entries = treeCache.get(parentPath);
    const err = errors.get(parentPath);
    if (!entries && loading.has(parentPath)) {
      return (
        <div className="ace-file-tree-message text-fg-mute">
          <TreeIndent depth={depth} />
          <span className="ace-file-tree-arrow-spacer" aria-hidden="true" />
          <span>加载中…</span>
        </div>
      );
    }
    if (!entries && err) {
      return (
        <div className="ace-file-tree-message text-danger">
          <TreeIndent depth={depth} />
          <span className="ace-file-tree-arrow-spacer" aria-hidden="true" />
          <span>{err}</span>
        </div>
      );
    }
    if (!entries) return null;
    const displayEntries = entriesWithReviewRows(entries, parentPath, reviewStatusByPath);
    if (displayEntries.length === 0) {
      return (
        <div className="ace-file-tree-message text-fg-mute italic">
          <TreeIndent depth={depth} />
          <span className="ace-file-tree-arrow-spacer" aria-hidden="true" />
          <span>(空目录)</span>
        </div>
      );
    }
    return displayEntries.map((e) => {
      const isDir = e.kind === 'dir';
      const isOpen = isDir && expandedDirs.has(e.path);
      const isActive = !isDir && selectedNormalizedPath
        && normalizeTreePath(e.path) === selectedNormalizedPath;
      const absolutePath = joinWorkspacePath(cwd, e.path);
      const explorerPath = isDir ? absolutePath : '';
      const locatePath = isDir ? '' : containingWorkspacePath(cwd, e.path);
      const reviewStatus = e.review_status || statusForTreeEntry(e, reviewStatusByPath);
      const statusTitle = fileChangeStatusTitle(reviewStatus, isDir);
      const activeGuideIndex = selectedParentGuideIndex >= 0
        && depth > selectedParentGuideIndex
        && isPathWithinTreeBranch(e.path, selectedParentPath)
        ? selectedParentGuideIndex
        : -1;
      return (
        <div key={e.path}>
          <button
            type="button"
            data-desktop-open-in-explorer-kind={isDir ? 'directory' : undefined}
            data-desktop-open-in-explorer-path={explorerPath || undefined}
            data-desktop-file-path={e.path || undefined}
            data-desktop-file-absolute-path={absolutePath || undefined}
            data-desktop-file-locate-path={locatePath || undefined}
            data-desktop-file-kind={isDir ? 'directory' : 'file'}
            data-desktop-file-selected={isActive ? 'true' : 'false'}
            data-desktop-file-expanded={isDir ? (isOpen ? 'true' : 'false') : undefined}
            data-desktop-file-preview={isDir ? undefined : 'true'}
            data-desktop-file-add-context={isDir ? undefined : 'true'}
            data-review-status={reviewStatus || undefined}
            className={clsx(
              'ace-file-row w-full flex items-center gap-1 text-left text-[12px] py-[3px] pr-2',
              'hover:bg-surface-hi cursor-pointer',
              isActive && 'bg-accent-soft text-accent',
            )}
            onClick={() => isDir ? toggleDir(e.path) : onPickFile(e)}
            title={reviewStatus ? `${e.path} - ${statusTitle}` : e.path}
          >
            <TreeIndent depth={depth} activeGuideIndex={activeGuideIndex} />
            {isDir ? (
              <TreeArrowIcon open={isOpen} />
            ) : (
              <span className="ace-file-tree-arrow-spacer" aria-hidden="true" />
            )}
            {isDir ? (
              <span className="ace-file-name truncate">{e.name}</span>
            ) : (
              <span className="ace-file-leaf-content">
                <FileTypeIcon path={e.path || e.name} size={20} />
                <span className="ace-file-name truncate">{e.name}</span>
              </span>
            )}
            {reviewStatus && (
              <span
                className="ace-file-status-badge"
                data-status={reviewStatus}
                title={statusTitle}
                aria-label={statusTitle}
              >
                {reviewStatus}
              </span>
            )}
          </button>
          {isDir && isOpen && renderEntries(e.path, depth + 1)}
        </div>
      );
    });
  };

  if (!cwd) {
    return <div className="ace-empty-state">无 cwd,请先选择会话</div>;
  }
  return (
    <div className="ace-file-tree flex-1 overflow-y-auto py-1" ref={treeRef}>
      {renderEntries('', 0)}
    </div>
  );
}

// ────────────────────────────────────────────────────────────
// 审查 tab — 聚合 messages.hunks
// ────────────────────────────────────────────────────────────
function ChangesList({
  messages,
  groups,
  summary,
  cwd,
  selectedFile,
  selectedFileRevision = 0,
  onOpenFile,
}) {
  const fallbackGroups = useMemo(() => aggregateHunksFromMessages(messages || []), [messages]);
  const reviewGroups = groups || fallbackGroups;
  const reviewSummary = summary || summarizeChangeGroups(reviewGroups);
  return (
    <ChangeCompactList
      groups={reviewGroups}
      summary={reviewSummary}
      cwd={cwd}
      selectedFile={selectedFile}
      selectedFileRevision={selectedFileRevision}
      onOpenFile={onOpenFile}
    />
  );
}

// ────────────────────────────────────────────────────────────
// 主组件
// ────────────────────────────────────────────────────────────
export function SidePanel({
  sessionRef,
  sessionId,
  cwd,
  messages,
  changeGroups = null,
  changeSummary = null,
  fileRefreshKey = '',
  reviewRequest = 0,
  filesEnabled = true,
  width = 280,
  collapsed = false,
  busy = false,
  onToggleCollapse,
  onOpenFilePreview,
  onOpenSessionChangePreview,
  selectedChangeFile = '',
  selectedChangeFileRevision = 0,
}) {
  const api = useMemo(() => createApi(sessionRef || null), [sessionRef?.port, sessionRef?.token, sessionRef?.workspaceHash]);
  const [activeTab,    setActiveTab]    = useState(filesEnabled ? 'files' : 'changes');
  const [selectedPath, setSelectedPath] = useState(null);
  const [fileRefreshToken, setFileRefreshToken] = useState(0);
  // git 仓库检测(redesign-sidepanel-git-changes):is_repo 时「变更」tab
  // 整体切到 git 级视图;非仓库保留会话级 hunks 聚合。按 cwd 拉一次;
  // checkout 事件(分支可能变)时重拉。
  const [gitInfo, setGitInfo] = useState(null);
  useEffect(() => {
    let cancelled = false;
    setGitInfo(null);
    if (!cwd) return undefined;
    const load = () => {
      api.gitInfo(cwd)
        .then((info) => { if (!cancelled) setGitInfo(info); })
        .catch(() => { if (!cancelled) setGitInfo(null); });
    };
    load();
    const handler = (event) => {
      const changedCwd = event?.detail?.cwd || '';
      if (!changedCwd || changedCwd === cwd) load();
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => {
      cancelled = true;
      window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
    };
  }, [api, cwd]);
  const visibleTabs = useMemo(
    () => TABS.filter((tab) => filesEnabled || tab.key !== 'files'),
    [filesEnabled],
  );

  // 按 cwd 隔离的文件树缓存:cwd → Map<path, entries[]>。每个 cwd 一份独立缓存,
  // 切 cwd 时新 cwd 自动 .get() 取不到,FileTree 守卫不会误命中旧数据,也不需要
  // 任何"清缓存"动作,从根上消除 child-first effect 竞争(具体细节见 FileTree
  // 上方注释)。同 cwd 内 tab 切换 / session 切换都能复用,符合"切 session 不
  // 重复拉同一棵树"的原设计。
  const [treeCacheByCwd,    setTreeCacheByCwd]    = useState(new Map()); // cwd → Map<path, entries[]>
  const [expandedDirsByCwd, setExpandedDirsByCwd] = useState(new Map()); // cwd → Set<path>

  const cwdKey = cwd || '';
  const treeCache    = treeCacheByCwd.get(cwdKey) || EMPTY_TREE_CACHE;
  const expandedDirs = expandedDirsByCwd.get(cwdKey) || EMPTY_EXPANDED_DIRS;
  const fallbackChangeGroups = useMemo(
    () => (changeGroups ? null : aggregateHunksFromMessages(messages || [])),
    [changeGroups, messages],
  );
  const effectiveChangeGroups = changeGroups || fallbackChangeGroups || [];
  const effectiveChangeSummary = changeSummary || summarizeChangeGroups(effectiveChangeGroups);
  const reviewStatusByPath = useMemo(
    () => buildReviewStatusMap(effectiveChangeGroups, cwd),
    [effectiveChangeGroups, cwd],
  );

  const setTreeCache = useCallback((updater) => {
    setTreeCacheByCwd(prev => {
      const cur = prev.get(cwdKey) || new Map();
      const next = typeof updater === 'function' ? updater(cur) : updater;
      const n = new Map(prev);
      n.set(cwdKey, next);
      return n;
    });
  }, [cwdKey]);
  const setExpandedDirs = useCallback((updater) => {
    setExpandedDirsByCwd(prev => {
      const cur = prev.get(cwdKey) || new Set();
      const next = typeof updater === 'function' ? updater(cur) : updater;
      const n = new Map(prev);
      n.set(cwdKey, next);
      return n;
    });
  }, [cwdKey]);

  const refreshFileTree = useCallback(() => {
    if (!filesEnabled || !cwdKey) return;
    setFileRefreshToken(prev => prev + 1);
  }, [cwdKey, filesEnabled]);
  const collapseFileTree = useCallback(() => {
    if (!filesEnabled || !cwdKey) return;
    setExpandedDirs((prev) => (prev.size === 0 ? prev : new Set()));
  }, [cwdKey, filesEnabled, setExpandedDirs]);

  const lastFileRefreshKey = useRef('');
  useEffect(() => {
    if (!filesEnabled && activeTab === 'files') setActiveTab('changes');
  }, [activeTab, filesEnabled]);

  useEffect(() => {
    const nextKey = String(fileRefreshKey || '');
    if (!nextKey) {
      lastFileRefreshKey.current = nextKey;
      return;
    }
    if (nextKey === lastFileRefreshKey.current) return;
    lastFileRefreshKey.current = nextKey;
    refreshFileTree();
  }, [fileRefreshKey, refreshFileTree]);

  // cwd 变时 tab 回到「文件」,清选中文件。**不**清 treeCache,
  // 自然按 cwd-key 隔离即可。
  const lastCwd = useRef('');
  useEffect(() => {
    const c = cwd || '';
    if (c === lastCwd.current) return;
    lastCwd.current = c;
    setActiveTab(filesEnabled ? 'files' : 'changes');
    setSelectedPath(null);
  }, [cwd, filesEnabled]);

  useEffect(() => {
    if (!reviewRequest) return;
    setActiveTab('changes');
  }, [reviewRequest]);

  useEffect(() => {
    if (!selectedChangeFile) return;
    setActiveTab('changes');
  }, [selectedChangeFile, selectedChangeFileRevision]);

  const onPickFile = useCallback((entry) => {
    if (!filesEnabled) return;
    setSelectedPath(entry.path);
    onOpenFilePreview?.(entry.path);
  }, [filesEnabled, onOpenFilePreview]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      const effect = sidePanelContextActionEffect({ action, target, filesEnabled, cwd });
      if (!effect) return;

      if (effect.type === SIDE_PANEL_CONTEXT_EFFECTS.OPEN_FILE_PREVIEW) {
        detail.handled = true;
        setSelectedPath(effect.normalizedFilePath);
        onOpenFilePreview?.(effect.normalizedFilePath);
      } else if (effect.type === SIDE_PANEL_CONTEXT_EFFECTS.LOCATE_IN_FILE_TREE) {
        detail.handled = true;
        const relativeFilePath = normalizeWorkspaceRelativePath(effect.normalizedFilePath, cwd);
        setSelectedPath(relativeFilePath);
        setExpandedDirs((prev) => {
          const next = new Set(prev);
          for (const dir of pathAncestors(relativeFilePath)) next.add(dir);
          return next;
        });
        setActiveTab('files');
      } else if (effect.type === SIDE_PANEL_CONTEXT_EFFECTS.REFRESH_FILE_TREE) {
        detail.handled = true;
        refreshFileTree();
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [cwd, filesEnabled, onOpenFilePreview, refreshFileTree, setExpandedDirs]);

  return (
    // 宽度由父级 wrapper(.ace-side-panel-shell)控制,这里 100% 占满。width prop
    // 仍接收用于 wrapper 同步(ChatView 把同一值给 wrapper inline style)。
    <div className="ace-side-panel" style={{ width: '100%', minWidth: 0 }}>
      <div className="ace-side-tabs">
        <div className="ace-side-tabs-list">
          {visibleTabs.map((t) => (
            <button
              key={t.key}
              type="button"
              role="tab"
              aria-selected={activeTab === t.key}
              className="ace-side-tab"
              onClick={() => {
                if (activeTab === t.key && t.key === 'files') refreshFileTree();
                setActiveTab(t.key);
              }}
            >
              {t.label}
            </button>
          ))}
        </div>
        {filesEnabled && activeTab === 'files' && (
          <button
            type="button"
            onClick={refreshFileTree}
            className="ace-side-panel-refresh-btn"
            title="刷新文件列表"
            aria-label="刷新文件列表"
          >
            <VsIcon name="refresh" size={16} />
          </button>
        )}
        {filesEnabled && activeTab === 'files' && (
          <button
            type="button"
            onClick={collapseFileTree}
            className="ace-side-panel-collapse-tree-btn"
            title="全部折叠文件树"
            aria-label="全部折叠文件树"
          >
            <VsIcon name="collapseAll" size={16} />
          </button>
        )}
        {onToggleCollapse && (
          <button
            type="button"
            onClick={onToggleCollapse}
            className="ace-side-panel-collapse-btn"
            title={collapsed ? '展开右侧面板' : '收起右侧面板'}
            aria-label={collapsed ? '展开右侧面板' : '收起右侧面板'}
            aria-expanded={!collapsed}
          >
            <PanelToggleIcon side="right" size={15} />
          </button>
        )}
      </div>

      <div className="flex-1 flex flex-col overflow-hidden">
        {filesEnabled && activeTab === 'files' && (
          <FileTree
            api={api}
            cwd={cwd}
            treeCache={treeCache}
            setTreeCache={setTreeCache}
            expandedDirs={expandedDirs}
            setExpandedDirs={setExpandedDirs}
            selectedPath={selectedPath}
            onPickFile={onPickFile}
            onRefreshTree={refreshFileTree}
            refreshToken={fileRefreshToken}
            reviewStatusByPath={reviewStatusByPath || EMPTY_REVIEW_STATUS}
          />
        )}
        {activeTab === 'changes' && (gitInfo?.is_repo ? (
          <GitChangesPanel
            api={api}
            cwd={cwd}
            gitInfo={gitInfo}
            busy={busy}
            visible={!collapsed && activeTab === 'changes'}
          />
        ) : (
          <ChangesList
            messages={messages}
            groups={effectiveChangeGroups}
            summary={effectiveChangeSummary}
            cwd={cwd}
            selectedFile={selectedChangeFile}
            selectedFileRevision={selectedChangeFileRevision}
            onOpenFile={onOpenSessionChangePreview}
          />
        ))}
      </div>
    </div>
  );
}
