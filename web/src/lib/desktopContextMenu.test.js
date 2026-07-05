import assert from 'node:assert/strict';
import {
  buildDesktopContextMenuItems,
  clampContextMenuPosition,
  contextMenuOpenDelay,
  contextTargetsFromElement,
  DESKTOP_CONTEXT_ACTIONS,
  CONTEXT_MENU_REOPEN_DELAY_MS,
  OPEN_IN_EXPLORER_TARGET_SELECTOR,
  SESSION_PIN_TARGET_SELECTOR,
  WORKSPACE_TARGET_SELECTOR,
  FILE_TARGET_SELECTOR,
  PREVIEW_TARGET_SELECTOR,
  REVIEW_TARGET_SELECTOR,
  MESSAGE_TARGET_SELECTOR,
  TOOL_TARGET_SELECTOR,
  ATTACHMENT_TARGET_SELECTOR,
  containingWorkspacePath,
  joinWorkspacePath,
  openInExplorerTargetFromElement,
  sessionPinTargetFromElement,
  workspaceTargetFromElement,
} from './desktopContextMenu.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (err) {
    console.error(`[fail] ${name}`);
    throw err;
  }
}

function ids(items) {
  return items.map((item) => item.id);
}

function elementFor(selector, attrs) {
  return {
    closest(requested) {
      return requested === selector ? this : null;
    },
    getAttribute(name) {
      return attrs[name] || '';
    },
  };
}

test('普通区域只显示全选', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({})), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('选中文本时显示复制', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({ hasSelection: true })), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.COPY,
  ]);
});

test('预览选区把引用到聊天放在第一行', () => {
  const items = buildDesktopContextMenuItems({
    hasSelection: true,
    previewTarget: { type: 'preview', path: 'README.md', kind: 'markdown' },
  });
  assert.deepEqual(ids(items).slice(0, 3), [
    DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT,
    DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT,
    DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA,
  ]);
  assert.equal(items[0].target.path, 'README.md');
});

test('文本框显示粘贴和剪切', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({ editable: true })), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.PASTE,
    DESKTOP_CONTEXT_ACTIONS.CUT,
  ]);
});

test('debug 模式显示检查', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({ editable: true, hasSelection: true, debug: true })), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.COPY,
    DESKTOP_CONTEXT_ACTIONS.PASTE,
    DESKTOP_CONTEXT_ACTIONS.CUT,
    DESKTOP_CONTEXT_ACTIONS.INSPECT,
  ]);
});

test('目录目标显示在资源管理器中打开', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({ openInExplorer: true })), [
    DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('未置顶会话目标显示会话动作', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    sessionTarget: { sessionId: 's1', title: 'T', pinned: false, canArchive: true },
  })), [
    DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION,
    DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION,
    DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE,
    DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID,
    DESKTOP_CONTEXT_ACTIONS.PIN_SESSION,
    DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('descriptor metadata includes label keys, disabled states, and confirmations', () => {
  const items = buildDesktopContextMenuItems({
    sessionTarget: { sessionId: 's1', title: '', pinned: false, canArchive: true },
    workspaceTarget: {
      workspaceHash: 'w1',
      active: true,
      expanded: true,
      canRemove: true,
    },
    attachmentTarget: { id: 'a1', mutable: true },
  });
  const copyTitle = items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE);
  assert.equal(copyTitle.labelKey, DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE);
  assert.equal(copyTitle.enabled, false);

  const activateWorkspace = items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE);
  assert.equal(activateWorkspace.enabled, false);

  for (const action of [
    DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION,
    DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT,
  ]) {
    const item = items.find((candidate) => candidate.id === action);
    assert.equal(item.danger, true);
    assert.equal(typeof item.confirm, 'string');
    assert.equal(item.confirm.length > 0, true);
  }
});

test('已置顶会话目标显示取消置顶菜单项', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({ sessionPinTarget: { pinned: true } })), [
    DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('workspace 目标显示项目动作', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    workspaceTarget: {
      workspaceHash: 'w1',
      path: 'C:/repo',
      active: false,
      expanded: false,
      canRemove: true,
    },
  })), [
    DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION,
    DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH,
    DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
    DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('workspace 有 opencode import count 时显示导入动作', () => {
  const items = buildDesktopContextMenuItems({
    workspaceTarget: {
      workspaceHash: 'w1',
      path: 'C:/repo',
      active: false,
      expanded: false,
      opencodeImportCount: 3,
    },
  });
  assert.deepEqual(ids(items).slice(0, 4), [
    DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE,
    DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION,
    DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS,
  ]);
});

test('workspace target parses opencode import count attribute', () => {
  const target = workspaceTargetFromElement(elementFor(WORKSPACE_TARGET_SELECTOR, {
    'data-desktop-workspace-id': 'w1',
    'data-desktop-workspace-opencode-import-count': '28',
  }));
  assert.equal(target.opencodeImportCount, 28);
});

test('file and directory targets build expected actions', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    fileTarget: {
      kind: 'file',
      relativePath: 'src/a.cpp',
      absolutePath: 'C:/repo/src/a.cpp',
      locatePath: 'C:/repo/src',
    },
  })), [
    DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE,
    DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH,
    DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH,
    DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);

  assert.deepEqual(ids(buildDesktopContextMenuItems({
    fileTarget: {
      kind: 'directory',
      relativePath: 'src',
      absolutePath: 'C:/repo/src',
      expanded: true,
    },
  })), [
    DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
    DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH,
    DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH,
    DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE,
    DESKTOP_CONTEXT_ACTIONS.COLLAPSE_DIRECTORY,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('review, message, tool, and attachment targets build object actions first', () => {
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    reviewTarget: { kind: 'summary' },
    hasSelection: true,
  })), [
    DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS,
    DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS,
    DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.COPY,
  ]);
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    messageTarget: { role: 'assistant', messageId: 'm1', text: 'hello', canFork: true },
  })), [
    DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT,
    DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    toolTarget: { id: 't1', visibleOutput: 'tail', fullOutput: 'all', expanded: false },
  })), [
    DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT,
    DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT,
    DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
  assert.deepEqual(ids(buildDesktopContextMenuItems({
    attachmentTarget: { id: 'a1', name: 'a.png', url: '/blob', previewUrl: '/blob', mutable: true },
  })), [
    DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL,
    DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('聊天图片附件显示复制图片动作', () => {
  const items = buildDesktopContextMenuItems({
    attachmentTarget: {
      id: 'a1',
      name: 'a.png',
      url: '/blob',
      previewUrl: '/blob',
      copyImageUrl: '/blob',
      mimeType: 'image/png',
      kind: 'image',
      mutable: false,
    },
  });

  assert.deepEqual(ids(items), [
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE,
    DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
  assert.equal(items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE).enabled, true);
});

test('右键目标提取 open-in-explorer path 和 kind', () => {
  const element = elementFor(OPEN_IN_EXPLORER_TARGET_SELECTOR, {
    'data-desktop-open-in-explorer-path': 'C:/repo/src',
    'data-desktop-open-in-explorer-kind': 'directory',
  });
  assert.deepEqual(openInExplorerTargetFromElement(element), {
    path: 'C:/repo/src',
    kind: 'directory',
  });
});

test('没有目录元数据时不提取 open-in-explorer 目标', () => {
  assert.equal(openInExplorerTargetFromElement({ closest: () => null }), null);
});

test('右键目标提取 session pin metadata', () => {
  const element = elementFor(SESSION_PIN_TARGET_SELECTOR, {
    'data-desktop-session-id': 's1',
    'data-desktop-session-workspace': 'w1',
    'data-desktop-session-pinned': 'true',
  });
  assert.deepEqual(sessionPinTargetFromElement(element), {
    sessionId: 's1',
    workspaceHash: 'w1',
    pinned: true,
  });
});

test('contextTargetsFromElement 提取各类目标', () => {
  const workspace = elementFor(WORKSPACE_TARGET_SELECTOR, {
    'data-desktop-workspace-id': 'w1',
    'data-desktop-workspace-name': 'repo',
    'data-desktop-workspace-path': 'C:/repo',
    'data-desktop-workspace-active': 'true',
    'data-desktop-workspace-expanded': 'false',
    'data-desktop-workspace-remove': 'true',
  });
  assert.equal(contextTargetsFromElement(workspace).workspaceTarget.workspaceHash, 'w1');

  const file = elementFor(FILE_TARGET_SELECTOR, {
    'data-desktop-file-path': 'src/a.cpp',
    'data-desktop-file-absolute-path': 'C:/repo/src/a.cpp',
    'data-desktop-file-kind': 'file',
  });
  assert.equal(contextTargetsFromElement(file).fileTarget.relativePath, 'src/a.cpp');

  const preview = elementFor(PREVIEW_TARGET_SELECTOR, {
    'data-desktop-preview-path': 'README.md',
    'data-desktop-preview-kind': 'markdown',
  });
  assert.equal(contextTargetsFromElement(preview).previewTarget.kind, 'markdown');

  const review = elementFor(REVIEW_TARGET_SELECTOR, {
    'data-desktop-review-kind': 'file',
    'data-desktop-review-file': 'src/a.cpp',
  });
  assert.equal(contextTargetsFromElement(review).reviewTarget.file, 'src/a.cpp');

  const message = elementFor(MESSAGE_TARGET_SELECTOR, {
    'data-desktop-message-role': 'assistant',
    'data-desktop-message-id': 'm1',
    'data-desktop-message-text': 'hello',
    'data-desktop-message-can-fork': 'true',
  });
  assert.equal(contextTargetsFromElement(message).messageTarget.text, 'hello');

  const tool = elementFor(TOOL_TARGET_SELECTOR, {
    'data-desktop-tool-id': 't1',
    'data-desktop-tool-visible-output': 'tail',
  });
  assert.equal(contextTargetsFromElement(tool).toolTarget.visibleOutput, 'tail');

  const attachment = elementFor(ATTACHMENT_TARGET_SELECTOR, {
    'data-desktop-attachment-id': 'a1',
    'data-desktop-attachment-name': 'a.png',
    'data-desktop-attachment-copy-image-url': '/blob',
    'data-desktop-attachment-mime-type': 'image/png',
    'data-desktop-attachment-kind': 'image',
    'data-desktop-attachment-mutable': 'true',
  });
  assert.equal(contextTargetsFromElement(attachment).attachmentTarget.mutable, true);
  assert.equal(contextTargetsFromElement(attachment).attachmentTarget.copyImageUrl, '/blob');
  assert.equal(contextTargetsFromElement(attachment).attachmentTarget.mimeType, 'image/png');
  assert.equal(contextTargetsFromElement(attachment).attachmentTarget.kind, 'image');
});

test('非图片附件的复制图片动作不可用但保留元数据动作', () => {
  const items = buildDesktopContextMenuItems({
    attachmentTarget: { id: 'a1', name: 'a.txt', url: '/blob', previewUrl: '', mutable: false },
  });
  assert.deepEqual(ids(items), [
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE,
    DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME,
    DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
  assert.equal(items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE).enabled, false);
  assert.equal(items.find((item) => item.id === DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL).enabled, true);
});

test('workspace 路径和相对目录拼接', () => {
  assert.equal(joinWorkspacePath('C:/repo', ''), 'C:/repo');
  assert.equal(joinWorkspacePath('C:/repo/', 'src/tool'), 'C:/repo/src/tool');
  assert.equal(joinWorkspacePath('C:/repo\\', '\\src\\tool'), 'C:/repo/src/tool');
  assert.equal(joinWorkspacePath('C:/repo', 'C:\\repo\\src\\tool'), 'C:/repo/src/tool');
  assert.equal(containingWorkspacePath('C:/repo', 'src/tool/a.cpp'), 'C:/repo/src/tool');
  assert.equal(containingWorkspacePath('C:/repo', 'a.cpp'), 'C:/repo');
});

test('菜单位置保持在视口内', () => {
  assert.deepEqual(clampContextMenuPosition({
    x: 390,
    y: 290,
    width: 120,
    height: 80,
    viewportWidth: 400,
    viewportHeight: 300,
  }), { left: 274, top: 214 });
});

test('已有右键菜单时重开前短暂隐藏', () => {
  assert.equal(contextMenuOpenDelay(), 0);
  assert.equal(contextMenuOpenDelay({ hasVisibleMenu: true }), CONTEXT_MENU_REOPEN_DELAY_MS);
  assert.equal(contextMenuOpenDelay({ hasPendingMenu: true }), CONTEXT_MENU_REOPEN_DELAY_MS);
});
