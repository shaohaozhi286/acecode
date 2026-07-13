export const PATH_REFERENCE_LIMIT = 50;

function isSpace(ch) {
  return ch === ' ' || ch === '\t' || ch === '\r' || ch === '\n';
}

export function normalizeReferencePath(value = '') {
  let out = String(value || '').replaceAll('\\', '/');
  while (out.includes('//')) out = out.replaceAll('//', '/');
  if (out.startsWith('./')) out = out.slice(2);
  return out;
}

export function pathReferenceTokenAtCursor(text, cursor) {
  const input = String(text || '');
  const caret = Math.max(0, Math.min(input.length, Number.isFinite(cursor) ? cursor : input.length));
  for (let at = caret; at >= 0; at -= 1) {
    if (input[at] !== '@') continue;
    if (at > 0 && !isSpace(input[at - 1])) continue;
    const quoted = input[at + 1] === '"';
    let end = at + (quoted ? 2 : 1);
    if (quoted) {
      while (end < input.length && input[end] !== '"' && input[end] !== '\r' && input[end] !== '\n') end += 1;
      if (input[end] === '"') end += 1;
    } else {
      while (end < input.length && !isSpace(input[end])) end += 1;
    }
    if (caret < at || caret > end) continue;
    const valueStart = at + (quoted ? 2 : 1);
    const valueEnd = quoted && input[end - 1] === '"' ? end - 1 : end;
    return { begin: at, end, path: input.slice(valueStart, valueEnd), quoted };
  }
  return null;
}

export function splitPathReferenceQuery(value = '') {
  const path = normalizeReferencePath(value);
  const slash = path.lastIndexOf('/');
  if (slash < 0) return { directory: '', filter: path };
  return { directory: path.slice(0, slash), filter: path.slice(slash + 1) };
}

export function unsafeReferencePath(value = '') {
  const path = normalizeReferencePath(value);
  if (path.startsWith('/') || path.startsWith('\\') || /^[A-Za-z]:/.test(path)) return true;
  return path.split('/').some((part) => part === '..');
}

export function formatPathReference(relativePath, { directory = false, trailingSpace = true } = {}) {
  let path = normalizeReferencePath(relativePath);
  const filesystemRoot = path === '/' || /^[A-Za-z]:\/$/.test(path);
  if (!filesystemRoot) path = path.replace(/\/+$/, '');
  if (!path && directory) path = '.';
  if (directory && !path.endsWith('/')) path += '/';
  const quoted = /\s/.test(path);
  return `${quoted ? `@"${path}"` : `@${path}`}${trailingSpace ? ' ' : ''}`;
}

export function replacePathReferenceToken(text, token, relativePath, {
  directory = false,
  enterDirectory = false,
} = {}) {
  const input = String(text || '');
  const begin = Math.max(0, Math.min(input.length, Number(token?.begin) || 0));
  const end = Math.max(begin, Math.min(input.length, Number(token?.end) || begin));
  const replacement = formatPathReference(relativePath, {
    directory,
    trailingSpace: !enterDirectory,
  });
  return {
    text: input.slice(0, begin) + replacement + input.slice(end),
    cursor: begin + replacement.length,
  };
}

export function insertPathReferenceAtCaret(text, caret, relativePath) {
  const input = String(text || '');
  const cursor = Math.max(0, Math.min(input.length, Number.isFinite(caret) ? caret : input.length));
  const beforeNeedsSpace = cursor > 0 && !isSpace(input[cursor - 1]);
  const reference = formatPathReference(relativePath, { directory: true, trailingSpace: true });
  const insertion = `${beforeNeedsSpace ? ' ' : ''}${reference}`;
  return {
    text: input.slice(0, cursor) + insertion + input.slice(cursor),
    cursor: cursor + insertion.length,
  };
}

export function normalizePathReferenceCandidates(entries, filter = '', { foldersOnly = false } = {}) {
  const needle = String(filter || '').toLocaleLowerCase();
  return (Array.isArray(entries) ? entries : [])
    .filter((item) => item && (!foldersOnly || item.kind === 'dir'))
    .filter((item) => !needle || String(item.name || '').toLocaleLowerCase().includes(needle))
    .sort((a, b) => {
      const aDir = a.kind === 'dir';
      const bDir = b.kind === 'dir';
      if (aDir !== bDir) return aDir ? -1 : 1;
      const folded = String(a.name || '').localeCompare(String(b.name || ''), undefined, { sensitivity: 'base' });
      return folded || String(a.name || '').localeCompare(String(b.name || ''));
    })
    .slice(0, PATH_REFERENCE_LIMIT)
    .map((item) => ({
      name: String(item.name || ''),
      path: normalizeReferencePath(item.path || item.name || ''),
      kind: item.kind === 'dir' ? 'dir' : 'file',
    }));
}

export function pathReferenceSignature(token, cursor, cwd = '') {
  if (!token) return '';
  return `${cwd}\n${token.begin}:${token.end}:${cursor}:${token.path}:${token.quoted ? 1 : 0}`;
}
