// 单会话视图右侧固定 280px 工作区面板。设计稿:
// project/components/single-session.jsx::FilePanel + Wireframes v2.html L185-207。
//
// 三个 tab,设计稿原画的"上 tab + 下方共用代码预览区"两段式被用户校正:
// 不做共用代码区,三个 tab 各自占满下半区。
//   - 文件: lazy 加载文件树,点击文件 → 自动切预览 tab + load
//   - 变更: 当前 session 内 tool_end.hunks 前端聚合(file_edit/file_write 才有
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
import * as Diff2Html from 'diff2html';
import hljs from 'highlight.js/lib/core';
import { ApiError, createApi } from '../lib/api.js';
import { aggregateHunksFromMessages } from '../lib/sessionChanges.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { langForFile } from '../lib/lang.js';
import { joinWorkspacePath } from '../lib/desktopContextMenu.js';
import { clsx, formatBytes } from '../lib/format.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { VsIcon } from './Icon.jsx';

const TABS = [
  { key: 'files',   label: '文件' },
  { key: 'changes', label: '变更' },
  { key: 'preview', label: '预览' },
];

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

// ────────────────────────────────────────────────────────────
// 文件 tab — lazy 文件树
// ────────────────────────────────────────────────────────────
function FileTree({ api, cwd, expandedDirs, setExpandedDirs, treeCache, setTreeCache,
                    selectedPath, onPickFile }) {
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

  // 首次 mount + cwd 变 → 拉根
  useEffect(() => {
    if (!cwd) return;
    loadDir('');
    // cwd 变会触发外层 reset,treeCache 已清空 — 这里只负责拉新根
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cwd]);

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
// 变更 tab — 聚合 messages.hunks
// ────────────────────────────────────────────────────────────
function ChangesList({ messages }) {
  const groups = useMemo(() => aggregateHunksFromMessages(messages || []), [messages]);

  if (groups.length === 0) {
    return (
      <div className="flex-1 flex flex-col items-center justify-center text-center px-4 py-8 gap-2">
        <div className="text-fg-mute text-[12px]">本会话暂无文件变更</div>
        <div className="text-fg-mute text-[10px] opacity-70">
          仅显示 file_edit / file_write 工具的改动
        </div>
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto px-2 py-2 flex flex-col gap-2">
      {groups.map((g) => (
        <ChangeGroup key={g.file} group={g} />
      ))}
    </div>
  );
}

function ChangeGroup({ group }) {
  const [open, setOpen] = useState(true);
  const diffHtml = useMemo(() => {
    if (!open) return '';
    const unified = hunksToUnifiedDiff(group.hunks, group.file);
    if (!unified) return '';
    try {
      return Diff2Html.html(unified, {
        drawFileList: false,
        outputFormat: 'line-by-line',
        matching: 'lines',
      });
    } catch {
      return '';
    }
  }, [open, group]);

  return (
    <div className="border border-border rounded-md bg-surface overflow-hidden">
      <button
        type="button"
        className="w-full flex items-center gap-1.5 px-2 py-1.5 cursor-pointer hover:bg-surface-hi text-left"
        onClick={() => setOpen((v) => !v)}
      >
        <VsIcon name={open ? 'glyphDown' : 'expandRight'} size={9} />
        <VsIcon name="file" size={13} mono={false} />
        <span className="text-[11px] font-mono truncate flex-1">{group.file}</span>
        {group.totalAdditions > 0 && (
          <span className="text-[10px] text-ok font-mono">+{group.totalAdditions}</span>
        )}
        {group.totalDeletions > 0 && (
          <span className="text-[10px] text-danger font-mono">-{group.totalDeletions}</span>
        )}
      </button>
      {open && diffHtml && (
        <div className="ace-diff border-t border-border" dangerouslySetInnerHTML={{ __html: diffHtml }} />
      )}
    </div>
  );
}

// ────────────────────────────────────────────────────────────
// 预览 tab — 文件原文 + hljs 高亮
// ────────────────────────────────────────────────────────────
function PreviewPanel({ api, cwd, path }) {
  const [state, setState] = useState({ status: 'idle', text: '', error: null, lang: '', size: 0 });

  useEffect(() => {
    if (!cwd || !path) {
      setState({ status: 'idle', text: '', error: null, lang: '', size: 0 });
      return;
    }
    let cancelled = false;
    setState({ status: 'loading', text: '', error: null, lang: '', size: 0 });
    api.readFile(cwd, path).then((text) => {
      if (cancelled) return;
      setState({ status: 'ok', text, error: null, lang: langForFile(path), size: text.length });
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
      setState({ status: 'error', text: '', error: msg, lang: '', size: extraSize });
    });
    return () => { cancelled = true; };
  }, [api, cwd, path]);

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
  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      <div className="px-2 py-1 text-[10px] text-fg-mute font-mono border-b border-border truncate" title={path}>
        {path} · {formatBytes(state.text.length)}{lang ? ` · ${lang}` : ''}
      </div>
      <CopyableCodeFrame text={state.text} className="flex-1 min-h-0 ace-side-preview-code">
        <div
          className="h-full overflow-auto text-[11px] ace-preview"
          dangerouslySetInnerHTML={{ __html: html }}
        />
      </CopyableCodeFrame>
    </div>
  );
}

// ────────────────────────────────────────────────────────────
// 主组件
// ────────────────────────────────────────────────────────────
export function SidePanel({ sessionRef, sessionId, cwd, messages, width = 280, collapsed = false, onToggleCollapse }) {
  const api = useMemo(() => createApi(sessionRef || null), [sessionRef?.port, sessionRef?.token, sessionRef?.workspaceHash]);

  const [activeTab,    setActiveTab]    = useState('files');
  const [expandedDirs, setExpandedDirs] = useState(new Set());
  const [treeCache,    setTreeCache]    = useState(new Map());
  const [selectedPath, setSelectedPath] = useState(null);

  // cwd 变时整面板 reset(典型场景:desktop 切 workspace → daemon 重启 → 新 cwd)。
  // 切 session 但 cwd 不变(同 daemon 多 session)→ 文件树/选中/tab 全保留,
  // 这样用户切来切去不会重复看一遍同一棵 cwd 的文件树。
  const lastCwd = useRef('');
  useEffect(() => {
    const c = cwd || '';
    if (c === lastCwd.current) return;
    lastCwd.current = c;
    setActiveTab('files');
    setExpandedDirs(new Set());
    setTreeCache(new Map());
    setSelectedPath(null);
  }, [cwd]);

  const onPickFile = useCallback((entry) => {
    setSelectedPath(entry.path);
    setActiveTab('preview');
  }, []);

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
              onClick={() => setActiveTab(t.key)}
            >
              {t.label}
            </button>
          ))}
        </div>
        {onToggleCollapse && (
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
            expandedDirs={expandedDirs}
            setExpandedDirs={setExpandedDirs}
            treeCache={treeCache}
            setTreeCache={setTreeCache}
            selectedPath={selectedPath}
            onPickFile={onPickFile}
          />
        )}
        {activeTab === 'changes' && <ChangesList messages={messages} />}
        {activeTab === 'preview' && (
          <PreviewPanel api={api} cwd={cwd} path={selectedPath} />
        )}
      </div>
    </div>
  );
}
