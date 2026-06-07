import { memo, useEffect, useState } from 'react';
import { attachmentsFromContentParts, contextsFromContentParts, isImageAttachment, normalizeAttachmentList } from '../lib/messageAttachments.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { contextPresentation } from '../lib/selectionChatContext.js';
import { clsx } from '../lib/format.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';

function attachmentContextKey(att, index) {
  return String(att?.id || att?.blob_url || att?.preview_url || att?.url || att?.path || att?.name || index);
}

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

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (action !== DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT) return;
      if (target?.type !== 'attachment' || !target.id) return;
      const match = attachmentItems.find((att, index) => attachmentContextKey(att, index) === target.id);
      if (!match) return;
      const label = match.name || 'attachment';
      const previewUrl = match.blob_url || match.preview_url || match.url || '';
      if (!isImageAttachment(match) || !previewUrl) return;
      detail.handled = true;
      setPreview({ src: previewUrl, alt: label });
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [attachmentItems]);

  if (attachmentItems.length === 0 && contextItems.length === 0) return null;

  return (
    <>
      <div className={clsx(
        'flex flex-wrap gap-2 max-w-full',
        align === 'right' ? 'justify-end' : 'justify-start',
      )}>
        {attachmentItems.map((att, index) => {
          const key = attachmentContextKey(att, index);
          const isImage = isImageAttachment(att);
          const label = att.name || 'attachment';
          const attachmentUrl = att.blob_url || att.preview_url || att.url || '';
          const canPreview = isImage && attachmentUrl;
          const mimeType = att.mime_type || att.mimeType || '';
          return (
            <button
              key={key}
              type="button"
              data-desktop-attachment-id={key}
              data-desktop-attachment-name={label}
              data-desktop-attachment-url={attachmentUrl || undefined}
              data-desktop-attachment-path={att.path || undefined}
              data-desktop-attachment-preview-url={canPreview ? attachmentUrl : undefined}
              data-desktop-attachment-copy-image-url={canPreview ? attachmentUrl : undefined}
              data-desktop-attachment-mime-type={mimeType || undefined}
              data-desktop-attachment-kind={att.kind || (isImage ? 'image' : 'file')}
              data-desktop-attachment-mutable="false"
              className={clsx(
                'max-w-[220px] rounded-md border bg-surface overflow-hidden text-left transition',
                canPreview ? 'cursor-zoom-in hover:border-accent-soft' : 'cursor-default',
                align === 'right' ? 'border-accent-soft' : 'border-border',
                compact && 'max-w-[180px]',
              )}
              title={label}
              onClick={() => {
                if (canPreview) setPreview({ src: attachmentUrl, alt: label });
              }}
            >
              {isImage && attachmentUrl ? (
                <img
                  src={attachmentUrl}
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
        {contextItems.map((ctx, index) => {
          const presentation = contextPresentation(ctx);
          const isSelection = ctx?.type === 'selection';
          const sourcePath = ctx?.source?.path || ctx?.path || presentation.label;
          return (
            <div
              key={ctx.local_id || ctx.id || ctx.type || index}
              className="h-6 max-w-[240px] px-2 rounded-md border border-accent-soft bg-surface flex items-center gap-1 text-[11px] font-sans leading-none text-fg"
              title={presentation.title}
            >
              {isSelection ? (
                <FileTypeIcon path={sourcePath} size={11} className="ace-selection-context-icon opacity-90" />
              ) : (
                <VsIcon name={presentation.icon} size={11} className="ace-selection-context-icon" />
              )}
              <span className="truncate">{presentation.label}</span>
            </div>
          );
        })}
      </div>
      <ImageLightbox preview={preview} onClose={() => setPreview(null)} />
    </>
  );
});
