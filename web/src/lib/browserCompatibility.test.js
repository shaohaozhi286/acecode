import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const componentsRoot = path.join(srcRoot, 'components');
const globalsPath = path.join(srcRoot, 'styles', 'globals.css');

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function walk(dir, exts = new Set(['.css', '.js', '.jsx'])) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      out.push(...walk(full, exts));
    } else if (exts.has(path.extname(entry.name))) {
      out.push(full);
    }
  }
  return out;
}

function stripComments(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/(^|[^:])\/\/.*$/gm, '$1');
}

run('web source avoids modern color syntax that older WebViews render poorly', () => {
  const forbidden = [
    new RegExp('color-' + 'mix\\s*\\(', 'i'),
    new RegExp('ok' + 'lch\\s*\\(', 'i'),
    new RegExp('\\bok' + 'lab\\b', 'i'),
    new RegExp('\\b' + 'lab\\s*\\(', 'i'),
    new RegExp('\\b' + 'lch\\s*\\(', 'i'),
  ];
  const offenders = [];
  for (const file of walk(srcRoot)) {
    if (path.basename(file) === 'browserCompatibility.test.js') continue;
    const text = stripComments(fs.readFileSync(file, 'utf8'));
    for (const pattern of forbidden) {
      if (pattern.test(text)) offenders.push(path.relative(srcRoot, file));
    }
  }
  assert.deepEqual([...new Set(offenders)].sort(), []);
});

run('color slash-opacity utilities have stable rgba fallbacks', () => {
  const globals = fs.readFileSync(globalsPath, 'utf8');
  const files = [
    ...walk(componentsRoot, new Set(['.jsx'])),
    path.join(srcRoot, 'App.jsx'),
  ];
  const colorSlashUtility = /(?:^|[\s'"`])((?:(?:hover|focus|focus-within):)?(?:bg|border|text|ring)-[A-Za-z0-9_-]+(?:-[A-Za-z0-9_-]+)*\/\d{1,3})(?=[\s'"`])/g;
  const missing = new Set();
  for (const file of files) {
    const text = fs.readFileSync(file, 'utf8');
    for (const match of text.matchAll(colorSlashUtility)) {
      const token = match[1];
      const selector = `.${token.replace(/:/g, '\\:').replace(/\//g, '\\/')}`;
      if (!globals.includes(selector)) {
        missing.add(`${path.relative(srcRoot, file)}:${token}`);
      }
    }
  }
  assert.deepEqual([...missing].sort(), []);
});

run('sidebar blocks static text selection but keeps editing controls selectable', () => {
  const globals = fs.readFileSync(globalsPath, 'utf8');
  assert.match(globals, /\.ace-sidebar\s*\{[^}]*[;\s]user-select:\s*none;/s);
  assert.match(
    globals,
    /\.ace-sidebar input,[\s\S]*?\.ace-sidebar \[contenteditable="true"\]\s*\{[^}]*[;\s]user-select:\s*text;/,
  );
});
