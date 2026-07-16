export const PROJECT_DIRECTORY_NAME_MAX_CODE_POINTS = 60;

function trimAsciiWhitespace(value) {
  return String(value || '').replace(/^[\t\n\v\f\r ]+|[\t\n\v\f\r ]+$/g, '');
}
function trimWindowsTrailingCharacters(value) {
  return value.replace(/[ .]+$/g, '');
}

function incompatibleProjectCharacter(character) {
  const code = character.codePointAt(0);
  return code < 32 || code === 127 || '<>:"/\\|?*'.includes(character);
}

function windowsReservedDeviceName(value) {
  const base = trimWindowsTrailingCharacters(value.split('.', 1)[0]).toUpperCase();
  if (['CON', 'PRN', 'AUX', 'NUL', 'CLOCK$'].includes(base)) return true;
  return /^(COM|LPT)[1-9]$/.test(base);
}

export function normalizeProjectDirectoryName(requestedName = '') {
  const requested = String(requestedName || '');
  const source = trimAsciiWhitespace(requested);
  let directoryName = '';

  for (const character of source) {
    if (incompatibleProjectCharacter(character)) {
      if (!directoryName.endsWith('-')) directoryName += '-';
    } else {
      directoryName += character;
    }
  }

  directoryName = trimWindowsTrailingCharacters(trimAsciiWhitespace(directoryName));
  if (!directoryName || directoryName === '.' || directoryName === '..') {
    directoryName = 'project';
  }
  directoryName = Array.from(directoryName)
    .slice(0, PROJECT_DIRECTORY_NAME_MAX_CODE_POINTS)
    .join('');
  directoryName = trimWindowsTrailingCharacters(directoryName) || 'project';
  if (windowsReservedDeviceName(directoryName)) directoryName += '-project';

  return {
    directoryName,
    changed: directoryName !== requested,
  };
}

export function projectPathPreview(parentDir = '', directoryName = '') {
  const parent = String(parentDir || '');
  const child = String(directoryName || '');
  if (!parent) return child;
  const separator = parent.includes('\\') && !parent.includes('/') ? '\\' : '/';
  const withoutTrailingSeparator = parent.replace(/[\\/]+$/g, '');
  if (!withoutTrailingSeparator) return `${separator}${child}`;
  return `${withoutTrailingSeparator}${separator}${child}`;
}

export function projectCreationErrorMessage(error) {
  return String(
    error?.body?.message
      || error?.message
      || '创建项目失败，请稍后重试',
  ).replace(/^HTTP\s+\d+:\s*/i, '');
}
