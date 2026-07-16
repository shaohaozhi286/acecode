// Git 详情的数据适配层。列表/patch 继续走 Git API 与共享缓存，所有可见详情
// DOM、展开折叠、滚动、单双栏 Diff 和右键动作统一交给 ChangeReviewDetails。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { buildChangeRow, buildSummaryLabel } from '../lib/gitChanges.js';
import { changesCache } from '../lib/gitChangesCache.js';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';
import { ChangeReviewDetails } from './ChangeReviewDetails.jsx';

export function GitChangeDetails({
  api,
  cwd = '',
  base = '',
  expandedFile = '',
  expandedFileRevision = 0,
  busy = false,
  onSelectFile,
  onOpenFilePreview,
}) {
  const [list, setList] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [patches, setPatches] = useState({});
  const cwdRef = useRef(cwd);
  const baseRef = useRef(base);
  const patchesRef = useRef(patches);
  const patchRequestTokensRef = useRef(new Map());
  cwdRef.current = cwd;
  baseRef.current = base;
  patchesRef.current = patches;

  const replacePatches = useCallback((next) => {
    patchesRef.current = next;
    setPatches(next);
  }, []);

  const resetPatches = useCallback(() => {
    patchRequestTokensRef.current.clear();
    replacePatches({});
  }, [replacePatches]);

  const fetchList = useCallback((force = false) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!targetCwd || !targetBase) return;
    if (!force) {
      const cached = changesCache.getList(targetCwd, targetBase);
      if (cached) {
        setList(cached);
        setError('');
        setLoading(false);
        return;
      }
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
      .catch((requestError) => {
        if (cwdRef.current !== targetCwd || baseRef.current !== targetBase) return;
        setError(
          requestError?.status === 504
            ? 'timeout'
            : (requestError?.body?.error || requestError?.message || 'error'),
        );
      })
      .finally(() => {
        if (cwdRef.current === targetCwd && baseRef.current === targetBase) setLoading(false);
      });
  }, [api]);

  useEffect(() => {
    setList(null);
    setError('');
    resetPatches();
    fetchList(false);
  }, [base, cwd, fetchList, resetPatches]);

  const previousBusyRef = useRef(busy);
  useEffect(() => {
    const wasBusy = previousBusyRef.current;
    previousBusyRef.current = busy;
    if (!wasBusy || busy || !cwdRef.current) return;
    changesCache.markStale(cwdRef.current);
    resetPatches();
    fetchList(true);
  }, [busy, fetchList, resetPatches]);

  useEffect(() => {
    const handler = (event) => {
      const changedCwd = event?.detail?.cwd || '';
      if (changedCwd && changedCwd !== cwdRef.current) return;
      changesCache.markStale(cwdRef.current);
      resetPatches();
      fetchList(true);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [fetchList, resetPatches]);

  const loadPatch = useCallback((path, force = false) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!path || !targetCwd || !targetBase) return;
    if (!force && patchesRef.current[path] !== undefined) return;

    if (!force) {
      const cached = changesCache.getPatch(targetCwd, targetBase, path);
      if (cached != null) {
        replacePatches({
          ...patchesRef.current,
          [path]: { text: cached },
        });
        return;
      }
    }

    const requestToken = Symbol(path);
    patchRequestTokensRef.current.set(path, requestToken);
    replacePatches({
      ...patchesRef.current,
      [path]: {},
    });
    api.gitFileDiff(targetCwd, path, targetBase)
      .then((result) => {
        if (
          cwdRef.current !== targetCwd
          || baseRef.current !== targetBase
          || patchRequestTokensRef.current.get(path) !== requestToken
        ) return;
        const patch = result?.patch || '';
        changesCache.putPatch(targetCwd, targetBase, path, patch);
        replacePatches({
          ...patchesRef.current,
          [path]: { text: patch },
        });
      })
      .catch((requestError) => {
        if (
          cwdRef.current !== targetCwd
          || baseRef.current !== targetBase
          || patchRequestTokensRef.current.get(path) !== requestToken
        ) return;
        const message = requestError?.status === 413
          ? 'diff 过大,请在终端查看'
          : requestError?.status === 504
            ? 'git 超时,点击重试'
            : '加载 diff 失败';
        replacePatches({
          ...patchesRef.current,
          [path]: { error: message },
        });
      });
  }, [api, replacePatches]);

  const fetchPatchText = useCallback((path) => {
    const targetCwd = cwdRef.current;
    const targetBase = baseRef.current;
    if (!path || !targetCwd || !targetBase) return Promise.resolve('');
    const cached = changesCache.getPatch(targetCwd, targetBase, path);
    if (cached != null) return Promise.resolve(cached);
    return api.gitFileDiff(targetCwd, path, targetBase)
      .then((result) => {
        const patch = result?.patch || '';
        changesCache.putPatch(targetCwd, targetBase, path, patch);
        return patch;
      })
      .catch(() => '');
  }, [api]);

  const rows = useMemo(() => (list?.files || []).map((file) => {
    const row = buildChangeRow(file);
    const patch = patches[row.path];
    const diff = patch?.error
      ? { state: 'error', message: patch.error }
      : patch && Object.prototype.hasOwnProperty.call(patch, 'text')
        ? { state: 'ready', text: patch.text }
        : { state: 'loading' };
    return {
      ...row,
      additions: typeof file.additions === 'number' && file.additions >= 0
        ? file.additions
        : null,
      deletions: typeof file.deletions === 'number' && file.deletions >= 0
        ? file.deletions
        : null,
      diff,
    };
  }), [list, patches]);

  const getAllDiffText = useCallback(
    () => Promise.all(rows.map((row) => fetchPatchText(row.path)))
      .then((texts) => texts.filter(Boolean).join('\n\n')),
    [fetchPatchText, rows],
  );

  const errorMessage = error === 'timeout'
    ? 'git 响应超时(仓库可能很大)'
    : error
      ? `加载失败:${error}`
      : '';

  return (
    <ChangeReviewDetails
      key={`${cwd}\u0000${base}`}
      rows={rows}
      ready={!!list}
      loading={loading}
      summaryLabel={buildSummaryLabel(list)}
      fileCount={list?.total_count ?? rows.length}
      totalAdditions={list?.total_additions ?? 0}
      totalDeletions={list?.total_deletions ?? 0}
      cwd={cwd}
      initialExpandedFile={expandedFile}
      initialExpandedFileRevision={expandedFileRevision}
      selectedFile={expandedFile}
      errorMessage={errorMessage}
      emptyMessage={`工作区相对 ${base} 无变更`}
      onRetryList={() => fetchList(true)}
      onSelectFile={onSelectFile}
      onOpenFile={onOpenFilePreview}
      onEnsureDiff={loadPatch}
      getFileDiffText={fetchPatchText}
      getAllDiffText={getAllDiffText}
      contentRevision={patches}
      ensureDiffRevision={patches}
    />
  );
}
