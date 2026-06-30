const MARKDOWN_EXTENSIONS = new Set(['md', 'markdown']);
const IMAGE_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg']);
const PDF_EXTENSIONS = new Set(['pdf']);
const WORD_EXTENSIONS = new Set(['docx']);
const SPREADSHEET_EXTENSIONS = new Set(['xlsx', 'xlsm']);
const UNSUPPORTED_BINARY_EXTENSIONS = new Set(['doc', 'xls']);

export function extensionForPath(path) {
  const name = String(path || '').split(/[\\/]/).pop() || '';
  const dot = name.lastIndexOf('.');
  if (dot < 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

export function filePreviewKind(path) {
  const ext = extensionForPath(path);
  if (IMAGE_EXTENSIONS.has(ext)) return 'image';
  if (PDF_EXTENSIONS.has(ext)) return 'pdf';
  if (WORD_EXTENSIONS.has(ext)) return 'word';
  if (SPREADSHEET_EXTENSIONS.has(ext)) return 'spreadsheet';
  if (UNSUPPORTED_BINARY_EXTENSIONS.has(ext)) return 'unsupported';
  if (MARKDOWN_EXTENSIONS.has(ext)) return 'markdown';
  return 'text';
}

export function isBlobFilePreview(path) {
  const kind = filePreviewKind(path);
  return kind === 'image' || kind === 'pdf' || kind === 'word' || kind === 'spreadsheet';
}
