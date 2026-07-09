// gitChanges.js 的单元测试(Node + node:assert)。
//
// 覆盖:
//  - 基线候选:有/无 remote 默认分支
//  - 列表缓存:命中 / markStale 失效 / 30s 过期 / stale 旧数据仍可读
//  - patch LRU:上限 20、touch 保鲜、markStale 清空
//  - shouldFetchList:不可见永不拉
//  - 行展示模型:±行数 / 二进制 / untracked "new"
//  - 汇总文案:截断提示

import assert from 'node:assert/strict';
import {
  buildBaseCandidates,
  createChangesCache,
  shouldFetchList,
  buildChangeRow,
  buildSummaryLabel,
  PATCH_LRU_LIMIT,
  STALE_AFTER_MS,
} from './gitChanges.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('基线候选:后端已验证的 default_base 优先,HEAD 兜底', () => {
  const { candidates, initial } = buildBaseCandidates({
    is_repo: true, default_branch: 'master', default_base: 'origin/master',
  });
  assert.deepEqual(candidates, ['origin/master', 'HEAD']);
  assert.equal(initial, 'origin/master');
});

run('基线候选:非仓库 / 无 default_base → 只有 HEAD', () => {
  assert.deepEqual(buildBaseCandidates(null).candidates, ['HEAD']);
  const noBase = buildBaseCandidates({ is_repo: true, default_base: '' });
  assert.deepEqual(noBase.candidates, ['HEAD']);
});

// 回归:纯本地仓库(无 origin remote / 从未 fetch)。后端 default_branch
// 兜底 "main" 但 default_base 为空 —— 修复前前端用 default_branch 自拼
// origin/main 当初始基线,/api/git/changes 对不存在的 ref 报 400,变更
// 面板整体「加载失败:invalid base」。期望:忽略 default_branch,基线
// 退回 HEAD。
run('基线候选:default_branch 兜底值不得被拼成 origin/<def>', () => {
  const { candidates, initial } = buildBaseCandidates({
    is_repo: true, default_branch: 'main', default_base: '',
  });
  assert.deepEqual(candidates, ['HEAD']);
  assert.equal(initial, 'HEAD');
});

// 回归:老后端(无 default_base 字段)混新前端。期望:字段缺失按无
// remote 处理退回 HEAD,不猜不拼。
run('基线候选:info 缺 default_base 字段 → 退回 HEAD', () => {
  const { initial } = buildBaseCandidates({
    is_repo: true, default_branch: 'main',
  });
  assert.equal(initial, 'HEAD');
});

run('列表缓存:put 后命中,markStale 后 miss 但 stale 旧数据可读', () => {
  let t = 1000;
  const cache = createChangesCache(() => t);
  cache.putList('/ws', 'HEAD', { files: [] });
  assert.ok(cache.getList('/ws', 'HEAD'));
  cache.markStale('/ws');
  assert.equal(cache.getList('/ws', 'HEAD'), null);
  assert.ok(cache.getListEvenIfStale('/ws', 'HEAD'));
});

run('列表缓存:超 30s 过期', () => {
  let t = 1000;
  const cache = createChangesCache(() => t);
  cache.putList('/ws', 'HEAD', { files: [] });
  t += STALE_AFTER_MS + 1;
  assert.equal(cache.getList('/ws', 'HEAD'), null);
});

run('列表缓存:markStale 只影响同 cwd', () => {
  const cache = createChangesCache(() => 0);
  cache.putList('/a', 'HEAD', { files: [] });
  cache.putList('/b', 'HEAD', { files: [] });
  cache.markStale('/a');
  assert.equal(cache.getList('/a', 'HEAD'), null);
  assert.ok(cache.getList('/b', 'HEAD'));
});

run('patch LRU:上限淘汰最旧、touch 保鲜、markStale 清空', () => {
  const cache = createChangesCache(() => 0);
  for (let i = 0; i < PATCH_LRU_LIMIT; i++) {
    cache.putPatch('/ws', 'HEAD', `f${i}.txt`, `patch${i}`);
  }
  // touch f0 让它变"最新",再塞一个 → 被淘汰的应是 f1。
  assert.equal(cache.getPatch('/ws', 'HEAD', 'f0.txt'), 'patch0');
  cache.putPatch('/ws', 'HEAD', 'overflow.txt', 'p');
  assert.equal(cache.getPatch('/ws', 'HEAD', 'f1.txt'), null);
  assert.equal(cache.getPatch('/ws', 'HEAD', 'f0.txt'), 'patch0');

  cache.markStale('/ws');
  assert.equal(cache.patchCount(), 0);
});

run('shouldFetchList:不可见永不拉;可见且无缓存才拉', () => {
  assert.equal(shouldFetchList({ visible: false, cachedAvailable: false }), false);
  assert.equal(shouldFetchList({ visible: true, cachedAvailable: true }), false);
  assert.equal(shouldFetchList({ visible: true, cachedAvailable: false }), true);
});

run('行展示模型:tracked ±行数 / 二进制 bin / untracked new', () => {
  assert.equal(buildChangeRow({ path: 'a', status: 'M', additions: 3, deletions: 1 }).statLabel, '+3 -1');
  assert.equal(buildChangeRow({ path: 'b', status: 'M', binary: true }).statLabel, 'bin');
  assert.equal(buildChangeRow({ path: 'c', status: 'A' }).statLabel, 'new');
});

run('汇总文案:常规与截断', () => {
  assert.equal(
    buildSummaryLabel({ files: [1, 2], total_count: 2, truncated: false }),
    '2 个文件已更改');
  assert.equal(
    buildSummaryLabel({ files: new Array(200), total_count: 350, truncated: true }),
    '350 个文件已更改(仅列出前 200 个)');
});

console.log('gitChanges.test.js: all tests passed');
