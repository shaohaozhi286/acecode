import { fileTypeIconForPath } from '../lib/fileTypeIcons.js';

const ICONS = {
  add: 'Add',
  archive: 'Archive',
  arrowLeft: 'ArrowLeft',
  arrowRight: 'ArrowRight',
  back: 'Backwards',
  brain: 'Brain',
  brightness: 'Brightness',
  check: 'Check',
  clearAll: 'ClearAll',
  close: 'Close',
  code: 'Code',
  command: 'TerminalReadWrite',
  collapseAll: 'CollapseAll',
  copy: 'Copy',
  darkTheme: 'DarkTheme',
  delete: 'Delete',
  document: 'Document',
  edit: 'Edit',
  editWindow: 'EditWindow',
  ellipsis: 'Ellipsis',
  embedding: 'Embedding',
  expandDown: 'ExpandDown',
  expandRight: 'ExpandRight',
  expandUp: 'ExpandUp',
  extension: 'Extension',
  eye: 'Eye',
  file: 'Document',
  folder: 'FolderClosed',
  folderAdd: 'AddFolder',
  folderOpen: 'FolderOpened',
  fork: 'Fork',
  glyphDown: 'GlyphDown',
  glyphUp: 'GlyphUp',
  globe: 'Globe',
  help: 'StatusHelp',
  hook: 'FishHook',
  info: 'StatusInformation',
  leftBar: 'LeftBar',
  lightbulb: 'IntellisenseLightBulbSparkle',
  list: 'List',
  lock: 'Lock',
  mcp: 'MCP',
  newSession: 'NewSession',
  ok: 'StatusOK',
  openFile: 'OpenFile',
  panelLeft: 'PanelLeft',
  panelRight: 'PanelRight',
  pin: 'Pin',
  refresh: 'Refresh',
  rightBar: 'RightBar',
  run: 'Run',
  running: 'StatusRunning',
  save: 'Save',
  screenFull: 'ScreenFull',
  screenNormal: 'ScreenNormal',
  search: 'Search',
  searchSparkle: 'SearchSparkle',
  send: 'Send',
  settings: 'Settings',
  stop: 'Stop',
  terminal: 'TerminalReadWrite',
  tool: 'Tool',
  warning: 'StatusWarning',
  wordWrap: 'WordWrap',
  world: 'World',
  error: 'StatusError',
};

const TOOL_ICON_MAP = new Map([
  ['$', 'terminal'],
  ['!', 'warning'],
  ['R', 'openFile'],
  ['W', 'save'],
  ['E', 'edit'],
  ['D', 'ok'],
  ['S', 'search'],
  ['\u2192', 'openFile'],
  ['\u270D', 'save'],
  ['\u270E', 'edit'],
  ['\u2713', 'ok'],
  ['\u2717', 'error'],
  ['\u{1F50D}', 'search'],
  ['\u26A0', 'warning'],
  ['\u26A0\uFE0F', 'warning'],
]);

const CSS_MASK_SUPPORTED = (() => {
  if (typeof CSS === 'undefined' || typeof CSS.supports !== 'function') return false;
  try {
    return CSS.supports('-webkit-mask-image', 'url("/vs-icons/Add.svg")')
      || CSS.supports('mask-image', 'url("/vs-icons/Add.svg")');
  } catch {
    return false;
  }
})();

export function VsIcon({
  name,
  size = 16,
  mono = true,
  className = '',
  alt = '',
  style,
  ...props
}) {
  const file = ICONS[name] || name;
  const src = `/vs-icons/${file}.svg`;
  const accessibilityProps = alt
    ? { role: 'img', 'aria-label': alt }
    : { 'aria-hidden': 'true' };
  return (
    <span
      className={['ace-icon', !CSS_MASK_SUPPORTED && 'ace-icon-fallback', className].filter(Boolean).join(' ')}
      data-icon-name={file}
      data-monochrome={mono ? 'true' : 'false'}
      style={{
        width: size,
        height: size,
        '--ace-icon-url': `url("${src}")`,
        ...style,
      }}
      {...accessibilityProps}
      {...props}
    >
      {!CSS_MASK_SUPPORTED && (
        <img
          className="ace-icon-fallback-img"
          src={src}
          alt=""
          draggable="false"
        />
      )}
    </span>
  );
}

export function FileTypeIcon({
  path,
  size = 20,
  className = '',
  fallback = 'file',
  style,
  ...props
}) {
  const icon = fileTypeIconForPath(path);
  if (!icon) {
    return <VsIcon name={fallback} size={size} mono={false} className={className} {...props} />;
  }
  return (
    <span
      className={['ace-file-type-icon', className].filter(Boolean).join(' ')}
      aria-hidden="true"
      data-file-type-icon={icon.id}
      style={{
        width: size,
        height: size,
        fontSize: size,
        color: icon.color,
        ...style,
      }}
      {...props}
    >
      {icon.glyph}
    </span>
  );
}

export function ToolSummaryIcon({ icon, ok, className = '' }) {
  if (!ok) return <VsIcon name="error" size={14} mono={false} className={className} />;
  const mapped = TOOL_ICON_MAP.get(icon) || (ICONS[icon] ? icon : 'ok');
  const statusIcon = mapped === 'ok' || mapped === 'warning' || mapped === 'error';
  return <VsIcon name={mapped} size={14} mono={!statusIcon} className={className} />;
}

export function PanelToggleIcon({ side = 'left', size = 16, className = '', ...props }) {
  return (
    <VsIcon
      name={side === 'right' ? 'panelRight' : 'panelLeft'}
      size={size}
      className={className}
      {...props}
    />
  );
}

export function RefreshIcon({ size = 16, className = '', ...props }) {
  return <VsIcon name="refresh" size={size} className={className} {...props} />;
}

// Slash-command badge icon. It inherits currentColor inside the accent badge.
export function CommandGlyph({ kind = 'skill', size = 12, className = '', ...props }) {
  const name = kind === 'builtin' ? 'tool' : kind === 'command' ? 'command' : 'lightbulb';
  return (
    <VsIcon
      name={name}
      size={size}
      className={className}
      {...props}
    />
  );
}

export function NavigationArrowIcon({ direction = 'back', size = 16, className = '', ...props }) {
  return (
    <VsIcon
      name={direction === 'forward' ? 'arrowRight' : 'arrowLeft'}
      size={size}
      className={className}
      {...props}
    />
  );
}
