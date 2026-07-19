import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('conversation title owns the find button and transcript search root', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(chatView, /onClick=\{onFindInConversation\}/);
  assert.match(chatView, /aria-label="搜索当前对话内容"/);
  assert.match(chatView, /data-conversation-find-root="true"/);
});

run('global find is controlled and searches only the conversation root', () => {
  const overlay = source('components/GlobalFindOverlay.jsx');
  assert.match(overlay, /enabled = false/);
  assert.match(overlay, /openRequest = 0/);
  assert.match(overlay, /collectFindMatches\(resolveFindRoot\(\), query\)/);
  assert.doesNotMatch(overlay, /collectFindMatches\(document\.body/);
  assert.match(overlay, /placeholder="搜索当前对话内容"/);
});

run('global find closes only when a pointer starts outside the overlay', () => {
  const overlay = source('components/GlobalFindOverlay.jsx');
  assert.match(
    overlay,
    /const onPointerDown = \(event\) => \{\s*if \(overlayRef\.current\?\.contains\(event\.target\)\) return;\s*close\(\);\s*\}/,
  );
  assert.match(overlay, /document\.addEventListener\('pointerdown', onPointerDown, true\)/);
  assert.match(overlay, /document\.removeEventListener\('pointerdown', onPointerDown, true\)/);
});

run('search palette renders task and project groups in one result sequence', () => {
  const palette = source('components/SearchPalette.jsx');
  assert.match(palette, /buildSearchResultSequence\(taskItems, projectItems\)/);
  assert.match(palette, /<span>任务<\/span>/);
  assert.match(palette, /<span>项目<\/span>/);
  assert.match(palette, /placeholder="搜索任务或项目"/);
  assert.match(palette, /onSelectWorkspace\?\.\(item\.value\)/);
  assert.match(palette, /window\.addEventListener\(SESSION_LIST_CHANGED_EVENT/);
});

run('project selection is handed from App to Sidebar activation', () => {
  const app = source('App.jsx');
  const sidebar = source('components/Sidebar.jsx');
  assert.match(app, /workspaceActivationRequest=\{workspaceActivationRequest\}/);
  assert.match(app, /onSelectWorkspace=\{handleSelectWorkspace\}/);
  assert.match(sidebar, /handledWorkspaceActivationRequestRef/);
  assert.match(sidebar, /Promise\.resolve\(onActivate\(workspace\)\)/);
  assert.match(sidebar, /SIDEBAR_SECTION_IDS\.WORKSPACES/);
});
