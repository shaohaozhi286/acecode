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

test('sidebar restores one default-collapsed extension section with settled counts', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /acecode\.sidebarCustomSectionExpanded\.v2/);
  assert.match(sidebar, /DEFAULT_SIDEBAR_CUSTOM_EXPANDED/);
  assert.match(sidebar, /Promise\.allSettled\(\[/);
  assert.match(sidebar, /api\.listSkills\(\)/);
  assert.match(sidebar, /api\.getMcp\(\)/);
  assert.match(sidebar, /api\.listExperts\(workspaceHash \|\| '__local__'\)/);
  assert.doesNotMatch(sidebar, /api\.listModels\(\)/);
  assert.match(sidebar, /const totalCount = sidebarCustomTotalCount\(counts\)/);
  assert.match(sidebar, /<VsIcon name="extension" size=\{16\} \/>/);
  assert.match(sidebar, />扩展<\/span>/);
  assert.match(sidebar, /data-sidebar-custom-section="true"/);
});

test('custom shortcuts reuse the existing settings-section callback', () => {
  const sidebar = source('components/Sidebar.jsx');
  const app = source('App.jsx');
  assert.match(sidebar, /SIDEBAR_CUSTOM_ITEMS\.map/);
  assert.doesNotMatch(sidebar, /id: 'models'/);
  assert.match(sidebar, /onOpenSettingsSection\?\.\(item\.settingsSection\)/);
  assert.match(app, /onOpenSettingsSection=\{openSettingsSection\}/);
  assert.match(sidebar, /onOpenExpertComponents\?\.\(\)/);
  assert.match(app, /onOpenExpertComponents=\{openExpertComponents\}/);
});
