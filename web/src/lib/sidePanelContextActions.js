import { DESKTOP_CONTEXT_ACTIONS } from './desktopContextMenu.js';
import { normalizeWorkspaceRelativePath } from './fileTreeChangeStatus.js';

export const SIDE_PANEL_CONTEXT_EFFECTS = Object.freeze({
  OPEN_FILE_PREVIEW: 'open_file_preview',
  LOCATE_IN_FILE_TREE: 'locate_in_file_tree',
  REFRESH_FILE_TREE: 'refresh_file_tree',
});

export function sidePanelContextActionEffect({
  action = '',
  target = null,
  filesEnabled = true,
  cwd = '',
} = {}) {
  const filePath = target?.type === 'review'
    ? target.file
    : (target?.type === 'file' ? (target.relativePath || target.path) : '');
  const normalizedFilePath = normalizeWorkspaceRelativePath(filePath, cwd);
  if (!normalizedFilePath) return null;

  if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE) {
    return {
      type: SIDE_PANEL_CONTEXT_EFFECTS.OPEN_FILE_PREVIEW,
      filePath,
      normalizedFilePath,
    };
  }

  if (!filesEnabled) return null;

  if (action === DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE) {
    return {
      type: SIDE_PANEL_CONTEXT_EFFECTS.LOCATE_IN_FILE_TREE,
      filePath,
      normalizedFilePath,
    };
  }

  if (action === DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE && target?.type === 'file') {
    return {
      type: SIDE_PANEL_CONTEXT_EFFECTS.REFRESH_FILE_TREE,
      filePath,
      normalizedFilePath,
    };
  }

  return null;
}
