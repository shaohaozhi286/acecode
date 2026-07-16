import { useEffect, useMemo, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import {
  normalizeProjectDirectoryName,
  projectCreationErrorMessage,
  projectPathPreview,
} from '../lib/projectCreation.js';

export function CreateProjectModal({ api, onClose, onCreated }) {
  const inputRef = useRef(null);
  const [name, setName] = useState('');
  const [defaultParentDir, setDefaultParentDir] = useState('');
  const [loadingDefaults, setLoadingDefaults] = useState(true);
  const [creating, setCreating] = useState(false);
  const [error, setError] = useState('');
  const normalizedName = useMemo(() => normalizeProjectDirectoryName(name), [name]);
  const busy = loadingDefaults || creating;

  useEffect(() => {
    const timer = window.setTimeout(() => inputRef.current?.focus(), 0);
    return () => window.clearTimeout(timer);
  }, []);

  useEffect(() => {
    let cancelled = false;
    setLoadingDefaults(true);
    api.getProjectDefaults()
      .then((result) => {
        if (cancelled) return;
        const next = String(result?.parent_dir || '');
        setDefaultParentDir(next);
      })
      .catch((requestError) => {
        if (!cancelled) setError(projectCreationErrorMessage(requestError));
      })
      .finally(() => {
        if (!cancelled) setLoadingDefaults(false);
      });
    return () => { cancelled = true; };
  }, [api]);

  const createProject = async (event, close) => {
    event.preventDefault();
    if (creating || !name.trim()) return;
    setCreating(true);
    setError('');
    try {
      const workspace = await api.createProject(name);
      await onCreated?.(workspace);
      close();
    } catch (requestError) {
      setError(projectCreationErrorMessage(requestError));
    } finally {
      setCreating(false);
    }
  };

  return (
    <Modal onClose={onClose} width={520} dismissOnBackdrop={!creating}>
      {({ close }) => (
        <form onSubmit={(event) => createProject(event, close)}>
          <div className="px-5 py-4 border-b border-border flex items-start gap-3">
            <div className="w-9 h-9 rounded-lg bg-accent/10 text-accent flex items-center justify-center shrink-0">
              <VsIcon name="folderAdd" size={19} />
            </div>
            <div className="min-w-0">
              <h2 className="text-[15px] font-semibold text-fg">新建项目</h2>
              <p className="mt-0.5 text-[12px] leading-5 text-fg-mute">
                创建一个空项目目录，并用于这次新对话。
              </p>
            </div>
          </div>

          <div className="px-5 py-4 space-y-4">
            <label className="block">
              <span className="block mb-1.5 text-[12px] font-medium text-fg">项目名称</span>
              <input
                ref={inputRef}
                value={name}
                onChange={(event) => {
                  setName(event.target.value);
                  setError('');
                }}
                maxLength={240}
                autoComplete="off"
                spellCheck={false}
                placeholder="例如：我的新项目"
                className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
                aria-describedby="create-project-name-preview"
              />
              {!!name.trim() && (
                <div id="create-project-name-preview" className="mt-1.5 text-[11px] leading-5 text-fg-mute">
                  目录名：<span className="font-mono text-fg">{normalizedName.directoryName}</span>
                  {normalizedName.changed && (
                    <span className="ml-2 text-accent">已自动兼容系统命名规则</span>
                  )}
                </div>
              )}
            </label>

            <div>
              <div className="mb-1.5 text-[12px] font-medium text-fg">默认位置</div>
              <div className="h-9 px-3 rounded-lg border border-border bg-bg flex items-center gap-2">
                <VsIcon name="folder" size={15} className="text-fg-mute shrink-0" />
                <div
                  className="min-w-0 flex-1 truncate font-mono text-[11.5px] text-fg-mute"
                  title={defaultParentDir || 'ACECode 全局项目目录'}
                >
                  {loadingDefaults ? '正在读取默认位置…' : (defaultParentDir || 'ACECode 全局项目目录')}
                </div>
              </div>
              {!!name.trim() && (
                <div
                  className="mt-1.5 truncate font-mono text-[11px] leading-5 text-fg-mute"
                  title={projectPathPreview(defaultParentDir, normalizedName.directoryName)}
                >
                  {projectPathPreview(defaultParentDir, normalizedName.directoryName)}
                </div>
              )}
            </div>

            {error && (
              <div role="alert" className="px-3 py-2 rounded-lg border border-danger/30 bg-surface text-[12px] leading-5 text-danger">
                {error}
              </div>
            )}
          </div>

          <div className="px-5 py-3 border-t border-border bg-surface-alt flex items-center justify-end gap-2">
            <button
              type="button"
              onClick={close}
              disabled={creating}
              className="h-8 px-3 rounded-lg text-[12px] text-fg hover:bg-surface-hi disabled:opacity-50"
            >
              取消
            </button>
            <button
              type="submit"
              disabled={busy || !name.trim()}
              className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 disabled:hover:brightness-100 flex items-center justify-center gap-1.5"
            >
              {creating && <span className="ace-spinner w-3 h-3" />}
              {creating ? '创建中…' : '创建项目'}
            </button>
          </div>
        </form>
      )}
    </Modal>
  );
}
