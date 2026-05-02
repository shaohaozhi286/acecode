// 共享 modal 容器:遮罩 + 居中 + esc 关闭 + 进入/退出动画。
// 不依赖 bootstrap modal,纯 Tailwind + 内联状态。

import { useEffect, useState } from 'react';
import { clsx } from '../lib/format.js';

export function Modal({ children, onClose, width = 460, dismissOnBackdrop = true }) {
  const [show, setShow] = useState(false);

  useEffect(() => {
    requestAnimationFrame(() => setShow(true));
    const onKey = (e) => { if (e.key === 'Escape') handleClose(); };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const handleClose = () => {
    setShow(false);
    setTimeout(() => onClose?.(), 200);
  };

  return (
    <div
      className={clsx(
        'fixed inset-0 z-[200] flex items-center justify-center p-4 transition-colors duration-200',
        show ? 'bg-black/35' : 'bg-black/0',
      )}
      onClick={() => dismissOnBackdrop && handleClose()}
    >
      <div
        className={clsx(
          'bg-surface border border-border rounded-xl ace-shadow-lg overflow-hidden transition-all duration-200',
          show ? 'opacity-100 scale-100 translate-y-0' : 'opacity-0 scale-95 translate-y-2',
        )}
        style={{ width }}
        onClick={(e) => e.stopPropagation()}
      >
        {typeof children === 'function' ? children({ close: handleClose }) : children}
      </div>
    </div>
  );
}

// 右侧滑出面板(SkillsPanel / MCPPanel 用)
export function SlideOver({ children, onClose, width = 380 }) {
  const [show, setShow] = useState(false);

  useEffect(() => {
    requestAnimationFrame(() => setShow(true));
    const onKey = (e) => { if (e.key === 'Escape') handleClose(); };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const handleClose = () => {
    setShow(false);
    setTimeout(() => onClose?.(), 240);
  };

  return (
    <div
      className={clsx(
        'fixed inset-0 z-[250] transition-colors duration-250',
        show ? 'bg-black/25' : 'bg-black/0',
      )}
      onClick={handleClose}
    >
      <div
        className={clsx(
          'absolute top-0 right-0 bottom-0 bg-surface border-l border-border ace-shadow-lg flex flex-col transition-transform duration-250 ease-out',
          show ? 'translate-x-0' : 'translate-x-full',
        )}
        style={{ width }}
        onClick={(e) => e.stopPropagation()}
      >
        {typeof children === 'function' ? children({ close: handleClose }) : children}
      </div>
    </div>
  );
}

// Toggle switch — Modal/Panels 复用
export function Toggle({ on, onChange, disabled }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      disabled={disabled}
      onClick={() => onChange?.(!on)}
      className={clsx(
        'w-9 h-5 rounded-full relative transition-colors shrink-0 disabled:opacity-50',
        on ? 'bg-accent border border-accent' : 'bg-surface-hi border border-border',
      )}
    >
      <span
        className={clsx(
          'absolute top-px left-px w-[15px] h-[15px] rounded-full bg-white shadow transition-transform',
          on ? 'translate-x-[16px]' : 'translate-x-0',
        )}
      />
    </button>
  );
}
