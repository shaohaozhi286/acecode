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

// 斜杠命令徽标的前导图标。用 currentColor 上色,放进 accent 色徽标里自动变蓝。
// skill → sparkle(灵感/技能);builtin → 终端提示符 `>_`。
export function CommandGlyph({ kind = 'skill', size = 12, className = '', ...props }) {
  if (kind === 'builtin') {
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
        {...props}
      >
        <path d="M3.75 4.5 7 8l-3.25 3.5" />
        <path d="M8.5 11.5h3.75" />
      </svg>
    );
  }
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="currentColor"
      className={className}
      aria-hidden="true"
      {...props}
    >
      <path d="M8 1.4c.18 1.86.86 3.13 1.96 3.92.78.57 1.86.96 3.24 1.18-1.38.22-2.46.61-3.24 1.18C8.86 8.47 8.18 9.74 8 11.6c-.18-1.86-.86-3.13-1.96-3.92C5.26 7.11 4.18 6.72 2.8 6.5c1.38-.22 2.46-.61 3.24-1.18C7.14 4.53 7.82 3.26 8 1.4z" />
      <path d="M12.6 9.6c.09.86.42 1.45 1.6 1.65-1.18.2-1.51.79-1.6 1.65-.09-.86-.42-1.45-1.6-1.65 1.18-.2 1.51-.79 1.6-1.65z" />
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
