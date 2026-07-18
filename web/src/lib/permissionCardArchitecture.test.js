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

run('PermissionCard is an inline non-modal surface with explicit actions', () => {
  const card = source('components/PermissionCard.jsx');
  assert.match(card, /data-permission-card/);
  assert.match(card, />\s*拒绝\s*</);
  assert.match(card, />\s*本次会话允许\s*</);
  assert.match(card, /primaryLabel/);
  assert.match(card, /disabled=\{!pending\}/);
  assert.doesNotMatch(card, /<Modal|position:\s*['"]fixed|fixed inset|backdrop|document\.addEventListener|Escape|autoFocus|focus\(/);
});

run('PermissionCard provides resolved copy and bounded Plan/command previews', () => {
  const card = source('components/PermissionCard.jsx');
  assert.match(card, /已允许一次/);
  assert.match(card, /本次会话已允许/);
  assert.match(card, /已拒绝/);
  assert.match(card, /<details open/);
  assert.match(card, /min\(42vh, 320px\)/);
  assert.match(card, /min\(38vh, 280px\)/);
  assert.match(card, /overflow-auto/);
  assert.match(card, /break-words/);
});

run('shared Plan presentation retains specialized actions', () => {
  const presentation = source('lib/permissionRequestPresentation.js');
  assert.match(presentation, /批准计划/);
  assert.match(presentation, /进入 Plan/);
  assert.match(presentation, /hideAllowSession: isPlanEnter \|\| isPlanApproval/);
});

console.log('permissionCardArchitecture tests passed');
