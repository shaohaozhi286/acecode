import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = (relativePath) => fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('expert components page uses local CRUD and shared immediate Modal', () => {
  const page = source('components/ExpertComponentsPage.jsx');
  assert.match(page, /data-expert-components-page="true"/);
  assert.match(page, /api\.listExperts/);
  assert.match(page, /api\.createExpert/);
  assert.match(page, /api\.updateExpert/);
  assert.match(page, /api\.deleteExpert/);
  assert.match(page, /<Modal onClose=/);
  assert.match(page, /data-expert-delete-dialog="true"/);
  assert.doesNotMatch(page, /window\.confirm|window\.alert/);
});

test('expert teams reuse the expert gallery and keep technical details out of the UI', () => {
  const page = source('components/ExpertComponentsPage.jsx');
  assert.match(page, />\s*新建专家\s*</);
  assert.match(page, />\s*组建专家团\s*</);
  assert.match(page, /data-team-expert-picker="true"/);
  assert.match(page, /和其他专家组团/);
  assert.match(page, /返回专家团/);
  assert.match(page, /设为主理人/);
  assert.doesNotMatch(page, /添加成员|updateMember|成员指令/);
  assert.doesNotMatch(page, /Skills|MCP|skill_roots|package_root|expert\.json|font-mono/);
});

test('new composer sends expert binding and existing sessions lock it', () => {
  const chat = source('components/ChatView.jsx');
  const controls = source('components/ComposerSessionControls.jsx');
  assert.match(chat, /expert_id: homeExpertId/);
  assert.match(chat, /expertLocked: false/);
  assert.match(chat, /expertLocked: true/);
  assert.match(controls, /当前会话已绑定专家组件/);
  assert.match(controls, /disabled=\{expertLocked\}/);
});

test('app routes expert entry to page and use action back to a new task', () => {
  const app = source('App.jsx');
  assert.match(app, /expertComponents: true/);
  assert.match(app, /<ExpertComponentsPage/);
  assert.match(app, /expertId: expert\?\.id \|\| ''/);
  assert.match(app, /!activeRef\?\.expertComponents/);
});
