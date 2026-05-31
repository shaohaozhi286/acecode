import { memo, useState } from 'react';
import { createPortal } from 'react-dom';
import { attachmentsFromContentParts, contextsFromContentParts, isImageAttachment, normalizeAttachmentList } from '../lib/messageAttachments.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';

export const AttachmentStrip = memo(function AttachmentStrip({
  contentParts = [],
  attachments,
  contexts,
  align = 'left',
  compact = false,
}) {
  const [preview, setPreview] = useState(null);
  const attachmentItems = attachments !== undefined
    ? normalizeAttachmentList(attachments)
    : attachmentsFromContentParts(contentParts);
  const contextItems = contexts !== undefined
    ? (Array.isArray(contexts) ? contexts : [])
    : contextsFromContentParts(contentParts);

  if (attachmentItems.length === 0 && contextItems.length === 0) return null;

  return (
    <>
      <div className={clsx(
        'flex flex-wrap gap-2 max-w-full',
        align === 'right' ? 'justify-end' : 'justify-start',
      )}>
        {attachmentItems.map((att, index) => {
          const key = att.id || att.blob_url || att.path || att.name || index;
          const isImage = isImageAttachment(att);
          const label = att.name || 'attachment';
          const canPreview = isImage && att.blob_url;
          return (
            <button
              key={key}
              type="button"
              className={clsx(
                'max-w-[220px] rounded-md border bg-surface overflow-hidden text-left transition',
                canPreview ? 'cursor-zoom-in hover:border-accent-soft' : 'cursor-default',
                align === 'right' ? 'border-accent-soft' : 'border-border',
                compact && 'max-w-[180px]',
              )}
              title={label}
              onClick={() => {
                if (canPreview) setPreview({ src: att.blob_url, alt: label });
              }}
            >
              {isImage && att.blob_url ? (
                <img
                  src={att.blob_url}
                  alt={label}
                  className={clsx(
                    'block max-w-full object-contain bg-bg',
                    compact ? 'max-h-28' : 'max-h-40',
                  )}
                />
              ) : (
                <span className="px-2.5 py-2 flex items-center gap-2">
                  <VsIcon name="file" size={15} />
                  <span className="truncate text-[12px] text-fg">{label}</span>
                </span>
              )}
            </button>
          );
        })}
        {contextItems.map((ctx) => (
          <div
            key={ctx.local_id || ctx.id || ctx.type || 'browser'}
            className="h-7 px-2 rounded-md border border-accent-soft bg-surface flex items-center gap-1.5 text-[12px] text-fg"
          >
            <VsIcon name="search" size={12} />
            <span>{ctx.label || 'Browser'}</span>
          </div>
        ))}
      </div>
      {preview
        ? createPortal(
            <div
              className="fixed inset-0 z-[80] bg-black/70 flex items-center justify-center p-6"
              role="dialog"
              aria-modal="true"
              onClick={() => setPreview(null)}
            >
              <button
                type="button"
                className="absolute top-3 right-3 w-8 h-8 rounded-md bg-surface text-fg border border-border flex items-center justify-center"
                aria-label="关闭预览"
                title="关闭"
                onClick={() => setPreview(null)}
              >
                <VsIcon name="close" size={15} />
              </button>
              <img
                src={preview.src}
                alt={preview.alt}
                className="max-w-full max-h-full object-contain shadow-xl"
                onClick={(event) => event.stopPropagation()}
              />
            </div>,
            document.body,
          )
        : null}
    </>
  );
});
