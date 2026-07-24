import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const webRoot = path.resolve(srcRoot, '..');

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

run('composer runtime depends on Slate and no longer depends on Lexical', () => {
  const packageJson = JSON.parse(fs.readFileSync(path.join(webRoot, 'package.json'), 'utf8'));
  const dependencies = packageJson.dependencies || {};
  assert.ok(dependencies.slate);
  assert.ok(dependencies['slate-react']);
  assert.ok(dependencies['slate-history']);
  assert.equal(dependencies.lexical, undefined);
  assert.equal(dependencies['@lexical/react'], undefined);

  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /from 'slate';/);
  assert.match(composer, /from 'slate-react';/);
  assert.match(composer, /from 'slate-history';/);
  assert.doesNotMatch(composer, /lexical/i);
});

run('command and path tags are Slate inline void elements with fixed order', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /editor\.isInline = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isInline\(element\)/s);
  assert.match(composer, /editor\.isVoid = \(element\) => \(\s+isComposerInlineTag\(element\) \? true : isVoid\(element\)/s);
  assert.equal((composer.match(/draggable=\{false\}/g) || []).length, 2);
  assert.match(composer, /types\.includes\('application\/x-slate-fragment'\)/);
});

run('composer command tag reuses the sent-message badge without a visible slash', () => {
  const composer = source('components/RichComposer.jsx');
  const message = source('components/Message.jsx');
  const styles = source('styles/globals.css');

  assert.match(composer, /replace\(\/\^\\\/\+\/, ''\)/);
  assert.match(composer, /className="ace-cmd-token ace-slate-inline-tag"/);
  assert.match(composer, /<CommandGlyph[^>]*className="ace-cmd-token-glyph"/s);
  assert.match(composer, /className="ace-cmd-token-name">\{displayName\}/);
  assert.match(message, /className="ace-cmd-token"/);
  assert.match(styles, /\.ace-cmd-token\s*\{/);
  assert.doesNotMatch(styles, /\.ace-rich-command-token\s*\{/);
});

run('path tags keep canonical text while using the compact badge surface', () => {
  const composer = source('components/RichComposer.jsx');
  assert.match(composer, /data-composer-inline-tag="path"/);
  assert.match(composer, /className="ace-cmd-token ace-slate-inline-tag ace-slate-path-tag"/);
  assert.match(composer, /element\?\.directory\s+\? <VsIcon name="folder"/s);
  assert.match(composer, /<FileTypeIcon path=\{path\} size=\{12\}/);
});

run('atomic deletion is routed through the plain-text tag range helper', () => {
  const composer = source('components/RichComposer.jsx');
  const model = source('lib/richComposerModel.js');
  assert.match(model, /export function composerAdjacentTagDeletionRange/);
  assert.match(composer, /composerAdjacentTagDeletionRange\(editor\.children, editor\.selection, direction\)/);
  assert.match(composer, /Transforms\.select\(editor, composerSelectionFromPlainTextRange/);
  assert.match(composer, /Transforms\.delete\(editor\)/);
});

run('attachments, contexts, and footer controls remain outside the Slate editor', () => {
  const inputBar = source('components/InputBar.jsx');
  const composer = source('components/RichComposer.jsx');
  const editorIndex = inputBar.indexOf('<RichComposer');
  const footerIndex = inputBar.indexOf('<ComposerSessionControls', editorIndex);

  assert.ok(editorIndex >= 0);
  assert.ok(footerIndex > editorIndex);
  assert.match(inputBar.slice(0, editorIndex), /selectionContextItems\.map/);
  assert.doesNotMatch(composer, /ComposerSessionControls|AttachmentStrip|ComposerSelectionCard/);
});
