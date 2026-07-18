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

function expectInOrder(text, markers) {
  let cursor = -1;
  for (const marker of markers) {
    const index = text.indexOf(marker, cursor + 1);
    assert.notEqual(index, -1, `missing marker: ${marker}`);
    assert.ok(index > cursor, `marker is out of order: ${marker}`);
    cursor = index;
  }
}

run('global, workspace, and active-session composers share integrated session controls', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');
  const sessionControlProps = chatView.match(/sessionControls=\{\{/g) || [];
  const gitPills = chatView.match(/<GitSessionPill/g) || [];

  assert.equal(sessionControlProps.length, 2);
  assert.equal(gitPills.length, 2);
  assert.match(chatView, /const homeTokenBudget = useMemo\(\(\) => normalizeTokenBudget\(\{/);
  assert.match(chatView, /tokenBudget: homeTokenBudget/);
  assert.match(chatView, /tokenBudget,\s+permissionMode,/);
  assert.match(inputBar, /textareaBaseHeight = LINE_HEIGHT \* 2 \+ textareaVerticalPadding/);
  assert.doesNotMatch(chatView, /import \{ StatusBar \}/);
  assert.doesNotMatch(chatView, /<StatusBar/);
});

run('composer footer preserves required left-to-right control order', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const footer = component.slice(component.indexOf('data-composer-session-controls="true"'));

  expectInOrder(footer, [
    'data-composer-control="add-context"',
    'data-composer-control="permission"',
    'data-composer-control="selected-contexts"',
    '<ModelLoadIndicator load={modelLoad} />',
    'data-composer-control="token-budget"',
    'data-composer-control="model"',
    'data-composer-control="submit"',
  ]);
});

run('selected contexts remain horizontally accessible while fixed controls do not shrink', () => {
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-context-strip\s*\{[^}]*flex: 1 1 auto;[^}]*overflow-x: auto;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-session-right\s*\{[^}]*flex-shrink: 0;/s,
  );
  assert.match(styles, /@container \(max-width: 560px\)/);
});

run('composer permission and model selectors share a 13px label size', () => {
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-control-button\s*\{[^}]*font-size: 13px;/s,
  );
});

run('model selector leaves enough line height for lowercase descenders', () => {
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-model-label\s*\{[^}]*line-height: 1\.35;/s,
  );
});

run('model refresh lives in the integrated model menu', () => {
  const component = source('components/ComposerSessionControls.jsx');

  expectInOrder(component, [
    'ace-composer-model-menu-header',
    'onRefreshModels &&',
    'aria-label="刷新模型列表"',
    'ace-composer-model-options',
  ]);
});

run('model selector shows its label without the legacy A glyph', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const styles = source('styles/globals.css');

  assert.doesNotMatch(component, /ModelGlyph|ace-composer-model-glyph/);
  assert.doesNotMatch(styles, /\.ace-composer-model-glyph/);
});
