import { normalizeTreePath } from './fileTreeChangeStatus.js';

export const DESKTOP_CONTEXT_ACTIONS = Object.freeze({
  OPEN_IN_EXPLORER: 'open_in_explorer',
  LOCATE_FILE: 'locate_file',
  PIN_SESSION: 'pin_session',
  UNPIN_SESSION: 'unpin_session',
  OPEN_SESSION: 'open_session',
  RENAME_SESSION: 'rename_session',
  COPY_SESSION_TITLE: 'copy_session_title',
  COPY_SESSION_ID: 'copy_session_id',
  EXPORT_SESSION: 'export_session',
  ARCHIVE_SESSION: 'archive_session',
  ACTIVATE_WORKSPACE: 'activate_workspace',
  EXPAND_WORKSPACE: 'expand_workspace',
  COLLAPSE_WORKSPACE: 'collapse_workspace',
  NEW_WORKSPACE_SESSION: 'new_workspace_session',
  IMPORT_OPENCODE_SESSIONS: 'import_opencode_sessions',
  RENAME_WORKSPACE: 'rename_workspace',
  COPY_WORKSPACE_PATH: 'copy_workspace_path',
  REMOVE_WORKSPACE: 'remove_workspace',
  PREVIEW_FILE: 'preview_file',
  REFRESH_DETAILS: 'refresh_details',
  CLOSE_PREVIEW_TAB: 'close_preview_tab',
  CLOSE_OTHER_PREVIEW_TABS: 'close_other_preview_tabs',
  CLOSE_PREVIEW_TABS_TO_RIGHT: 'close_preview_tabs_to_right',
  CLOSE_ALL_PREVIEW_TABS: 'close_all_preview_tabs',
  COPY_RELATIVE_PATH: 'copy_relative_path',
  COPY_ABSOLUTE_PATH: 'copy_absolute_path',
  ADD_FILE_CONTEXT: 'add_file_context',
  ADD_DIRECTORY_CONTEXT: 'add_directory_context',
  ADD_SELECTION_CONTEXT: 'add_selection_context',
  REFRESH_FILE_TREE: 'refresh_file_tree',
  EXPAND_DIRECTORY: 'expand_directory',
  COLLAPSE_DIRECTORY: 'collapse_directory',
  COPY_PREVIEW_TEXT: 'copy_preview_text',
  COPY_PREVIEW_METADATA: 'copy_preview_metadata',
  COPY_PREVIEW_IMAGE: 'copy_preview_image',
  COPY_FILE_DIFF: 'copy_file_diff',
  COPY_ALL_DIFFS: 'copy_all_diffs',
  LOCATE_IN_FILE_TREE: 'locate_in_file_tree',
  EXPAND_ALL_DIFFS: 'expand_all_diffs',
  COLLAPSE_ALL_DIFFS: 'collapse_all_diffs',
  COPY_MESSAGE_TEXT: 'copy_message_text',
  FORK_MESSAGE: 'fork_message',
  COPY_VISIBLE_TOOL_OUTPUT: 'copy_visible_tool_output',
  COPY_FULL_TOOL_OUTPUT: 'copy_full_tool_output',
  EXPAND_TOOL: 'expand_tool',
  COLLAPSE_TOOL: 'collapse_tool',
  PREVIEW_ATTACHMENT: 'preview_attachment',
  COPY_ATTACHMENT_IMAGE: 'copy_attachment_image',
  COPY_ATTACHMENT_NAME: 'copy_attachment_name',
  COPY_ATTACHMENT_URL: 'copy_attachment_url',
  REMOVE_ATTACHMENT: 'remove_attachment',
  SELECT_ALL: 'select_all',
  COPY: 'copy',
  PASTE: 'paste',
  CUT: 'cut',
  INSPECT: 'inspect',
});

export const OPEN_IN_EXPLORER_TARGET_SELECTOR = '[data-desktop-open-in-explorer-path]';
export const SESSION_PIN_TARGET_SELECTOR = '[data-desktop-session-id]';
export const SESSION_TARGET_SELECTOR = '[data-desktop-session-id]';
export const WORKSPACE_TARGET_SELECTOR = '[data-desktop-workspace-id]';
export const FILE_TARGET_SELECTOR = '[data-desktop-file-path]';
export const PREVIEW_TARGET_SELECTOR = '[data-desktop-preview-path]';
export const PREVIEW_TAB_TARGET_SELECTOR = '[data-desktop-preview-tab-key]';
export const REVIEW_TARGET_SELECTOR = '[data-desktop-review-kind]';
export const MESSAGE_TARGET_SELECTOR = '[data-desktop-message-role]';
export const TOOL_TARGET_SELECTOR = '[data-desktop-tool-id]';
export const ATTACHMENT_TARGET_SELECTOR = '[data-desktop-attachment-id]';

export const SESSION_PIN_TOGGLE_EVENT = 'acecode:session-pin-toggle';
export const DESKTOP_CONTEXT_ACTION_EVENT = 'acecode:desktop-context-action';
export const CONTEXT_MENU_REOPEN_DELAY_MS = 10;

const GROUPS = Object.freeze({
  OBJECT: 'object',
  FILE: 'file',
  SELECTION: 'selection',
  REVIEW: 'review',
  CONTENT: 'content',
  GENERIC: 'generic',
  DEBUG: 'debug',
  DANGER: 'danger',
});

function getAttr(el, name, datasetName = '') {
  if (!el) return '';
  if (typeof el.getAttribute === 'function') return el.getAttribute(name) || '';
  return el.dataset?.[datasetName || name.replace(/^data-/, '').replace(/-([a-z])/g, (_, c) => c.toUpperCase())] || '';
}

function boolAttr(el, name, datasetName = '') {
  return getAttr(el, name, datasetName) === 'true';
}

function numberAttr(el, name, datasetName = '') {
  const n = Number(getAttr(el, name, datasetName));
  return Number.isFinite(n) ? n : 0;
}

function closest(target, selector) {
  if (!target || typeof target.closest !== 'function') return null;
  return target.closest(selector);
}

function actionDescriptor(id, target = null, opts = {}) {
  return {
    id,
    labelKey: opts.labelKey || id,
    target,
    group: opts.group || GROUPS.OBJECT,
    enabled: opts.enabled !== false,
    danger: !!opts.danger,
    confirm: opts.confirm || '',
    separatorBefore: !!opts.separatorBefore,
  };
}

function addAction(items, id, target, opts = {}) {
  items.push(actionDescriptor(id, target, opts));
}

export function contextMenuOpenDelay({ hasVisibleMenu = false, hasPendingMenu = false } = {}) {
  return hasVisibleMenu || hasPendingMenu ? CONTEXT_MENU_REOPEN_DELAY_MS : 0;
}

export function withMenuSeparators(items = []) {
  let lastGroup = '';
  return items.map((item, index) => {
    const group = item?.group || '';
    const separatorBefore = !!item?.separatorBefore || (index > 0 && group && lastGroup && group !== lastGroup);
    lastGroup = group || lastGroup;
    return separatorBefore === !!item?.separatorBefore ? item : { ...item, separatorBefore };
  });
}

export function buildDesktopContextMenuItems({
  editable = false,
  hasSelection = false,
  debug = false,
  openInExplorer = false,
  openInExplorerTarget = null,
  sessionPinTarget = null,
  sessionTarget = null,
  workspaceTarget = null,
  fileTarget = null,
  previewTarget = null,
  reviewTarget = null,
  messageTarget = null,
  toolTarget = null,
  attachmentTarget = null,
  previewTabTarget = null,
} = {}) {
  const items = [];

  if (!editable && previewTabTarget) {
    addAction(items, DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TAB, previewTabTarget, { group: GROUPS.OBJECT });
    addAction(items, DESKTOP_CONTEXT_ACTIONS.CLOSE_OTHER_PREVIEW_TABS, previewTabTarget, { group: GROUPS.OBJECT, enabled: previewTabTarget.hasOthers });
    addAction(items, DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TABS_TO_RIGHT, previewTabTarget, { group: GROUPS.OBJECT, enabled: previewTabTarget.hasRight });
    addAction(items, DESKTOP_CONTEXT_ACTIONS.CLOSE_ALL_PREVIEW_TABS, previewTabTarget, { group: GROUPS.GENERIC });
    if (previewTabTarget.tabType === 'file') {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH, previewTabTarget, { group: GROUPS.FILE, enabled: !!previewTabTarget.absolutePath });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH, previewTabTarget, { group: GROUPS.FILE, enabled: !!previewTabTarget.relativePath });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER, { path: previewTabTarget.absolutePath, kind: 'file' }, { group: GROUPS.CONTENT, enabled: !!previewTabTarget.absolutePath });
    }
    return withMenuSeparators(items);
  }

  if (!editable && hasSelection && previewTarget &&
      (previewTarget.kind === 'text' || previewTarget.kind === 'markdown')) {
    addAction(items, DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT, previewTarget, { group: GROUPS.SELECTION });
  }

  if (!editable) {
    if (sessionTarget) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION, sessionTarget);
      addAction(items, DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION, sessionTarget);
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE, sessionTarget, { enabled: !!sessionTarget.title });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID, sessionTarget);
      addAction(items, DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION, sessionTarget);
      addAction(items, sessionTarget.pinned ? DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION : DESKTOP_CONTEXT_ACTIONS.PIN_SESSION, sessionTarget);
      if (sessionTarget.canArchive) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION, sessionTarget, {
          group: GROUPS.DANGER,
          danger: true,
          confirm: '确认归档这个会话？',
        });
      }
    } else if (sessionPinTarget) {
      addAction(items, sessionPinTarget.pinned ? DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION : DESKTOP_CONTEXT_ACTIONS.PIN_SESSION, sessionPinTarget);
    }

    if (workspaceTarget) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE, workspaceTarget, { enabled: !workspaceTarget.active });
      addAction(items, workspaceTarget.expanded ? DESKTOP_CONTEXT_ACTIONS.COLLAPSE_WORKSPACE : DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE, workspaceTarget);
      addAction(items, DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION, workspaceTarget);
      if (workspaceTarget.opencodeImportCount > 0) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS, workspaceTarget);
      }
      addAction(items, DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE, workspaceTarget, { enabled: workspaceTarget.canRename !== false });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH, workspaceTarget, { enabled: !!workspaceTarget.path });
      if (workspaceTarget.path) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER, { path: workspaceTarget.path, kind: 'workspace' });
      }
      if (workspaceTarget.canRemove) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE, workspaceTarget, {
          group: GROUPS.DANGER,
          danger: true,
          confirm: '仅从桌面项目列表移除，不会删除磁盘文件。继续？',
        });
      }
    }

    if (fileTarget) {
      if (fileTarget.kind === 'directory') {
        if (fileTarget.absolutePath) {
          addAction(items, DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER, { path: fileTarget.absolutePath, kind: 'directory' }, { group: GROUPS.FILE });
        }
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH, fileTarget, { group: GROUPS.FILE, enabled: !!fileTarget.relativePath });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH, fileTarget, { group: GROUPS.FILE, enabled: !!fileTarget.absolutePath });
        if (fileTarget.canAddContext) {
          addAction(items, DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT, fileTarget, { group: GROUPS.FILE });
        }
        addAction(items, DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE, fileTarget, { group: GROUPS.FILE });
        addAction(items, fileTarget.expanded ? DESKTOP_CONTEXT_ACTIONS.COLLAPSE_DIRECTORY : DESKTOP_CONTEXT_ACTIONS.EXPAND_DIRECTORY, fileTarget, { group: GROUPS.FILE });
      } else {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE, fileTarget, { group: GROUPS.FILE });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH, fileTarget, { group: GROUPS.FILE, enabled: !!fileTarget.relativePath });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH, fileTarget, { group: GROUPS.FILE, enabled: !!fileTarget.absolutePath });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE, fileTarget, { group: GROUPS.FILE, enabled: !!fileTarget.locatePath });
        if (fileTarget.canAddContext) {
          addAction(items, DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT, fileTarget, { group: GROUPS.FILE });
        }
      }
    }

    if (previewTarget) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS, previewTarget, {
        group: GROUPS.CONTENT,
      });
      if (previewTarget.kind === 'image' && previewTarget.copyImageUrl) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_IMAGE, previewTarget, {
          group: GROUPS.CONTENT,
        });
      }
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT, previewTarget, {
        group: GROUPS.CONTENT,
        enabled: previewTarget.kind === 'text' || previewTarget.kind === 'markdown',
      });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA, previewTarget, { group: GROUPS.CONTENT });
    }

    if (reviewTarget) {
      if (reviewTarget.canRefresh) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS, reviewTarget, { group: GROUPS.REVIEW });
      }
      if (reviewTarget.kind === 'summary') {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS, reviewTarget, { group: GROUPS.REVIEW });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS, reviewTarget, { group: GROUPS.REVIEW });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS, reviewTarget, { group: GROUPS.REVIEW });
      } else {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF, reviewTarget, { group: GROUPS.REVIEW });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH, reviewTarget, { group: GROUPS.REVIEW, enabled: !!reviewTarget.file });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE, reviewTarget, { group: GROUPS.REVIEW, enabled: !!reviewTarget.file });
        addAction(items, DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE, reviewTarget, { group: GROUPS.REVIEW, enabled: !!reviewTarget.file });
      }
    }

    if (messageTarget) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT, messageTarget, { group: GROUPS.CONTENT, enabled: !!messageTarget.text });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE, messageTarget, { group: GROUPS.CONTENT, enabled: !!messageTarget.messageId && messageTarget.canFork });
    }

    if (toolTarget) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT, toolTarget, { group: GROUPS.CONTENT, enabled: !!toolTarget.visibleOutput });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT, toolTarget, { group: GROUPS.CONTENT, enabled: !!toolTarget.fullOutput });
      addAction(items, toolTarget.expanded ? DESKTOP_CONTEXT_ACTIONS.COLLAPSE_TOOL : DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL, toolTarget, {
        group: GROUPS.CONTENT,
        enabled: toolTarget.canToggle !== false,
      });
    }

    if (attachmentTarget) {
      if (!attachmentTarget.mutable) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE, attachmentTarget, {
          group: GROUPS.CONTENT,
          enabled: !!attachmentTarget.copyImageUrl,
        });
      }
      addAction(items, DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT, attachmentTarget, {
        group: GROUPS.CONTENT,
        enabled: !!attachmentTarget.previewUrl,
      });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME, attachmentTarget, { group: GROUPS.CONTENT, enabled: !!attachmentTarget.name });
      addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL, attachmentTarget, {
        group: GROUPS.CONTENT,
        enabled: !!(attachmentTarget.url || attachmentTarget.path),
      });
      if (attachmentTarget.mutable) {
        addAction(items, DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT, attachmentTarget, {
          group: GROUPS.DANGER,
          danger: true,
          confirm: '移除这个待发送附件？',
        });
      }
    }

    const explorerTarget = openInExplorerTarget || (openInExplorer ? { path: '', kind: '' } : null);
    if (explorerTarget && !items.some((item) => item.id === DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER)) {
      addAction(items, DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER, explorerTarget);
    }
  }

  addAction(items, DESKTOP_CONTEXT_ACTIONS.SELECT_ALL, null, { group: GROUPS.GENERIC });
  if (hasSelection) addAction(items, DESKTOP_CONTEXT_ACTIONS.COPY, null, { group: GROUPS.GENERIC });
  if (editable) {
    addAction(items, DESKTOP_CONTEXT_ACTIONS.PASTE, null, { group: GROUPS.GENERIC });
    addAction(items, DESKTOP_CONTEXT_ACTIONS.CUT, null, { group: GROUPS.GENERIC });
  }
  if (debug) addAction(items, DESKTOP_CONTEXT_ACTIONS.INSPECT, null, { group: GROUPS.DEBUG });

  return withMenuSeparators(items);
}

export function openInExplorerTargetFromElement(target) {
  const el = closest(target, OPEN_IN_EXPLORER_TARGET_SELECTOR);
  if (!el) return null;
  const path = getAttr(el, 'data-desktop-open-in-explorer-path', 'desktopOpenInExplorerPath');
  if (!path) return null;
  const kind = getAttr(el, 'data-desktop-open-in-explorer-kind', 'desktopOpenInExplorerKind');
  return { path, kind };
}

export function sessionPinTargetFromElement(target) {
  const session = sessionTargetFromElement(target);
  return session ? { sessionId: session.sessionId, workspaceHash: session.workspaceHash, pinned: session.pinned } : null;
}

export function sessionTargetFromElement(target) {
  const el = closest(target, SESSION_TARGET_SELECTOR);
  if (!el) return null;
  const sessionId = getAttr(el, 'data-desktop-session-id', 'desktopSessionId');
  if (!sessionId) return null;
  return {
    type: 'session',
    sessionId,
    workspaceHash: getAttr(el, 'data-desktop-session-workspace', 'desktopSessionWorkspace'),
    title: getAttr(el, 'data-desktop-session-title', 'desktopSessionTitle'),
    pinned: boolAttr(el, 'data-desktop-session-pinned', 'desktopSessionPinned'),
    canArchive: boolAttr(el, 'data-desktop-session-archive', 'desktopSessionArchive'),
  };
}

export function workspaceTargetFromElement(target) {
  const el = closest(target, WORKSPACE_TARGET_SELECTOR);
  if (!el) return null;
  const workspaceHash = getAttr(el, 'data-desktop-workspace-id', 'desktopWorkspaceId');
  if (!workspaceHash) return null;
  return {
    type: 'workspace',
    workspaceHash,
    name: getAttr(el, 'data-desktop-workspace-name', 'desktopWorkspaceName'),
    path: getAttr(el, 'data-desktop-workspace-path', 'desktopWorkspacePath')
      || getAttr(el, 'data-desktop-open-in-explorer-path', 'desktopOpenInExplorerPath'),
    active: boolAttr(el, 'data-desktop-workspace-active', 'desktopWorkspaceActive'),
    expanded: boolAttr(el, 'data-desktop-workspace-expanded', 'desktopWorkspaceExpanded'),
    canRename: getAttr(el, 'data-desktop-workspace-rename', 'desktopWorkspaceRename') !== 'false',
    canRemove: boolAttr(el, 'data-desktop-workspace-remove', 'desktopWorkspaceRemove'),
    opencodeImportCount: numberAttr(el, 'data-desktop-workspace-opencode-import-count', 'desktopWorkspaceOpencodeImportCount'),
  };
}

export function fileTargetFromElement(target) {
  const el = closest(target, FILE_TARGET_SELECTOR);
  if (!el) return null;
  const relativePath = getAttr(el, 'data-desktop-file-path', 'desktopFilePath');
  if (!relativePath) return null;
  return {
    type: 'file',
    relativePath,
    absolutePath: getAttr(el, 'data-desktop-file-absolute-path', 'desktopFileAbsolutePath'),
    locatePath: getAttr(el, 'data-desktop-file-locate-path', 'desktopFileLocatePath'),
    kind: getAttr(el, 'data-desktop-file-kind', 'desktopFileKind') || 'file',
    selected: boolAttr(el, 'data-desktop-file-selected', 'desktopFileSelected'),
    expanded: boolAttr(el, 'data-desktop-file-expanded', 'desktopFileExpanded'),
    canPreview: boolAttr(el, 'data-desktop-file-preview', 'desktopFilePreview'),
    canAddContext: boolAttr(el, 'data-desktop-file-add-context', 'desktopFileAddContext'),
  };
}

export function previewTargetFromElement(target) {
  const el = closest(target, PREVIEW_TARGET_SELECTOR);
  if (!el) return null;
  const path = getAttr(el, 'data-desktop-preview-path', 'desktopPreviewPath');
  if (!path) return null;
  return {
    type: 'preview',
    path,
    kind: getAttr(el, 'data-desktop-preview-kind', 'desktopPreviewKind') || 'text',
    size: getAttr(el, 'data-desktop-preview-size', 'desktopPreviewSize'),
    contentType: getAttr(el, 'data-desktop-preview-content-type', 'desktopPreviewContentType'),
    copyImageUrl: getAttr(el, 'data-desktop-preview-copy-image-url', 'desktopPreviewCopyImageUrl'),
  };
}

export function previewTabTargetFromElement(target) {
  const el = closest(target, PREVIEW_TAB_TARGET_SELECTOR);
  if (!el) return null;
  const key = getAttr(el, 'data-desktop-preview-tab-key', 'desktopPreviewTabKey');
  if (!key) return null;
  return {
    type: 'preview-tab',
    key,
    tabType: getAttr(el, 'data-desktop-preview-tab-type', 'desktopPreviewTabType') || 'file',
    relativePath: getAttr(el, 'data-desktop-preview-tab-path', 'desktopPreviewTabPath'),
    absolutePath: getAttr(el, 'data-desktop-preview-tab-absolute-path', 'desktopPreviewTabAbsolutePath'),
    hasOthers: boolAttr(el, 'data-desktop-preview-tab-has-others', 'desktopPreviewTabHasOthers'),
    hasRight: boolAttr(el, 'data-desktop-preview-tab-has-right', 'desktopPreviewTabHasRight'),
  };
}

export function reviewTargetFromElement(target) {
  const el = closest(target, REVIEW_TARGET_SELECTOR);
  if (!el) return null;
  const kind = getAttr(el, 'data-desktop-review-kind', 'desktopReviewKind');
  if (!kind) return null;
  return {
    type: 'review',
    kind,
    file: getAttr(el, 'data-desktop-review-file', 'desktopReviewFile'),
    canRefresh: !!closest(target, '[data-desktop-review-refresh="true"]'),
  };
}

export function messageTargetFromElement(target) {
  const el = closest(target, MESSAGE_TARGET_SELECTOR);
  if (!el) return null;
  const role = getAttr(el, 'data-desktop-message-role', 'desktopMessageRole');
  if (!role) return null;
  return {
    type: 'message',
    role,
    messageId: getAttr(el, 'data-desktop-message-id', 'desktopMessageId'),
    text: getAttr(el, 'data-desktop-message-text', 'desktopMessageText'),
    canFork: boolAttr(el, 'data-desktop-message-can-fork', 'desktopMessageCanFork'),
  };
}

export function toolTargetFromElement(target) {
  const el = closest(target, TOOL_TARGET_SELECTOR);
  if (!el) return null;
  const id = getAttr(el, 'data-desktop-tool-id', 'desktopToolId');
  if (!id) return null;
  return {
    type: 'tool',
    id,
    name: getAttr(el, 'data-desktop-tool-name', 'desktopToolName'),
    visibleOutput: getAttr(el, 'data-desktop-tool-visible-output', 'desktopToolVisibleOutput'),
    fullOutput: getAttr(el, 'data-desktop-tool-full-output', 'desktopToolFullOutput'),
    expanded: boolAttr(el, 'data-desktop-tool-expanded', 'desktopToolExpanded'),
    canToggle: getAttr(el, 'data-desktop-tool-toggle', 'desktopToolToggle') !== 'false',
  };
}

export function attachmentTargetFromElement(target) {
  const el = closest(target, ATTACHMENT_TARGET_SELECTOR);
  if (!el) return null;
  const id = getAttr(el, 'data-desktop-attachment-id', 'desktopAttachmentId');
  if (!id) return null;
  return {
    type: 'attachment',
    id,
    name: getAttr(el, 'data-desktop-attachment-name', 'desktopAttachmentName'),
    url: getAttr(el, 'data-desktop-attachment-url', 'desktopAttachmentUrl'),
    path: getAttr(el, 'data-desktop-attachment-path', 'desktopAttachmentPath'),
    previewUrl: getAttr(el, 'data-desktop-attachment-preview-url', 'desktopAttachmentPreviewUrl'),
    copyImageUrl: getAttr(el, 'data-desktop-attachment-copy-image-url', 'desktopAttachmentCopyImageUrl'),
    mimeType: getAttr(el, 'data-desktop-attachment-mime-type', 'desktopAttachmentMimeType'),
    kind: getAttr(el, 'data-desktop-attachment-kind', 'desktopAttachmentKind'),
    mutable: boolAttr(el, 'data-desktop-attachment-mutable', 'desktopAttachmentMutable'),
  };
}

export function contextTargetsFromElement(target) {
  return {
    openInExplorerTarget: openInExplorerTargetFromElement(target),
    sessionTarget: sessionTargetFromElement(target),
    workspaceTarget: workspaceTargetFromElement(target),
    fileTarget: fileTargetFromElement(target),
    previewTarget: previewTargetFromElement(target),
    previewTabTarget: previewTabTargetFromElement(target),
    reviewTarget: reviewTargetFromElement(target),
    messageTarget: messageTargetFromElement(target),
    toolTarget: toolTargetFromElement(target),
    attachmentTarget: attachmentTargetFromElement(target),
  };
}

export function joinWorkspacePath(cwd = '', relativePath = '') {
  const base = String(cwd || '').replace(/\\/g, '/').replace(/[\\/]+$/g, '');
  const rawRel = String(relativePath || '').replace(/\\/g, '/');
  const normalizedRel = normalizeTreePath(rawRel);
  if (/^[A-Za-z]:\//.test(normalizedRel)) return normalizedRel;
  const rel = rawRel.replace(/^[\\/]+/g, '');
  if (!base) return rel;
  return rel ? `${base}/${rel}` : base;
}

export function containingWorkspacePath(cwd = '', relativePath = '') {
  const rel = String(relativePath || '').replace(/\\/g, '/');
  const idx = rel.lastIndexOf('/');
  const parent = idx >= 0 ? rel.slice(0, idx) : '';
  return joinWorkspacePath(cwd, parent);
}

export function clampContextMenuPosition({
  x,
  y,
  width,
  height,
  viewportWidth,
  viewportHeight,
  margin = 6,
}) {
  const safeWidth = Math.max(0, Number(width) || 0);
  const safeHeight = Math.max(0, Number(height) || 0);
  const safeViewportWidth = Math.max(0, Number(viewportWidth) || 0);
  const safeViewportHeight = Math.max(0, Number(viewportHeight) || 0);
  const maxLeft = Math.max(margin, safeViewportWidth - safeWidth - margin);
  const maxTop = Math.max(margin, safeViewportHeight - safeHeight - margin);
  return {
    left: Math.min(Math.max(Number(x) || margin, margin), maxLeft),
    top: Math.min(Math.max(Number(y) || margin, margin), maxTop),
  };
}
