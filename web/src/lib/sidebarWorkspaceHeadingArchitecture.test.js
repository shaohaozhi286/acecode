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
