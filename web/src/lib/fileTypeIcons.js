import {
  SETI_DEFAULT_FILE_ICON_ID,
  SETI_FILE_EXTENSION_ICON_IDS,
  SETI_FILE_NAME_ICON_IDS,
  SETI_FILE_PATTERN_ICON_IDS,
  SETI_ICON_DEFINITIONS,
} from './setiFileIconTheme.generated.js';

function basename(pathOrName) {
  return String(pathOrName || '').split(/[\\/]/).pop() || '';
}

function normalizePath(pathOrName) {
  return String(pathOrName || '').replace(/\\/g, '/').toLowerCase();
}

function normalizeName(pathOrName) {
  return basename(pathOrName).toLowerCase();
}

function stripLeadingDots(value) {
  return String(value || '').replace(/^\.+/u, '');
}

function iconForId(iconId) {
  const id = iconId || SETI_DEFAULT_FILE_ICON_ID;
  const definition = SETI_ICON_DEFINITIONS[id] || SETI_ICON_DEFINITIONS[SETI_DEFAULT_FILE_ICON_ID];
  return {
    id,
    glyph: definition?.[0] || '',
    color: definition?.[1] || '#d4d7d6',
  };
}

function fileNameCandidates(name) {
  const candidates = [name, stripLeadingDots(name)].filter(Boolean);
  return Array.from(new Set(candidates));
}

function extensionCandidates(name) {
  const plain = stripLeadingDots(name);
  if (!plain) return [];
  const parts = plain.split('.').filter(Boolean);
  if (parts.length === 0) return [];
  const candidates = [];
  for (let i = 0; i < parts.length; i += 1) {
    candidates.push(parts.slice(i).join('.'));
  }
  return Array.from(new Set(candidates));
}

function globToRegExp(pattern) {
  let source = '';
  for (let i = 0; i < pattern.length; i += 1) {
    const ch = pattern[i];
    const next = pattern[i + 1];
    if (ch === '*' && next === '*') {
      const slash = pattern[i + 2] === '/';
      source += slash ? '(?:.*/)?' : '.*';
      i += slash ? 2 : 1;
    } else if (ch === '*') {
      source += '[^/]*';
    } else if (ch === '?') {
      source += '[^/]';
    } else {
      source += ch.replace(/[|\\{}()[\]^$+?.]/gu, '\\$&');
    }
  }
  return new RegExp(`^${source}$`, 'u');
}

const SETI_FILE_PATTERN_MATCHERS = SETI_FILE_PATTERN_ICON_IDS.map(([pattern, iconId]) => ({
  pattern,
  iconId,
  hasPathSegment: pattern.includes('/'),
  regex: globToRegExp(pattern),
}));

function patternIconIdForPath(pathOrName, name) {
  const normalizedPath = normalizePath(pathOrName);
  for (const matcher of SETI_FILE_PATTERN_MATCHERS) {
    const target = matcher.hasPathSegment ? normalizedPath : name;
    if (matcher.regex.test(target)) return matcher.iconId;
  }
  return '';
}

function iconIdForPath(pathOrName) {
  const name = normalizeName(pathOrName);

  for (const candidate of fileNameCandidates(name)) {
    const iconId = SETI_FILE_NAME_ICON_IDS[candidate];
    if (iconId) return iconId;
  }

  const patternIconId = patternIconIdForPath(pathOrName, name);
  if (patternIconId) return patternIconId;

  for (const candidate of extensionCandidates(name)) {
    const iconId = SETI_FILE_EXTENSION_ICON_IDS[candidate];
    if (iconId) return iconId;
  }

  return SETI_DEFAULT_FILE_ICON_ID;
}

export function fileTypeIconForPath(pathOrName) {
  return iconForId(iconIdForPath(pathOrName));
}
