const ICONS = {
  add: 'Add',
  back: 'Backwards',
  brightness: 'Brightness',
  close: 'Close',
  code: 'Code',
  copy: 'Copy',
  darkTheme: 'DarkTheme',
  document: 'Document',
  edit: 'Edit',
  editWindow: 'EditWindow',
  ellipsis: 'Ellipsis',
  expandDown: 'ExpandDown',
  expandRight: 'ExpandRight',
  expandUp: 'ExpandUp',
  extension: 'Extension',
  file: 'Document',
  folder: 'FolderClosed',
  folderAdd: 'AddFolder',
  folderOpen: 'FolderOpened',
  fork: 'Fork',
  glyphDown: 'GlyphDown',
  glyphUp: 'GlyphUp',
  help: 'StatusHelp',
  info: 'StatusInformation',
  lightbulb: 'IntellisenseLightBulbSparkle',
  lock: 'Lock',
  mcp: 'MCP',
  ok: 'StatusOK',
  openFile: 'OpenFile',
  pin: 'Pin',
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
  warning: 'StatusWarning',
  wordWrap: 'WordWrap',
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

export function VsIcon({ name, size = 16, mono = true, className = '', alt = '', ...props }) {
  const file = ICONS[name] || name;
  return (
    <img
      src={`/vs-icons/${file}.svg`}
      alt={alt}
      width={size}
      height={size}
      className={['ace-icon', className].filter(Boolean).join(' ')}
      draggable="false"
      data-monochrome={mono ? 'true' : 'false'}
      aria-hidden={alt ? undefined : true}
      {...props}
    />
  );
}

export function ToolSummaryIcon({ icon, ok, className = '' }) {
  if (!ok) return <VsIcon name="error" size={14} mono={false} className={className} />;
  const mapped = TOOL_ICON_MAP.get(icon) || (ICONS[icon] ? icon : 'ok');
  const statusIcon = mapped === 'ok' || mapped === 'warning' || mapped === 'error';
  return <VsIcon name={mapped} size={14} mono={!statusIcon} className={className} />;
}

export function PanelToggleIcon({ side = 'left', size = 16, className = '', ...props }) {
  const dividerX = side === 'right' ? 13.25 : 4.75;
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.45"
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
      aria-hidden="true"
      {...props}
    >
      <rect x="2.75" y="2.25" width="10.5" height="11.5" rx="1.25" />
      <path d={`M${dividerX} 2.8v10.4`} />
    </svg>
  );
}

export function RefreshIcon({ size = 16, className = '', ...props }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.55"
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
      aria-hidden="true"
      {...props}
    >
      <path d="M13 6.25A5 5 0 0 0 4.05 4.1L3 5.15" />
      <path d="M3 2.35v2.8h2.8" />
      <path d="M3 9.75a5 5 0 0 0 8.95 2.15L13 10.85" />
      <path d="M13 13.65v-2.8h-2.8" />
    </svg>
  );
}

export function NavigationArrowIcon({ direction = 'back', size = 16, className = '', ...props }) {
  const flip = direction === 'forward';
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.6"
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
      aria-hidden="true"
      style={flip ? { transform: 'scaleX(-1)' } : undefined}
      {...props}
    >
      <path d="M9.5 4.25 5.75 8l3.75 3.75" />
      <path d="M6.25 8h5.5" />
    </svg>
  );
}
