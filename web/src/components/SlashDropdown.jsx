// SlashDropdown:输入框敲 / 时弹出的命令补全浮层。
//
// 以 InputBar 为锚点智能选择上/下方,8 行理想可视区,空间不足时限高滚动。
// 键盘:↑/↓/PgUp/PgDn/Home/End/Enter/Tab/Esc;鼠标:hover 高亮 + click 等价 Enter。
// 选中后调 onSelect(item) — InputBar 决定是插入文本还是别的。

import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { rankCommands, slashCommandKindPresentation } from '../lib/slashCommands.js';
import {
  computeAnchoredDropdownLayout,
  DROPDOWN_GAP_PX,
} from '../lib/dropdownPlacement.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

const VISIBLE_ROWS = 8;
const ROW_HEIGHT = 36; // px
const MAX_LIST_HEIGHT = VISIBLE_ROWS * ROW_HEIGHT;

function sameLayout(left, right) {
  return left.placement === right.placement
    && left.constrained === right.constrained
    && Math.abs((left.maxHeight ?? 0) - (right.maxHeight ?? 0)) < 0.5;
}

export function SlashDropdown({ items, query, onSelect, onClose }) {
  const ranked = useMemo(() => rankCommands(query, items), [query, items]);
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [layout, setLayout] = useState({ placement: 'above', maxHeight: null });
  const [scrollMetrics, setScrollMetrics] = useState({
    scrollTop: 0,
    clientHeight: MAX_LIST_HEIGHT,
  });
  const popupRef = useRef(null);
  const listRef = useRef(null);
  const rowRefs = useRef(new Map());
  const indicatorHeightRef = useRef(0);

  const updateScrollMetrics = useCallback(() => {
    const list = listRef.current;
    if (!list) return;
    const next = {
      scrollTop: list.scrollTop,
      clientHeight: list.clientHeight,
    };
    setScrollMetrics((previous) => (
      Math.abs(previous.scrollTop - next.scrollTop) < 0.5
      && previous.clientHeight === next.clientHeight
        ? previous
        : next
    ));
  }, []);

  const measureLayout = useCallback(() => {
    const popup = popupRef.current;
    const list = listRef.current;
    const anchor = popup?.parentElement;
    if (!popup || !list || !anchor) return;

    const anchorRect = anchor.getBoundingClientRect();
    const visualViewport = window.visualViewport;
    const viewportTop = Number.isFinite(visualViewport?.offsetTop)
      ? visualViewport.offsetTop
      : 0;
    const viewportHeight = Number.isFinite(visualViewport?.height)
      ? visualViewport.height
      : window.innerHeight;
    const indicator = popup.querySelector('[data-scroll-indicator]');
    if (indicator) {
      indicatorHeightRef.current = Math.max(
        indicatorHeightRef.current,
        indicator.getBoundingClientRect().height,
      );
    }

    const borderHeight = Math.max(0, popup.offsetHeight - popup.clientHeight);
    const idealListHeight = Math.min(list.scrollHeight, MAX_LIST_HEIGHT);
    const idealIndicatorHeight = ranked.length > VISIBLE_ROWS
      ? indicatorHeightRef.current
      : 0;
    const preferredHeight = borderHeight + idealListHeight + idealIndicatorHeight;
    const next = computeAnchoredDropdownLayout({
      anchorTop: anchorRect.top,
      anchorBottom: anchorRect.bottom,
      viewportTop,
      viewportHeight,
      preferredHeight,
    });

    setLayout((previous) => (sameLayout(previous, next) ? previous : next));
    updateScrollMetrics();
  }, [ranked.length, updateScrollMetrics]);

  // 列表变化时把 selectedIndex 钉到 0(避免越界 / 滑出范围)。
  useEffect(() => { setSelectedIndex(0); }, [ranked.length, query]);

  // 选中行滚入可视区。
  useEffect(() => {
    const row = rowRefs.current.get(selectedIndex);
    if (row) row.scrollIntoView({ block: 'nearest' });
  }, [selectedIndex]);

  // 键盘事件挂在 window 的捕获阶段,确保不被 textarea 默认行为吃掉(尤其是 Enter)。
  // InputBar 通过 prop `onClose` 控制何时取消挂载,这里组件 unmount 自动解绑。
  const onKey = useCallback((event) => {
    const total = ranked.length;
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
      return;
    }
    if (event.key === 'Enter' || event.key === 'Tab') {
      if (total === 0) return;
      event.preventDefault();
      event.stopPropagation();
      onSelect?.(ranked[selectedIndex]);
      return;
    }
    if (total === 0) return;
    let next = selectedIndex;
    if (event.key === 'ArrowDown')      next = Math.min(total - 1, selectedIndex + 1);
    else if (event.key === 'ArrowUp')   next = Math.max(0, selectedIndex - 1);
    else if (event.key === 'PageDown')  next = Math.min(total - 1, selectedIndex + VISIBLE_ROWS);
    else if (event.key === 'PageUp')    next = Math.max(0, selectedIndex - VISIBLE_ROWS);
    else if (event.key === 'Home')      next = 0;
    else if (event.key === 'End')       next = total - 1;
    else return;
    event.preventDefault();
    event.stopPropagation();
    setSelectedIndex(next);
  }, [ranked, selectedIndex, onSelect, onClose]);

  useEffect(() => {
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [onKey]);

  useLayoutEffect(() => {
    measureLayout();

    const visualViewport = window.visualViewport;
    window.addEventListener('resize', measureLayout);
    window.addEventListener('scroll', measureLayout, true);
    visualViewport?.addEventListener?.('resize', measureLayout);
    visualViewport?.addEventListener?.('scroll', measureLayout);

    let resizeObserver = null;
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(measureLayout);
      const anchor = popupRef.current?.parentElement;
      if (anchor) resizeObserver.observe(anchor);
      if (listRef.current) resizeObserver.observe(listRef.current);
    }

    return () => {
      window.removeEventListener('resize', measureLayout);
      window.removeEventListener('scroll', measureLayout, true);
      visualViewport?.removeEventListener?.('resize', measureLayout);
      visualViewport?.removeEventListener?.('scroll', measureLayout);
      resizeObserver?.disconnect();
    };
  }, [measureLayout]);

  if (!items || items.length === 0) {
    // 命令清单还没加载,什么都不渲染(InputBar 应该等 commands 非空再挂载本组件)
    return null;
  }

  // 滚动指示
  const aboveCount = Math.max(0, Math.floor(scrollMetrics.scrollTop / ROW_HEIGHT));
  const visibleEnd = scrollMetrics.clientHeight > 0
    ? Math.ceil((scrollMetrics.scrollTop + scrollMetrics.clientHeight) / ROW_HEIGHT)
    : aboveCount;
  const belowCount = Math.max(0, ranked.length - visibleEnd);
  const opensBelow = layout.placement === 'below';

  return (
    <div
      ref={popupRef}
      role="listbox"
      aria-label="斜杠命令"
      data-placement={layout.placement}
      data-constrained={layout.constrained ? 'true' : 'false'}
      className="absolute left-0 right-0 flex flex-col bg-surface border border-border rounded-md ace-shadow-lg overflow-hidden font-sans"
      style={{
        zIndex: 60,
        top: opensBelow ? `calc(100% + ${DROPDOWN_GAP_PX}px)` : 'auto',
        bottom: opensBelow ? 'auto' : `calc(100% + ${DROPDOWN_GAP_PX}px)`,
        maxHeight: layout.maxHeight == null ? undefined : `${layout.maxHeight}px`,
      }}
      onMouseDown={(e) => e.preventDefault()} // 阻止失焦,让 textarea 仍持焦点
    >
      {aboveCount > 0 && (
        <div
          data-scroll-indicator="above"
          className="shrink-0 px-3 py-1 text-[11px] text-fg-mute bg-surface-alt border-b border-border"
        >
          ↑ {aboveCount} more above
        </div>
      )}
      <div
        ref={listRef}
        className="min-h-0 flex-1 overflow-y-auto"
        style={{ maxHeight: MAX_LIST_HEIGHT }}
        onScroll={updateScrollMetrics}
      >
        {ranked.length === 0 ? (
          <div className="px-3 py-3 text-center text-fg-mute text-[12px]">无匹配命令</div>
        ) : ranked.map((it, idx) => {
          const selected = idx === selectedIndex;
          const presentation = slashCommandKindPresentation(it);
          return (
            <div
              key={it.kind + ':' + it.name}
              ref={(el) => { if (el) rowRefs.current.set(idx, el); else rowRefs.current.delete(idx); }}
              role="option"
              aria-selected={selected}
              aria-label={`${presentation.label} ${it.name}${it.description ? ' ' + it.description : ''}`}
              data-command-kind={it.kind}
              onMouseEnter={() => setSelectedIndex(idx)}
              onMouseDown={(e) => { e.preventDefault(); onSelect?.(it); }}
              className={clsx(
                'flex items-center gap-2 px-3 cursor-pointer text-[13px] tracking-normal',
                selected ? 'bg-surface-hi text-fg' : 'text-fg hover:bg-surface-hi/60',
              )}
              style={{ height: ROW_HEIGHT }}
            >
              <span
                className="inline-flex h-5 w-5 shrink-0 items-center justify-center rounded text-accent"
                title={presentation.label}
                aria-hidden="true"
                style={{ transform: 'translateY(2px)' }}
              >
                <VsIcon name={presentation.icon} size={14} />
              </span>
              <span className="min-w-0 max-w-[220px] shrink-0 truncate text-accent font-medium text-[13px]">{it.name}</span>
              <span className="min-w-0 flex-1 truncate text-fg-mute text-[13px]">{it.description}</span>
            </div>
          );
        })}
      </div>
      {belowCount > 0 && (
        <div
          data-scroll-indicator="below"
          className="shrink-0 px-3 py-1 text-[11px] text-fg-mute bg-surface-alt border-t border-border"
        >
          ↓ {belowCount} more below
        </div>
      )}
    </div>
  );
}
