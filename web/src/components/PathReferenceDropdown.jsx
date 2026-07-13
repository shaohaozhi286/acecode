import { useCallback, useEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';

const VISIBLE_ROWS = 9;
const ROW_HEIGHT = 36;

export function PathReferenceDropdown({
  items = [],
  loading = false,
  error = '',
  onReference,
  onEnterDirectory,
  onClose,
}) {
  const [selected, setSelected] = useState(0);
  const rowRefs = useRef(new Map());

  useEffect(() => { setSelected(0); }, [items]);
  useEffect(() => {
    rowRefs.current.get(selected)?.scrollIntoView?.({ block: 'nearest' });
  }, [selected]);

  const onKey = useCallback((event) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
      return;
    }
    const item = items[selected];
    if (event.key === 'Enter') {
      event.preventDefault();
      event.stopPropagation();
      if (item) onReference?.(item);
      return;
    }
    if (event.key === 'ArrowRight' || event.key === 'Tab') {
      if (!item || item.kind !== 'dir') return;
      event.preventDefault();
      event.stopPropagation();
      onEnterDirectory?.(item);
      return;
    }
    if (items.length === 0) return;
    let next = selected;
    if (event.key === 'ArrowDown') next = Math.min(items.length - 1, selected + 1);
    else if (event.key === 'ArrowUp') next = Math.max(0, selected - 1);
    else if (event.key === 'PageDown') next = Math.min(items.length - 1, selected + VISIBLE_ROWS);
    else if (event.key === 'PageUp') next = Math.max(0, selected - VISIBLE_ROWS);
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = items.length - 1;
    else return;
    event.preventDefault();
    event.stopPropagation();
    setSelected(next);
  }, [items, onClose, onEnterDirectory, onReference, selected]);

  useEffect(() => {
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [onKey]);

  return (
    <div
      role="listbox"
      aria-label="文件和文件夹引用"
      className="absolute bottom-full left-0 w-[400px] max-w-full mb-2 bg-surface border border-border rounded-lg ace-shadow-lg overflow-hidden font-sans"
      style={{ zIndex: 62 }}
      onMouseDown={(event) => event.preventDefault()}
    >
      <div className="overflow-y-auto" style={{ maxHeight: VISIBLE_ROWS * ROW_HEIGHT }}>
        {loading && items.length === 0 ? (
          <div className="px-3 py-3 text-center text-fg-mute text-[12px]">正在读取当前目录…</div>
        ) : error ? (
          <div className="px-3 py-3 text-center text-danger text-[12px]">{error}</div>
        ) : items.length === 0 ? (
          <div className="px-3 py-3 text-center text-fg-mute text-[12px]">没有匹配的文件或文件夹</div>
        ) : items.map((item, index) => {
          const isDirectory = item.kind === 'dir';
          const active = index === selected;
          return (
            <div
              key={`${item.kind}:${item.path}`}
              ref={(element) => {
                if (element) rowRefs.current.set(index, element);
                else rowRefs.current.delete(index);
              }}
              role="option"
              aria-selected={active}
              aria-label={`${isDirectory ? '文件夹' : '文件'} ${item.path}`}
              className={clsx(
                'flex items-center gap-2 px-2 text-[13px]',
                active ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
              )}
              style={{ height: ROW_HEIGHT }}
              onMouseEnter={() => setSelected(index)}
            >
              <button
                type="button"
                className="min-w-0 flex-1 h-full flex items-center gap-2 text-left"
                onMouseDown={(event) => { event.preventDefault(); onReference?.(item); }}
                title={isDirectory ? '引用此文件夹' : '引用此文件'}
              >
                {isDirectory ? <VsIcon name="folder" size={14} /> : <FileTypeIcon path={item.path} size={14} />}
                <span className="truncate">{item.path}{isDirectory ? '/' : ''}</span>
              </button>
              {isDirectory && (
                <button
                  type="button"
                  className="shrink-0 h-6 px-2 rounded text-[11px] text-fg-mute hover:bg-bg hover:text-fg"
                  aria-label={`进入文件夹 ${item.path}`}
                  onMouseDown={(event) => { event.preventDefault(); event.stopPropagation(); onEnterDirectory?.(item); }}
                >
                  进入
                </button>
              )}
            </div>
          );
        })}
      </div>
      <div className="px-2 py-1 border-t border-border text-[10px] text-fg-mute bg-surface-alt">
        Enter 引用 · Right/Tab 进入文件夹 · Esc 关闭
      </div>
    </div>
  );
}
