// WindowControls:frameless desktop 模式下的标题栏窗口控制三连
// (minimize / maximize ↔ restore / close)。从 TopBar 抽出,SettingsPage 等
// 全屏覆盖 TopBar 的页面也复用,避免每处重写。
//
// 用法:
//   const { isMaximized, framelessDesktop } = useFramelessWindowState();
//   {framelessDesktop && (
//     <WindowControls isMaximized={isMaximized} />
//   )}

import { useEffect, useState } from 'react';
import { clsx } from '../lib/format.js';

export function isFramelessDesktop() {
  return typeof window !== 'undefined'
    && window.__ACECODE_FRAMELESS_WINDOW__ === true
    && typeof window.aceDesktop_startWindowDrag === 'function';
}

export function isInteractiveTarget(target) {
  return !!target?.closest?.('button,a,input,textarea,select,[role="button"],[data-ace-no-window-drag="true"]');
}

export function nativePointerEvent(event) {
  return {
    button: (event?.button ?? 0) + 1,
    screenX: event?.screenX ?? 0,
    screenY: event?.screenY ?? 0,
    time: Math.max(0, Math.floor(event?.timeStamp ?? 0)),
  };
}

// 监听 native 推送的 maximize 状态。多个组件可同时挂(WndProc 每次都遍历 webview
// 找全局回调,这里覆盖式注册 — 期望同一时刻只有一个 SettingsPage 或 TopBar 在
// 监听;切走时 cleanup 还原 null)。
export function useFramelessWindowState() {
  const framelessDesktop = isFramelessDesktop();
  const [isMaximized, setIsMaximized] = useState(false);

  useEffect(() => {
    if (!framelessDesktop) return undefined;
    let cancelled = false;
    const fetchInitial = async () => {
      if (typeof window.aceDesktop_isWindowMaximized !== 'function') return;
      try {
        const raw = await window.aceDesktop_isWindowMaximized();
        const r = typeof raw === 'string' ? JSON.parse(raw) : raw;
        if (!cancelled) setIsMaximized(!!(r && r.maximized));
      } catch {
        // bind 偶发抛错 → 保留默认 false
      }
    };
    fetchInitial();

    const prev = window.aceDesktop_onMaximizeStateChanged;
    window.aceDesktop_onMaximizeStateChanged = (m) => {
      setIsMaximized(!!m);
      // 也通知前一个监听者(链式),避免 TopBar 与 SettingsPage 共存时丢事件
      if (typeof prev === 'function') prev(m);
    };

    return () => {
      cancelled = true;
      // 还原前一个回调,而不是 delete — 防止 SettingsPage 关闭后 TopBar 失联
      window.aceDesktop_onMaximizeStateChanged = prev || undefined;
    };
  }, [framelessDesktop]);

  return { framelessDesktop, isMaximized };
}

function WindowGlyph({ type }) {
  if (type === 'minimize') {
    return (
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <path d="M3.5 10.5h9" />
      </svg>
    );
  }
  if (type === 'maximize') {
    return (
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <rect x="4.25" y="4.25" width="7.5" height="7.5" rx="1" />
      </svg>
    );
  }
  if (type === 'restore') {
    // Win11 style: back box as an L shape, front box complete.
    return (
      <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <path d="M5.75 6.25V3.25h6.5v6.5H10.25" />
        <rect x="3.75" y="6.25" width="6.5" height="6.5" rx="1" />
      </svg>
    );
  }
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true" focusable="false">
      <path d="M4.25 4.25l7.5 7.5M11.75 4.25l-7.5 7.5" />
    </svg>
  );
}

export function WindowControl({ type, title, onClick }) {
  return (
    <button
      type="button"
      data-ace-no-window-drag="true"
      title={title}
      aria-label={title}
      onClick={onClick}
      className={clsx('ace-window-control', type === 'close' && 'ace-window-control-close')}
    >
      <WindowGlyph type={type} />
    </button>
  );
}

// 三连组合 — 用得多就直接用这个,不用三连各自写一遍。
export function WindowControls({ isMaximized }) {
  return (
    <div className="ace-window-controls" data-ace-no-window-drag="true">
      <WindowControl type="minimize" title="最小化" onClick={() => window.aceDesktop_minimizeWindow?.()} />
      <WindowControl
        type={isMaximized ? 'restore' : 'maximize'}
        title={isMaximized ? '还原' : '最大化'}
        onClick={() => window.aceDesktop_toggleMaximizeWindow?.()}
      />
      <WindowControl type="close" title="关闭" onClick={() => window.aceDesktop_closeWindow?.()} />
    </div>
  );
}
