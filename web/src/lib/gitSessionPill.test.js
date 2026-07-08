// gitSessionPill.js 的单元测试(Node + node:assert,无 DOM)。
//
// 覆盖:
//  - 非 git 仓库 / 未加载 → 不可见(零占位)
//  - 新会话可交互;有消息 / 已在 worktree / busy / checkout 中 → 只读
//  - 勾 worktree 后分支下拉语义切换为 worktree-base
//  - checkout 409 dirty/busy 与普通错误的分派
//  - worktree 意图字段的生成条件

import assert from 'node:assert/strict';
import {
  buildPillModel,
  classifyCheckoutError,
  buildWorktreeIntent,
} from './gitSessionPill.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const repoInfo = {
  is_repo: true,
  branch: 'master',
  default_branch: 'master',
  branches: ['master', 'dev'],
  dirty: false,
};

run('非仓库 / 未加载 → 不可见', () => {
  assert.equal(buildPillModel({ gitInfo: null }).visible, false);
  assert.equal(buildPillModel({ gitInfo: { is_repo: false } }).visible, false);
});

run('git 仓库新会话 → 可见且可交互,默认 checkout 语义', () => {
  const m = buildPillModel({ gitInfo: repoInfo });
  assert.equal(m.visible, true);
  assert.equal(m.branch, 'master');
  assert.deepEqual(m.branches, ['master', 'dev']);
  assert.equal(m.interactive, true);
  assert.equal(m.branchSelectMeaning, 'checkout');
  assert.equal(m.started, false);
});

run('勾选 worktree → 分支下拉语义变为基线选择', () => {
  const m = buildPillModel({ gitInfo: repoInfo, worktreeChecked: true });
  assert.equal(m.branchSelectMeaning, 'worktree-base');
  assert.equal(m.worktreeChecked, true);
});

run('会话已有消息 → 只读徽标', () => {
  const m = buildPillModel({ gitInfo: repoInfo, sessionStarted: true });
  assert.equal(m.interactive, false);
  assert.equal(m.started, true);
});

run('已在 worktree 的会话 → 只读 + worktree 徽标 + 复选框呈勾选态', () => {
  const m = buildPillModel({
    gitInfo: repoInfo,
    worktreeSession: { name: 'ses-abc12345', branch: 'worktree-ses-abc12345' },
  });
  assert.equal(m.interactive, false);
  assert.equal(m.worktreeBadge, 'ses-abc12345');
  assert.equal(m.worktreeChecked, true);
});

run('busy / checkout 进行中 → 暂时禁交互', () => {
  assert.equal(buildPillModel({ gitInfo: repoInfo, busy: true }).interactive, false);
  const m = buildPillModel({ gitInfo: repoInfo, checkingOut: true });
  assert.equal(m.interactive, false);
  assert.equal(m.checkingOut, true);
});

run('checkout 409 dirty → 弹 stash 确认并携带文件列表', () => {
  const d = classifyCheckoutError(409, { error: 'dirty', files: ['a.txt', 'b.txt'] });
  assert.equal(d.kind, 'dirty');
  assert.deepEqual(d.files, ['a.txt', 'b.txt']);
});

run('checkout 409 busy → busy 提示;其它 → error + 可展示信息', () => {
  assert.equal(classifyCheckoutError(409, { error: 'busy' }).kind, 'busy');
  const e = classifyCheckoutError(409, { error: 'checkout failed', detail: 'conflict: x' });
  assert.equal(e.kind, 'error');
  assert.equal(e.message, 'conflict: x');
  const plain = classifyCheckoutError(500, null);
  assert.equal(plain.kind, 'error');
  assert.equal(plain.message, 'HTTP 500');
});

run('worktree 意图:勾选且未开始才生成;base 透传', () => {
  assert.equal(buildWorktreeIntent({ worktreeChecked: false }), null);
  assert.equal(
    buildWorktreeIntent({ worktreeChecked: true, sessionStarted: true }), null);
  assert.deepEqual(
    buildWorktreeIntent({ worktreeChecked: true, selectedBase: 'dev' }),
    { create: true, base: 'dev' });
  assert.deepEqual(
    buildWorktreeIntent({ worktreeChecked: true, selectedBase: '' }),
    { create: true });
});

console.log('gitSessionPill.test.js: all tests passed');
