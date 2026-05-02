const ICONS = {
  add: 'Add',
  back: 'Backwards',
  brightness: 'Brightness',
  close: 'Close',
  code: 'Code',
  darkTheme: 'DarkTheme',
  document: 'Document',
  edit: 'Edit',
  ellipsis: 'Ellipsis',
  expandDown: 'ExpandDown',
  expandRight: 'ExpandRight',
  extension: 'Extension',
  folder: 'FolderClosed',
  glyphDown: 'GlyphDown',
  glyphUp: 'GlyphUp',
  help: 'StatusHelp',
  info: 'StatusInformation',
  lightbulb: 'IntellisenseLightBulbSparkle',
  lock: 'Lock',
  mcp: 'MCP',
  ok: 'StatusOK',
  openFile: 'OpenFile',
  run: 'Run',
  running: 'StatusRunning',
  save: 'Save',
  search: 'Search',
  searchSparkle: 'SearchSparkle',
  send: 'Send',
  settings: 'Settings',
  stop: 'Stop',
  terminal: 'TerminalReadWrite',
  warning: 'StatusWarning',
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
