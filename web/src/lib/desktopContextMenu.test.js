import assert from 'node:assert/strict';
import {
  buildDesktopContextMenuItems,
  clampContextMenuPosition,
  DESKTOP_CONTEXT_ACTIONS,
  OPEN_IN_EXPLORER_TARGET_SELECTOR,
  SESSION_PIN_TARGET_SELECTOR,
  joinWorkspacePath,
  openInExplorerTargetFromElement,
  sessionPinTargetFromElement,
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

test('普通区域只显示全选', () => {
  assert.deepEqual(buildDesktopContextMenuItems({}), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('选中文本时显示复制', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ hasSelection: true }), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.COPY,
  ]);
});

test('文本框显示粘贴和剪切', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ editable: true }), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.PASTE,
    DESKTOP_CONTEXT_ACTIONS.CUT,
  ]);
});

test('debug 模式显示检查', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ editable: true, hasSelection: true, debug: true }), [
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
    DESKTOP_CONTEXT_ACTIONS.COPY,
    DESKTOP_CONTEXT_ACTIONS.PASTE,
    DESKTOP_CONTEXT_ACTIONS.CUT,
    DESKTOP_CONTEXT_ACTIONS.INSPECT,
  ]);
});

test('目录目标显示在资源管理器中打开', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ openInExplorer: true }), [
    DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('未置顶会话目标显示置顶菜单项', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ sessionPinTarget: { pinned: false } }), [
    DESKTOP_CONTEXT_ACTIONS.PIN_SESSION,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('已置顶会话目标显示取消置顶菜单项', () => {
  assert.deepEqual(buildDesktopContextMenuItems({ sessionPinTarget: { pinned: true } }), [
    DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION,
    DESKTOP_CONTEXT_ACTIONS.SELECT_ALL,
  ]);
});

test('右键目标提取 open-in-explorer path 和 kind', () => {
  const element = {
    closest(selector) {
      return selector === OPEN_IN_EXPLORER_TARGET_SELECTOR ? this : null;
    },
    getAttribute(name) {
      if (name === 'data-desktop-open-in-explorer-path') return 'C:/repo/src';
      if (name === 'data-desktop-open-in-explorer-kind') return 'directory';
      return '';
    },
  };
  assert.deepEqual(openInExplorerTargetFromElement(element), {
    path: 'C:/repo/src',
    kind: 'directory',
  });
});

test('没有目录元数据时不提取 open-in-explorer 目标', () => {
  assert.equal(openInExplorerTargetFromElement({ closest: () => null }), null);
});

test('右键目标提取 session pin metadata', () => {
  const element = {
    closest(selector) {
      return selector === SESSION_PIN_TARGET_SELECTOR ? this : null;
    },
    getAttribute(name) {
      if (name === 'data-desktop-session-id') return 's1';
      if (name === 'data-desktop-session-workspace') return 'w1';
      if (name === 'data-desktop-session-pinned') return 'true';
      return '';
    },
  };
  assert.deepEqual(sessionPinTargetFromElement(element), {
    sessionId: 's1',
    workspaceHash: 'w1',
    pinned: true,
  });
});

test('workspace 路径和相对目录拼接', () => {
  assert.equal(joinWorkspacePath('C:/repo', ''), 'C:/repo');
  assert.equal(joinWorkspacePath('C:/repo/', 'src/tool'), 'C:/repo/src/tool');
  assert.equal(joinWorkspacePath('C:/repo\\', '\\src\\tool'), 'C:/repo/src/tool');
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
