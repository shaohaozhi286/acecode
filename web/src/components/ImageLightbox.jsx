import { createPortal } from 'react-dom';
import { VsIcon } from './Icon.jsx';

export function ImageLightbox({ preview, onClose }) {
  if (!preview?.src) return null;

  return createPortal(
    <div
      className="fixed inset-0 z-[80] bg-black/70 flex items-center justify-center p-6"
      role="dialog"
      aria-modal="true"
      onClick={onClose}
    >
      <button
        type="button"
        className="absolute top-3 right-3 w-8 h-8 rounded-md bg-surface text-fg border border-border flex items-center justify-center"
        aria-label="关闭预览"
        title="关闭"
        onClick={onClose}
      >
        <VsIcon name="close" size={15} />
      </button>
      <img
        src={preview.src}
        alt={preview.alt || ''}
        className="max-w-full max-h-full object-contain shadow-xl"
        onClick={(event) => event.stopPropagation()}
      />
    </div>,
    document.body,
  );
}
