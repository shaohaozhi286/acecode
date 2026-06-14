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

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
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
  statusForTreeEntry,
} from '../lib/fileTreeChangeStatus.js';
import { fileTreeReloadPaths } from '../lib/fileTreeRefresh.js';
import { clsx } from '../lib/format.js';
import { FileTypeIcon, PanelToggleIcon, VsIcon } from './Icon.jsx';
import { ChangeCompactList } from './ChangeReview.jsx';

const TABS = [
  { key: 'changes', label: '变更' },
  { key: 'files',   label: '文件' },
];

// 切 cwd / 没有 cwd 时给 FileTree 传一个稳定的空 Map / Set,避免每次渲染
// 都 new 一份导致 useEffect deps 抖动 / 子组件 useCallback 失效。
const EMPTY_TREE_CACHE    = new Map();
const EMPTY_EXPANDED_DIRS = new Set();
const EMPTY_REVIEW_STATUS = new Map();

function pathAncestors(path) {
  const parts = String(path || '').split(/[\\/]/).filter(Boolean);
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

  // 递归渲染 — 每个目录的子项展开时插入到该目录节点之后
  const renderEntries = (parentPath, depth) => {
    const entries = treeCache.get(parentPath);
    const err = errors.get(parentPath);
    if (!entries && loading.has(parentPath)) {
      return (
        <div className="px-2 py-1 text-fg-mute text-[11px]" style={{ paddingLeft: 12 + depth * 14 }}>
          加载中…
        </div>
      );
    }
    if (!entries && err) {
      return (
        <div className="px-2 py-1 text-danger text-[11px]" style={{ paddingLeft: 12 + depth * 14 }}>
          {err}
        </div>
      );
    }
    if (!entries) return null;
    const displayEntries = entriesWithReviewRows(entries, parentPath, reviewStatusByPath);
    if (displayEntries.length === 0) {
      return (
        <div className="px-2 py-1 text-fg-mute text-[11px] italic" style={{ paddingLeft: 12 + depth * 14 }}>
          (空目录)
        </div>
      );
    }
    return displayEntries.map((e) => {
      const isDir = e.kind === 'dir';
      const isOpen = isDir && expandedDirs.has(e.path);
      const isActive = !isDir && selectedPath === e.path;
      const absolutePath = joinWorkspacePath(cwd, e.path);
      const explorerPath = isDir ? absolutePath : '';
      const locatePath = isDir ? '' : containingWorkspacePath(cwd, e.path);
      const reviewStatus = e.review_status || statusForTreeEntry(e, reviewStatusByPath);
      const statusTitle = fileChangeStatusTitle(reviewStatus, isDir);
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
            style={{ paddingLeft: 10 + depth * 14 }}
            onClick={() => isDir ? toggleDir(e.path) : onPickFile(e)}
            title={reviewStatus ? `${e.path} - ${statusTitle}` : e.path}
          >
            {isDir ? (
              <VsIcon name={isOpen ? 'folderOpen' : 'folder'} size={14} mono={false} />
            ) : (
              <FileTypeIcon path={e.path || e.name} size={20} />
            )}
            <span className="ace-file-name truncate">{e.name}</span>
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
    <div className="ace-file-tree flex-1 overflow-y-auto py-1">
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
  width = 280,
  collapsed = false,
  onToggleCollapse,
  onOpenFilePreview,
  onOpenSessionChangePreview,
  selectedChangeFile = '',
  selectedChangeFileRevision = 0,
}) {
  const api = useMemo(() => createApi(sessionRef || null), [sessionRef?.port, sessionRef?.token, sessionRef?.workspaceHash]);
  const [activeTab,    setActiveTab]    = useState('files');
  const [selectedPath, setSelectedPath] = useState(null);
  const [fileRefreshToken, setFileRefreshToken] = useState(0);

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
    () => buildReviewStatusMap(effectiveChangeGroups),
    [effectiveChangeGroups],
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
    if (!cwdKey) return;
    setFileRefreshToken(prev => prev + 1);
  }, [cwdKey]);

  const lastFileRefreshKey = useRef('');
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
    setActiveTab('files');
    setSelectedPath(null);
  }, [cwd]);

  useEffect(() => {
    if (!reviewRequest) return;
    setActiveTab('changes');
  }, [reviewRequest]);

  useEffect(() => {
    if (!selectedChangeFile) return;
    setActiveTab('changes');
  }, [selectedChangeFile, selectedChangeFileRevision]);

  const onPickFile = useCallback((entry) => {
    setSelectedPath(entry.path);
    onOpenFilePreview?.(entry.path);
  }, [onOpenFilePreview]);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      const filePath = target?.type === 'review'
        ? target.file
        : (target?.type === 'file' ? (target.relativePath || target.path) : '');
      if (!filePath) return;

      if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE) {
        detail.handled = true;
        setSelectedPath(filePath);
        if (target?.type === 'review') onOpenSessionChangePreview?.(filePath);
        else onOpenFilePreview?.(filePath);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE) {
        detail.handled = true;
        setSelectedPath(filePath);
        setExpandedDirs((prev) => {
          const next = new Set(prev);
          for (const dir of pathAncestors(filePath)) next.add(dir);
          return next;
        });
        setActiveTab('files');
      } else if (action === DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE && target?.type === 'file') {
        detail.handled = true;
        refreshFileTree();
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onOpenFilePreview, onOpenSessionChangePreview, refreshFileTree, setExpandedDirs]);

  return (
    // 宽度由父级 wrapper(.ace-side-panel-shell)控制,这里 100% 占满。width prop
    // 仍接收用于 wrapper 同步(ChatView 把同一值给 wrapper inline style)。
    <div className="ace-side-panel" style={{ width: '100%', minWidth: 0 }}>
      <div className="ace-side-tabs">
        <div className="ace-side-tabs-list">
          {TABS.map((t) => (
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
        {activeTab === 'files' && (
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
        {activeTab === 'files' && (
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
        {activeTab === 'changes' && (
          <ChangesList
            messages={messages}
            groups={effectiveChangeGroups}
            summary={effectiveChangeSummary}
            cwd={cwd}
            selectedFile={selectedChangeFile}
            selectedFileRevision={selectedChangeFileRevision}
            onOpenFile={onOpenSessionChangePreview}
          />
        )}
      </div>
    </div>
  );
}
