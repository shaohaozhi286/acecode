export const PERMISSION_MODES = [
  { id: 'default', label: '默认权限', hint: '写/执行操作前确认', color: 'ok' },
  { id: 'accept-edits', label: '自动接收编辑', hint: '文件编辑自动通过,命令仍确认', color: 'warn' },
  { id: 'yolo', label: '完全访问权限', hint: '跳过所有工具权限确认', color: 'danger' },
];

const VALID_PERMISSION_MODES = new Set(PERMISSION_MODES.map((mode) => mode.id));

export function normalizePermissionMode(mode) {
  const value = String(mode || '').trim();
  if (value === 'acceptEdits') return 'accept-edits';
  return VALID_PERMISSION_MODES.has(value) ? value : 'default';
}

export function permissionModeOption(mode) {
  const normalized = normalizePermissionMode(mode);
  return PERMISSION_MODES.find((item) => item.id === normalized) || PERMISSION_MODES[0];
}
