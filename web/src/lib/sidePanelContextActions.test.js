import assert from 'node:assert/strict';
import {
  DESKTOP_CONTEXT_ACTIONS,
} from './desktopContextMenu.js';
import {
  SIDE_PANEL_CONTEXT_EFFECTS,
  sidePanelContextActionEffect,
} from './sidePanelContextActions.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('review 目标的预览文件打开真实文件预览', () => {
  assert.deepEqual(sidePanelContextActionEffect({
    action: DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE,
    target: { type: 'review', file: 'src\\main.cpp' },
    filesEnabled: true,
  }), {
    type: SIDE_PANEL_CONTEXT_EFFECTS.OPEN_FILE_PREVIEW,
    filePath: 'src\\main.cpp',
    normalizedFilePath: 'src/main.cpp',
  });
});

run('没有文件树时 review 预览文件不可处理', () => {
  assert.equal(sidePanelContextActionEffect({
    action: DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE,
    target: { type: 'review', file: 'src/main.cpp' },
    filesEnabled: false,
  }), null);
});

run('定位到文件树仍展开祖先目录', () => {
  assert.deepEqual(sidePanelContextActionEffect({
    action: DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE,
    target: { type: 'review', file: './src/deep/main.cpp' },
    filesEnabled: true,
  }), {
    type: SIDE_PANEL_CONTEXT_EFFECTS.LOCATE_IN_FILE_TREE,
    filePath: './src/deep/main.cpp',
    normalizedFilePath: 'src/deep/main.cpp',
  });
});
