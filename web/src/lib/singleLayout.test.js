import assert from 'node:assert/strict';
import {
  MIN_CHAT_WIDTH,
  MIN_PREVIEW_PANEL_WIDTH,
  MIN_SINGLE_SHELL_WIDTH,
  MIN_SIDEBAR_WIDTH,
  MIN_SIDE_PANEL_WIDTH,
  DEFAULT_SINGLE_LAYOUT,
  normalizePreviewPanelWidth,
  normalizeSidebarWidth,
  normalizeSidePanelWidth,
  solveSingleContentLayout,
  validateLayoutWidths,
} from './singleLayout.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('desktop chat column minimum width is 350px', () => {
  assert.equal(MIN_CHAT_WIDTH, 350);
});

run('single-session shell minimum width covers sidebar, chat, preview, and side panel', () => {
  assert.equal(
    MIN_SINGLE_SHELL_WIDTH,
    MIN_SIDEBAR_WIDTH + MIN_CHAT_WIDTH + MIN_PREVIEW_PANEL_WIDTH + MIN_SIDE_PANEL_WIDTH,
  );
});

run('side panel can expand until the chat column reaches its minimum width', () => {
  assert.equal(normalizeSidePanelWidth(1200, 1400), 1050);
});

run('side panel no longer has a fixed 560px maximum width', () => {
  assert.equal(normalizeSidePanelWidth(900, 1200), 850);
});

run('side panel keeps its own minimum width on narrow content', () => {
  assert.equal(normalizeSidePanelWidth(100, 500), MIN_SIDE_PANEL_WIDTH);
});

run('side panel resize reserves chat and preview minimum widths when preview is visible', () => {
  assert.equal(normalizeSidePanelWidth(900, {
    contentWidth: 1400,
    previewPanelVisible: true,
  }), 630);
});

run('preview resize reserves the current right side panel width', () => {
  assert.equal(normalizePreviewPanelWidth(1200, {
    contentWidth: 1400,
    sidePanelWidth: 360,
    sidePanelVisible: true,
  }), 690);
});

run('layout validation accepts legacy widths without previewPanel', () => {
  assert.equal(validateLayoutWidths({ sidebar: 270, sidePanel: 280 }), true);
  assert.equal(validateLayoutWidths(DEFAULT_SINGLE_LAYOUT), true);
});

run('sidebar resize reserves the side panel and chat minimum widths', () => {
  assert.equal(normalizeSidebarWidth(360, {
    shellWidth: 1200,
    sidePanelWidth: 700,
    sidePanelVisible: true,
  }), 160);
});

run('sidebar resize reserves preview panel width when visible', () => {
  assert.equal(normalizeSidebarWidth(360, {
    shellWidth: 1200,
    sidePanelWidth: 280,
    sidePanelVisible: true,
    previewPanelWidth: 640,
    previewPanelVisible: true,
  }), MIN_SIDEBAR_WIDTH);
});

run('single content layout opens preview by first compressing chat', () => {
  const result = solveSingleContentLayout({
    contentWidth: 1400,
    sidePanelWidth: 280,
    previewPanelWidth: 640,
    previewPanelVisible: true,
  });
  assert.deepEqual(result, {
    chatWidth: 480,
    previewPanelWidth: 640,
    sidePanelWidth: 280,
  });
});

run('single content layout keeps side panel stable and shrinks preview before chat minimum breaks', () => {
  const result = solveSingleContentLayout({
    contentWidth: 1200,
    sidePanelWidth: 360,
    previewPanelWidth: 640,
    previewPanelVisible: true,
  });
  assert.deepEqual(result, {
    chatWidth: MIN_CHAT_WIDTH,
    previewPanelWidth: 490,
    sidePanelWidth: 360,
  });
});

run('single content layout keeps side panel stable when preview reaches minimum', () => {
  const result = solveSingleContentLayout({
    contentWidth: 950,
    sidePanelWidth: 360,
    previewPanelWidth: 640,
    previewPanelVisible: true,
  });
  assert.deepEqual(result, {
    chatWidth: 170,
    previewPanelWidth: MIN_PREVIEW_PANEL_WIDTH,
    sidePanelWidth: 360,
  });
});

run('single content layout never borrows width from side panel even below minimum shell width', () => {
  const result = solveSingleContentLayout({
    contentWidth: 900,
    sidePanelWidth: 360,
    previewPanelWidth: 640,
    previewPanelVisible: true,
  });
  assert.deepEqual(result, {
    chatWidth: 120,
    previewPanelWidth: MIN_PREVIEW_PANEL_WIDTH,
    sidePanelWidth: 360,
  });
});

run('single content layout maximizes preview into chat work area while keeping side panel', () => {
  const result = solveSingleContentLayout({
    contentWidth: 1400,
    sidePanelWidth: 280,
    previewPanelWidth: 640,
    previewPanelVisible: true,
    previewPanelMaximized: true,
  });
  assert.deepEqual(result, {
    chatWidth: 0,
    previewPanelWidth: 1120,
    sidePanelWidth: 280,
  });
});

run('single content layout hides preview and gives width back to chat', () => {
  const result = solveSingleContentLayout({
    contentWidth: 1000,
    sidePanelWidth: 280,
    previewPanelWidth: 640,
    previewPanelVisible: false,
  });
  assert.deepEqual(result, {
    chatWidth: 720,
    previewPanelWidth: 0,
    sidePanelWidth: 280,
  });
});
