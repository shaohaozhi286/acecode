// 全局会话搜索面板:Ctrl/Cmd+K 触发,跨所有 workspace 列出 session,前端 fuzzy 过滤。
//
// 数据 60s TTL 缓存,WS session_status 事件命中 invalidate。键盘导航
// (↑/↓/PgUp/PgDn/Home/End/Enter/Esc)优先级最高,绑在根节点的 keydown 上。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import {
  mergeSessionContentMatches,
  rankSessions,
  searchRelativeTime,
  shouldSearchUserMessages,
} from '../lib/searchSessions.js';
import { sessionDisplayTitle, withNewSessionDisplayTitles } from '../lib/sessionTitle.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

const CACHE_TTL_MS = 60_000;
const PAGE_SIZE = 8;
const MAX_EMPTY_RESULTS = 50;

// 组件外缓存,跨 open/close 共享(关闭再打开 60s 内不再 fetch)。
const cache = {
  ts: 0,
  data: { sessions: [], errors: [] },
};

function isCacheFresh(now = Date.now()) {
  return cache.ts > 0 && now - cache.ts < CACHE_TTL_MS;
}

function invalidateCache() {
  cache.ts = 0;
}

function searchMatchContext(match) {
  if (!match || match.kind !== 'user_message') return '';
  const attachments = Array.isArray(match.attachments) ? match.attachments.filter(Boolean) : [];
  if (attachments.length > 0) return `附件: ${attachments.slice(0, 2).join(', ')}`;
  return String(match.snippet || '');
}

export function SearchPalette({ open, onClose, currentWorkspaceHash = '', onSelectSession }) {
  const [query, setQuery] = useState('');
  const [loadState, setLoadState] = useState('idle'); // 'idle' | 'loading' | 'ready'
  const [data, setData] = useState(cache.data);
  const [contentSearch, setContentSearch] = useState({ query: '', matches: [] });
  const [selectedIndex, setSelectedIndex] = useState(0);
  const inputRef = useRef(null);
  const listRef = useRef(null);
  const rowRefs = useRef(new Map());
  const reqIdRef = useRef(0);
  const contentReqIdRef = useRef(0);

  // WS 事件 invalidate 缓存(关闭面板时也监听,确保下次打开拿新数据)。
  useEffect(() => {
    const onMsg = (e) => {
      const t = e.detail?.type;
      if (t === 'session_status' || t === 'session_status_snapshot' || t === 'mark_session_read_ack') {
        invalidateCache();
      }
    };
    connection.addEventListener('message', onMsg);
    return () => connection.removeEventListener('message', onMsg);
  }, []);

  // 打开时:reset query/selection,fetch(若缓存过期)。
  useEffect(() => {
    if (!open) return;
    setQuery('');
    setContentSearch({ query: '', matches: [] });
    setSelectedIndex(0);
    requestAnimationFrame(() => inputRef.current?.focus());

    if (isCacheFresh()) {
      setData(cache.data);
      setLoadState('ready');
      return;
    }
    const reqId = ++reqIdRef.current;
    setLoadState('loading');
    api.listAllWorkspaceSessions().then((result) => {
      if (reqId !== reqIdRef.current) return; // 过期请求丢弃
      cache.ts = Date.now();
      cache.data = result;
      setData(result);
      setLoadState('ready');
    }).catch(() => {
      if (reqId !== reqIdRef.current) return;
      setLoadState('ready');
    });
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const q = query.trim();
    if (!shouldSearchUserMessages(q)) {
      contentReqIdRef.current++;
      setContentSearch({ query: '', matches: [] });
      return;
    }
    const reqId = ++contentReqIdRef.current;
    const timer = setTimeout(() => {
      api.searchSessionUserMessages(q, 50).then((result) => {
        if (reqId !== contentReqIdRef.current) return;
        const matches = Array.isArray(result?.matches) ? result.matches : [];
        setContentSearch({ query: q, matches });
      }).catch(() => {
        if (reqId !== contentReqIdRef.current) return;
        setContentSearch({ query: q, matches: [] });
      });
    }, 160);
    return () => clearTimeout(timer);
  }, [open, query]);

  // 排序后的可见列表;空查询时取最近 50 条。
  const items = useMemo(() => {
    const baseSessions = withNewSessionDisplayTitles(data.sessions || []);
    const q = query.trim();
    const matches = shouldSearchUserMessages(q) && contentSearch.query === q
      ? contentSearch.matches
      : [];
    const merged = mergeSessionContentMatches(baseSessions, matches);
    const ranked = rankSessions(merged, query, Date.now());
    return query.trim() ? ranked : ranked.slice(0, MAX_EMPTY_RESULTS);
  }, [data, query, contentSearch]);

  // query / items 变化时把 selectedIndex 钉到 0(避免滑出范围)。
  useEffect(() => {
    setSelectedIndex(0);
  }, [query, data]);

  // 选中项滚动到可视区。
  useEffect(() => {
    const row = rowRefs.current.get(selectedIndex);
    if (row) row.scrollIntoView({ block: 'nearest' });
  }, [selectedIndex, items.length]);

  const commit = useCallback((index) => {
    const it = items[index];
    if (!it) return;
    onSelectSession?.(it);
  }, [items, onSelectSession]);

  const onRootKeyDown = useCallback((event) => {
    const total = items.length;
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
      return;
    }
    if (event.key === 'Enter') {
      if (total === 0) return;
      event.preventDefault();
      event.stopPropagation();
      commit(selectedIndex);
      return;
    }
    if (total === 0) return;
    let next = selectedIndex;
    if (event.key === 'ArrowDown')      next = Math.min(total - 1, selectedIndex + 1);
    else if (event.key === 'ArrowUp')   next = Math.max(0, selectedIndex - 1);
    else if (event.key === 'PageDown')  next = Math.min(total - 1, selectedIndex + PAGE_SIZE);
    else if (event.key === 'PageUp')    next = Math.max(0, selectedIndex - PAGE_SIZE);
    else if (event.key === 'Home')      next = 0;
    else if (event.key === 'End')       next = total - 1;
    else return;
    event.preventDefault();
    event.stopPropagation();
    setSelectedIndex(next);
  }, [items, selectedIndex, commit, onClose]);

  if (!open) return null;

  return (
    <div
      className="fixed inset-0 z-[300] flex items-start justify-center pt-[15vh] px-4"
      onKeyDown={onRootKeyDown}
      onMouseDown={(e) => { if (e.target === e.currentTarget) onClose?.(); }}
      style={{ background: 'rgba(var(--ace-bg-rgb), 0.50)' }}
    >
      <div
        className="bg-surface border border-border rounded-xl ace-shadow-lg overflow-hidden flex flex-col"
        style={{ width: 'min(640px, 90vw)', maxHeight: '70vh' }}
        onMouseDown={(e) => e.stopPropagation()}
      >
        {/* 顶部输入栏 */}
        <div className="h-12 px-3 flex items-center gap-2 border-b border-border shrink-0">
          <VsIcon name="search" size={14} className="text-fg-mute shrink-0" />
          <input
            ref={inputRef}
            type="text"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="搜索会话与 workspace"
            className="flex-1 bg-transparent border-0 outline-none text-[14px] text-fg placeholder:text-fg-mute"
          />
          <button
            type="button"
            onClick={onClose}
            title="关闭 (Esc)"
            className="w-6 h-6 rounded-md text-fg-mute hover:text-fg hover:bg-surface-hi flex items-center justify-center"
          >
            <VsIcon name="close" size={12} />
          </button>
        </div>

        {/* 错误条:某 workspace 加载失败 */}
        {loadState === 'ready' && data.errors?.length > 0 && (
          <div className="px-3 py-1.5 text-[11px] text-warning bg-warning-soft/30 border-b border-border shrink-0">
            部分 workspace 加载失败:{data.errors.map((e) => e.name || e.hash).join(',')}
          </div>
        )}

        {/* 列表区 */}
        <div ref={listRef} className="flex-1 overflow-y-auto">
          {loadState === 'loading' && (
            <div className="px-4 py-8 text-center text-fg-mute text-[13px]">加载中…</div>
          )}
          {loadState === 'ready' && items.length === 0 && (
            <div className="px-4 py-8 text-center text-fg-mute text-[13px]">
              {query.trim() ? '无匹配结果' : '暂无会话'}
            </div>
          )}
          {loadState === 'ready' && items.map((s, idx) => {
            const selected = idx === selectedIndex;
            const showWsName = (s.workspace_hash || '') !== currentWorkspaceHash;
            const rel = searchRelativeTime(s.updated_at || s.created_at);
            const matchContext = searchMatchContext(s.search_match);
            const right = [
              selected && 'Enter',
              showWsName && (s.workspaceName || ''),
              rel,
            ].filter(Boolean).join(' · ');
            return (
              <div
                key={s.id}
                ref={(el) => { if (el) rowRefs.current.set(idx, el); else rowRefs.current.delete(idx); }}
                role="option"
                aria-selected={selected}
                onMouseEnter={() => setSelectedIndex(idx)}
                onMouseDown={(e) => { e.preventDefault(); commit(idx); }}
                className={clsx(
                  'min-h-12 px-3 py-2 flex items-center gap-3 cursor-pointer text-[13px]',
                  selected ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
                )}
              >
                <VsIcon name="code" size={16} className="text-fg-mute shrink-0" />
                <span className="min-w-0 flex-1 flex flex-col gap-0.5">
                  <span className="truncate">{sessionDisplayTitle(s)}</span>
                  {matchContext ? (
                    <span className="truncate text-[11px] text-fg-mute">{matchContext}</span>
                  ) : null}
                </span>
                <span className="text-[12px] text-fg-mute shrink-0">{right}</span>
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
