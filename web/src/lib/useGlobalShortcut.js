// 全局快捷键 hook:在 window 上监听 keydown,匹配函数命中即调 handler 并 preventDefault。
// 对所有焦点位置生效(包括 textarea / contenteditable),用于 Ctrl/Cmd+K 这种约定俗成的快捷键。

import { useEffect } from 'react';

// 纯函数:判断 KeyboardEvent 是否匹配 spec。spec 形如 {key: 'k', ctrl: true, meta: true}。
// ctrl/meta 为 true 表示"该修饰键之一按下即可"(Mac 用 Cmd,Windows 用 Ctrl 都接受)。
export function matchShortcut(event, spec) {
  if (!event || !spec) return false;
  const expectedKey = (spec.key || '').toLowerCase();
  if (typeof event.key !== 'string') return false;
  if (event.key.toLowerCase() !== expectedKey) return false;
  if (spec.ctrl || spec.meta) {
    if (!event.ctrlKey && !event.metaKey) return false;
  }
  if (spec.shift && !event.shiftKey) return false;
  if (spec.alt && !event.altKey) return false;
  return true;
}

export function useGlobalShortcut(matcher, handler, deps = []) {
  useEffect(() => {
    if (typeof window === 'undefined') return undefined;
    const onKey = (event) => {
      const hit = typeof matcher === 'function' ? matcher(event) : matchShortcut(event, matcher);
      if (!hit) return;
      event.preventDefault();
      handler?.(event);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
}
