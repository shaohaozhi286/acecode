import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('workspace heading actions stay mounted and reveal on pointer or keyboard intent', () => {
  const sidebar = source('components/Sidebar.jsx');
  const styles = source('styles/globals.css');

  assert.match(sidebar, /className="ace-sidebar-section-header ace-sidebar-section-text/);
  assert.match(sidebar, /data-sidebar-section-actions=\{sectionId\} className="ace-sidebar-section-actions/);
  assert.match(sidebar, /data-sidebar-collapse-all-workspaces="true"/);
  assert.match(
    sidebar,
    /setExpandedSessionLists\(\(previous\) => \(\s*expandedSessionListsAfterWorkspaceCollapseAll\(previous, workspaces\)\s*\)\);/,
  );
  assert.match(
    sidebar,
    /updateExpanded\(\(prev\) => \{\s*const next = new Set\(prev\);\s*if \(!workspaceCollapseAllRef\.current\) \{/,
  );
  assert.match(
    sidebar,
    /!revealTarget\.noWorkspace\s*&& revealTarget\.workspaceHash\s*&& !workspaceCollapseAllRef\.current/,
  );
  assert.match(sidebar, /data-tour-target="sidebar-add-project"/);
  assert.match(styles, /\.ace-sidebar-section-actions\s*\{\s*opacity: 0;\s*pointer-events: none;/);
  assert.match(
    styles,
    /\.ace-sidebar-section-header:hover \.ace-sidebar-section-actions,\s*\.ace-sidebar-section-header:focus-within \.ace-sidebar-section-actions\s*\{\s*opacity: 1;\s*pointer-events: auto;/,
  );
});

test('workspace collapse-all keeps disclosure-only reopen session lists compact', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(
    sidebar,
    /listKey\s*&& \(revealTarget\.noWorkspace \|\| !workspaceCollapseAllRef\.current\)\s*&& sessionListNeedsRevealExpansion/,
  );

  const toggleStart = sidebar.indexOf('const onToggle = (hash) => {');
  const toggleEnd = sidebar.indexOf('\n  const onActivate', toggleStart);
  assert.ok(toggleStart >= 0 && toggleEnd > toggleStart);
  const toggleSource = sidebar.slice(toggleStart, toggleEnd);
  assert.doesNotMatch(toggleSource, /workspaceCollapseAllRef\.current\s*=\s*false/);
});

test('workspace rows expose a shared menu button followed by the new-task shortcut', () => {
  const sidebar = source('components/Sidebar.jsx');
  const icons = source('components/Icon.jsx');
  const workspaceMenuSvg = source('../public/vs-icons/WorkspaceMenu.svg');
  const iconGenerator = source('../../scripts/regenerate_web_icons.mjs');
  const groupStart = sidebar.indexOf('function WorkspaceGroup({');
  const groupEnd = sidebar.indexOf('\nfunction NoWorkspaceSessionGroup(', groupStart);
  assert.ok(groupStart >= 0 && groupEnd > groupStart);

  const workspaceGroup = sidebar.slice(groupStart, groupEnd);
  const actionsStart = workspaceGroup.indexOf('data-sidebar-workspace-actions="true"');
  const actionsEnd = workspaceGroup.indexOf('\n        </span>\n      </div>', actionsStart);
  assert.ok(actionsStart >= 0 && actionsEnd > actionsStart);

  const actions = workspaceGroup.slice(actionsStart, actionsEnd);
  const menuIndex = actions.indexOf('data-sidebar-workspace-menu="true"');
  const newTaskIndex = actions.indexOf('data-sidebar-workspace-new-task="true"');
  assert.ok(menuIndex >= 0 && newTaskIndex > menuIndex);
  assert.equal((actions.match(/<button\b/g) || []).length, 2);
  assert.match(actions, /onClick=\{openWorkspaceContextMenu\}/);
  assert.match(actions, /<VsIcon name="workspaceMenu" size=\{16\}/);
  assert.match(actions, /<VsIcon name="newSession" size=\{16\}/);
  assert.doesNotMatch(actions, /<VsIcon name="(?:edit|close)"/);

  assert.match(icons, /workspaceMenu: 'WorkspaceMenu'/);
  assert.match(workspaceMenuSvg, /viewBox="0 0 16 16"/);
  assert.match(workspaceMenuSvg, /transform="matrix\(1 0 0 1 2 7\)"/);
  assert.match(workspaceMenuSvg, /d="M0 1[^"]*M5 1[^"]*M10 1/);
  assert.doesNotMatch(workspaceMenuSvg, /<(?:rect|polygon)\b|rotate\(/);
  assert.match(iconGenerator, /WorkspaceMenu: 'MoreThree'/);
  assert.match(iconGenerator, /WorkspaceMenu: `<svg[^`]*M0 1[^`]*M5 1[^`]*M10 1/);

  assert.match(
    workspaceGroup,
    /const openWorkspaceContextMenu = useCallback\(\(event\) => \{[\s\S]*event\.preventDefault\(\);[\s\S]*event\.stopPropagation\(\);[\s\S]*dispatchEvent\(new MouseEvent\('contextmenu',[\s\S]*bubbles: true,[\s\S]*cancelable: true,/,
  );
});
