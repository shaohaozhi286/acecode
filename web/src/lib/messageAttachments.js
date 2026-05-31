export function attachmentsFromContentParts(contentParts = []) {
  if (!Array.isArray(contentParts)) return [];
  return contentParts
    .filter((part) => part && (part.type === 'image' || part.type === 'file') && part.attachment)
    .map((part) => ({ ...part.attachment, type: part.type }));
}

export function contextsFromContentParts(contentParts = []) {
  if (!Array.isArray(contentParts)) return [];
  return contentParts
    .filter((part) => part && part.type === 'browser_context')
    .map((part) => part.context || {});
}

export function normalizeAttachmentList(attachments = []) {
  if (!Array.isArray(attachments)) return [];
  return attachments
    .map((att) => {
      if (!att || typeof att !== 'object') return null;
      if (att.attachment && typeof att.attachment === 'object') {
        return { ...att.attachment, type: att.type || att.attachment.kind || '' };
      }
      return { ...att };
    })
    .filter(Boolean);
}

export function isImageAttachment(att) {
  if (!att || typeof att !== 'object') return false;
  return att.type === 'image'
    || att.kind === 'image'
    || String(att.mime_type || att.mimeType || '').startsWith('image/');
}
