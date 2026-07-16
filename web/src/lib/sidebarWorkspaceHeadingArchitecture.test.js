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
  assert.match(sidebar, /data-tour-target="sidebar-add-project"/);
  assert.match(styles, /\.ace-sidebar-section-actions\s*\{\s*opacity: 0;\s*pointer-events: none;/);
  assert.match(
    styles,
    /\.ace-sidebar-section-header:hover \.ace-sidebar-section-actions,\s*\.ace-sidebar-section-header:focus-within \.ace-sidebar-section-actions\s*\{\s*opacity: 1;\s*pointer-events: auto;/,
  );
});
