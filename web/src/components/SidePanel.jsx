// 单会话视图右侧固定 280px 工作区面板。设计稿:
// project/components/single-session.jsx::FilePanel + Wireframes v2.html L185-207。
//
// 三个 tab,设计稿原画的"上 tab + 下方共用代码预览区"两段式被用户校正:
// 不做共用代码区,三个 tab 各自占满下半区。
//   - 文件: lazy 加载文件树,点击文件 → 自动切预览 tab + load
//   - 审查: 当前 session 内 tool_end.hunks 前端聚合(file_edit/file_write 才有
//           hunks,bash sed 抓不到 — 已知 limitation,空态文案要明示)
//   - 预览: 选中文件 highlight.js 高亮原文,5MB cap / binary 拒绝时友好提示
//
// 切 session 时面板内部状态(tab/expanded/cache/preview)**保留** — 同一 daemon
// 下所有 session 共享同一个 cwd,文件树没必要重新拉一遍。只有 cwd 真变(典型
// 场景: desktop 切 workspace → daemon 重启)才整面板 reset。
//
// 仅在 single 视图挂载(由 ChatView 的 showSidePanel prop 控制),grid 视图
// 整面板不渲染。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import hljs from 'highlight.js/lib/core';
import { ApiError, createApi } from '../lib/api.js';
import { aggregateHunksFromMessages, summarizeChangeGroups } from '../lib/sessionChanges.js';
import { langForFile } from '../lib/lang.js';
import { renderMarkdown } from '../lib/markdown.js';
import { joinWorkspacePath } from '../lib/desktopContextMenu.js';
import { usePreference } from '../lib/usePreference.js';
import { clsx, formatBytes } from '../lib/format.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { VsIcon } from './Icon.jsx';
import { ChangeReviewPanel } from './ChangeReview.jsx';

const FILE_PREVIEW_WRAP_STORAGE_KEY = 'acecode.filePreviewWrap.v1';
const MARKDOWN_EXTENSIONS = new Set(['md', 'markdown']);
const IMAGE_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg']);

const TABS = [
  { key: 'files',   label: '文件' },
  { key: 'changes', label: '审查' },
  { key: 'preview', label: '预览' },
];

function extensionForPath(path) {
  const name = String(path || '').split(/[\\/]/).pop() || '';
  const dot = name.lastIndexOf('.');
  if (dot < 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

function isMarkdownPreview(path) {
  return MARKDOWN_EXTENSIONS.has(extensionForPath(path));
}

function isImagePreview(path) {
  return IMAGE_EXTENSIONS.has(extensionForPath(path));
}

// 切 cwd / 没有 cwd 时给 FileTree 传一个稳定的空 Map / Set,避免每次渲染
// 都 new 一份导致 useEffect deps 抖动 / 子组件 useCallback 失效。
const EMPTY_TREE_CACHE    = new Map();
const EMPTY_EXPANDED_DIRS = new Set();

function validateBooleanPreference(value) {
  return typeof value === 'boolean';
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
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
                    selectedPath, onPickFile, refreshToken }) {
  const [loading, setLoading] = useState(new Set()); // path 集合,正在请求中
  const [errors, setErrors]   = useState(new Map()); // path → 错误文案

  const loadDir = useCallback(async (path) => {
    if (treeCache.has(path) || loading.has(path)) return;
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

  // 首次 mount + cwd 变 → 拉根。treeCache 是 cwd 私有的,新 cwd 必然为空 Map,
  // loadDir 守卫不会命中,所以一定会发请求。
  useEffect(() => {
    if (!cwd) return;
    loadDir('');
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cwd, refreshToken]);

  const toggleDir = useCallback((path) => {
    setExpandedDirs(prev => {
      const n = new Set(prev);
      if (n.has(path)) n.delete(path);
      else { n.add(path); loadDir(path); }
      return n;
    });
  }, [loadDir, setExpandedDirs]);

  // 递归渲染 — 每个目录的子项展开时插入到该目录节点之后
  const renderEntries = (parentPath, depth) => {
    const entries = treeCache.get(parentPath);
    const err = errors.get(parentPath);
    if (loading.has(parentPath)) {
      return (
        <div className="px-2 py-1 text-fg-mute text-[11px]" style={{ paddingLeft: 8 + depth * 14 }}>
          加载中…
        </div>
      );
    }
    if (err) {
      return (
        <div className="px-2 py-1 text-danger text-[11px]" style={{ paddingLeft: 8 + depth * 14 }}>
          {err}
        </div>
      );
    }
    if (!entries) return null;
    if (entries.length === 0) {
      return (
        <div className="px-2 py-1 text-fg-mute text-[11px] italic" style={{ paddingLeft: 8 + depth * 14 }}>
          (空目录)
        </div>
      );
    }
    return entries.map((e) => {
      const isDir = e.kind === 'dir';
      const isOpen = isDir && expandedDirs.has(e.path);
      const isActive = !isDir && selectedPath === e.path;
      const explorerPath = isDir ? joinWorkspacePath(cwd, e.path) : '';
      return (
        <div key={e.path}>
          <button
            type="button"
            data-desktop-open-in-explorer-kind={isDir ? 'directory' : undefined}
            data-desktop-open-in-explorer-path={explorerPath || undefined}
            className={clsx(
              'ace-file-row w-full flex items-center gap-1 text-left text-[12px] font-mono py-[3px] pr-2',
              'hover:bg-surface-hi cursor-pointer',
              isActive && 'bg-accent-soft text-accent',
            )}
            style={{ paddingLeft: 6 + depth * 14 }}
            onClick={() => isDir ? toggleDir(e.path) : onPickFile(e)}
            title={e.path}
          >
            {isDir ? (
              <>
                <VsIcon name={isOpen ? 'glyphDown' : 'expandRight'} size={9} />
                <VsIcon name={isOpen ? 'folderOpen' : 'folder'} size={14} mono={false} />
              </>
            ) : (
              <>
                <span className="inline-block w-[9px]" />
                <VsIcon name="file" size={14} mono={false} />
              </>
            )}
            <span className="truncate">{e.name}</span>
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
function ChangesList({ messages, groups, summary }) {
  const fallbackGroups = useMemo(() => aggregateHunksFromMessages(messages || []), [messages]);
  const reviewGroups = groups || fallbackGroups;
  const reviewSummary = summary || summarizeChangeGroups(reviewGroups);
  return <ChangeReviewPanel groups={reviewGroups} summary={reviewSummary} />;
}

// ────────────────────────────────────────────────────────────
// 预览 tab — Markdown 渲染 / 图片 / 文件原文 + hljs 高亮
// ────────────────────────────────────────────────────────────
function PreviewPanel({ api, cwd, path, wrapPreview, onToggleWrapPreview, refreshToken }) {
  const [state, setState] = useState({
    status: 'idle',
    kind: 'text',
    text: '',
    error: null,
    lang: '',
    size: 0,
    imageUrl: '',
    contentType: '',
  });
  const [markdownSource, setMarkdownSource] = useState(false);

  useEffect(() => {
    if (!cwd || !path) {
      setState({ status: 'idle', kind: 'text', text: '', error: null, lang: '', size: 0, imageUrl: '', contentType: '' });
      return;
    }
    let cancelled = false;
    let objectUrl = '';
    setMarkdownSource(false);
    setState({ status: 'loading', kind: 'text', text: '', error: null, lang: '', size: 0, imageUrl: '', contentType: '' });

    if (isImagePreview(path)) {
      api.readFileBlob(cwd, path).then((blob) => {
        if (cancelled) return;
        objectUrl = URL.createObjectURL(blob);
        setState({
          status: 'ok',
          kind: 'image',
          text: '',
          error: null,
          lang: '',
          size: blob.size || 0,
          imageUrl: objectUrl,
          contentType: blob.type || '',
        });
      }).catch((err) => {
        if (cancelled) return;
        let msg = '读取失败';
        let extraSize = 0;
        if (err instanceof ApiError) {
          const body = err.body;
          if (body && typeof body === 'object') {
            if (body.error === 'file too large') {
              extraSize = Number(body.size || 0);
              msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
            } else if (body.error === 'unsupported file type') msg = '该图片格式暂不支持预览';
            else if (body.error === 'not found') msg = '文件不存在';
            else if (body.error) msg = body.error;
          } else {
            msg = `读取失败 (HTTP ${err.status})`;
          }
        }
        setState({ status: 'error', kind: 'image', text: '', error: msg, lang: '', size: extraSize, imageUrl: '', contentType: '' });
      });
      return () => {
        cancelled = true;
        if (objectUrl) URL.revokeObjectURL(objectUrl);
      };
    }

    api.readFile(cwd, path).then((text) => {
      if (cancelled) return;
      setState({
        status: 'ok',
        kind: isMarkdownPreview(path) ? 'markdown' : 'text',
        text,
        error: null,
        lang: langForFile(path),
        size: text.length,
        imageUrl: '',
        contentType: '',
      });
    }).catch((err) => {
      if (cancelled) return;
      // err.body 可能是 {error, size}
      let msg = '读取失败';
      let extraSize = 0;
      if (err instanceof ApiError) {
        const body = err.body;
        if (body && typeof body === 'object') {
          if (body.error === 'binary')          msg = '二进制文件,无法预览';
          else if (body.error === 'file too large') {
            extraSize = Number(body.size || 0);
            msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
          } else if (body.error === 'not found') msg = '文件不存在';
          else if (body.error)                   msg = body.error;
        } else {
          msg = `读取失败 (HTTP ${err.status})`;
        }
      }
      setState({ status: 'error', kind: 'text', text: '', error: msg, lang: '', size: extraSize, imageUrl: '', contentType: '' });
    });
    return () => { cancelled = true; };
  }, [api, cwd, path, refreshToken]);

  if (!path) {
    return <div className="ace-empty-state">未选中文件,请在「文件」tab 中点击一个文件</div>;
  }
  if (state.status === 'loading') {
    return <div className="ace-empty-state">加载中…</div>;
  }
  if (state.status === 'error') {
    return (
      <div className="ace-empty-state">
        <div className="text-danger text-[12px] mb-1">{state.error}</div>
        <div className="text-fg-mute text-[10px] font-mono opacity-70 break-all">{path}</div>
      </div>
    );
  }
  if (state.status !== 'ok') return null;
  if (state.kind === 'image') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden">
        <div className="px-2 py-1 text-[10px] text-fg-mute font-mono border-b border-border truncate" title={path}>
          {path} · {formatBytes(state.size)}{state.contentType ? ` · ${state.contentType}` : ''}
        </div>
        <div className="ace-side-image-preview">
          <img src={state.imageUrl} alt={path} draggable="false" />
        </div>
      </div>
    );
  }

  const lang = state.lang;
  let html;
  if (lang && hljs.getLanguage(lang)) {
    try {
      html = `<pre class="hljs"><code class="hljs language-${escapeHtml(lang)}">`
           + hljs.highlight(state.text, { language: lang, ignoreIllegals: true }).value
           + `</code></pre>`;
    } catch {
      html = `<pre><code>${escapeHtml(state.text)}</code></pre>`;
    }
  } else {
    html = `<pre><code>${escapeHtml(state.text)}</code></pre>`;
  }
  const wrapTitle = wrapPreview ? '关闭自动换行' : '开启自动换行';
  const isMarkdown = state.kind === 'markdown';
  const showMarkdownRendered = isMarkdown && !markdownSource;
  const markdownToggleTitle = markdownSource ? '渲染 Markdown' : '查看 Markdown 原文';
  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      <div className="px-2 py-1 text-[10px] text-fg-mute font-mono border-b border-border truncate" title={path}>
        {path} · {formatBytes(state.text.length)}{showMarkdownRendered ? ' · Markdown 预览' : (lang ? ` · ${lang}` : '')}
      </div>
      <CopyableCodeFrame
        text={state.text}
        className="flex-1 min-h-0 ace-side-preview-code"
        data-wrap={wrapPreview ? 'true' : 'false'}
        actions={(
          <>
            {isMarkdown && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-markdown-btn', showMarkdownRendered && 'is-active')}
                title={markdownToggleTitle}
                aria-label={markdownToggleTitle}
                aria-pressed={showMarkdownRendered}
                onClick={(event) => { event.stopPropagation(); setMarkdownSource((prev) => !prev); }}
              >
                <VsIcon name={markdownSource ? 'document' : 'code'} size={14} />
              </button>
            )}
            {!showMarkdownRendered && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-wrap-btn', wrapPreview && 'is-active')}
                title={wrapTitle}
                aria-label={wrapTitle}
                aria-pressed={wrapPreview}
                onClick={(event) => { event.stopPropagation(); onToggleWrapPreview?.(); }}
              >
                <VsIcon name="wordWrap" size={14} />
              </button>
            )}
          </>
        )}
      >
        {showMarkdownRendered ? (
          <div
            className="h-full overflow-auto ace-md ace-side-markdown-preview"
            dangerouslySetInnerHTML={{ __html: renderMarkdown(state.text) }}
          />
        ) : (
          <div
            className="h-full overflow-auto text-[11px] ace-preview"
            dangerouslySetInnerHTML={{ __html: html }}
          />
        )}
      </CopyableCodeFrame>
    </div>
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
  // 最大化:面板撑满整个聊天区,聊天区被父组件隐藏。再点击切换图标还原。
  maximized = false,
  onToggleMaximize,
}) {
  const api = useMemo(() => createApi(sessionRef || null), [sessionRef?.port, sessionRef?.token, sessionRef?.workspaceHash]);
  const [wrapPreview, setWrapPreview] = usePreference(
    FILE_PREVIEW_WRAP_STORAGE_KEY,
    false,
    validateBooleanPreference,
  );

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
    setTreeCacheByCwd(prev => {
      if (!prev.has(cwdKey)) return prev;
      const n = new Map(prev);
      n.delete(cwdKey);
      return n;
    });
    setExpandedDirsByCwd(prev => {
      if (!prev.has(cwdKey)) return prev;
      const n = new Map(prev);
      n.delete(cwdKey);
      return n;
    });
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

  // cwd 变时 tab 回到「文件」,清选中文件(预览 tab 会自动空)。**不**清 treeCache,
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

  const onPickFile = useCallback((entry) => {
    setSelectedPath(entry.path);
    setActiveTab('preview');
  }, []);
  const toggleWrapPreview = useCallback(() => {
    setWrapPreview((prev) => !prev);
  }, [setWrapPreview]);

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
        {(activeTab === 'files' || activeTab === 'preview') && (
          <button
            type="button"
            onClick={refreshFileTree}
            className="ace-side-panel-refresh-btn"
            title="刷新文件列表和预览"
            aria-label="刷新文件列表和预览"
          >
            <VsIcon name="running" size={14} />
          </button>
        )}
        {/* 最大化按钮放在收起按钮的左侧。最大化时聊天区已隐藏,继续允许"收起"
            会让整个区域变空,所以最大化态下隐藏收起按钮。 */}
        {onToggleMaximize && (
          <button
            type="button"
            onClick={onToggleMaximize}
            className="ace-side-panel-maximize-btn"
            title={maximized ? '还原右侧面板' : '展开为整屏'}
            aria-label={maximized ? '还原右侧面板' : '展开为整屏'}
            aria-pressed={maximized}
          >
            <VsIcon name={maximized ? 'screenNormal' : 'screenFull'} size={14} />
          </button>
        )}
        {onToggleCollapse && !maximized && (
          <button
            type="button"
            onClick={onToggleCollapse}
            className="ace-side-panel-collapse-btn"
            title={collapsed ? '展开右侧面板' : '收起右侧面板'}
            aria-label={collapsed ? '展开右侧面板' : '收起右侧面板'}
            aria-expanded={!collapsed}
          >
            <VsIcon name="expandRight" size={14} />
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
            refreshToken={fileRefreshToken}
          />
        )}
        {activeTab === 'changes' && (
          <ChangesList
            messages={messages}
            groups={changeGroups}
            summary={changeSummary}
          />
        )}
        {activeTab === 'preview' && (
          <PreviewPanel
            api={api}
            cwd={cwd}
            path={selectedPath}
            wrapPreview={wrapPreview}
            onToggleWrapPreview={toggleWrapPreview}
            refreshToken={fileRefreshToken}
          />
        )}
      </div>
    </div>
  );
}
