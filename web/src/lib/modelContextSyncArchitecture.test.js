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

run('saved model edits refresh the mounted active-session model state', () => {
  const app = source('App.jsx');
  const settings = source('components/SettingsPage.jsx');
  const chat = source('components/ChatView.jsx');

  assert.match(settings, /const updated = await api\.updateModel\(name, payloads\[0\]\);/);
  assert.match(settings, /onModelProfileUpdated\?\.\(updated\);/);
  assert.match(app, /modelProfileRevision=\{modelProfileRevision\}/);
  assert.match(
    app,
    /onModelProfileUpdated=\{\(\) => setModelProfileRevision\(\(value\) => value \+ 1\)\}/,
  );
  assert.match(chat, /\[api, modelProfileRevision, ref\?\.context_window,/);
});
