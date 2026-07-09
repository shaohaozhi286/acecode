// lspStatus.js 单元测试(Node + node:assert,无 DOM)。
//
// 覆盖:
//  - 响应归一:过滤无效 server / 非法 open_files 兜底 / active 判定
//  - 指示器主体文案:0/1/多 server
//  - tooltip 行文案:root="." 与 open_files=0 时省略

import assert from 'node:assert/strict';
import {
  normalizeLspStatus,
  lspIndicatorLabel,
  lspServerLine,
} from './lspStatus.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('空 / 缺字段响应 → 未启用且不 active', () => {
  const a = normalizeLspStatus(null);
  assert.equal(a.enabled, false);
  assert.equal(a.active, false);
  assert.deepEqual(a.servers, []);
  const b = normalizeLspStatus({ enabled: true });
  assert.equal(b.enabled, true);
  assert.equal(b.active, false);
});

// 触发场景:后端返回混入缺 server_id 的脏项、open_files 为负/浮点。
// 期望行为:脏项被过滤,open_files 非正数归零、浮点向下取整。
run('归一:过滤无效 server + open_files 兜底', () => {
  const s = normalizeLspStatus({
    enabled: true,
    servers: [
      { server_id: 'clangd', root: '.', open_files: 3 },
      { server_id: '', root: 'x', open_files: 1 },        // 无名 → 丢弃
      { root: 'y', open_files: 2 },                        // 缺 server_id → 丢弃
      { server_id: 'pyright', root: 'sub', open_files: -1 }, // 负数 → 0
      { server_id: 'gopls', open_files: 2.9 },             // 浮点 → 2
    ],
  });
  assert.equal(s.active, true);
  assert.equal(s.servers.length, 3);
  assert.deepEqual(s.servers[0], { serverId: 'clangd', root: '.', openFiles: 3 });
  assert.equal(s.servers[1].openFiles, 0);
  assert.equal(s.servers[2].serverId, 'gopls');
  assert.equal(s.servers[2].openFiles, 2);
  assert.equal(s.servers[2].root, '');
});

run('指示器主体文案:单个显示名字,多个显示计数', () => {
  assert.equal(lspIndicatorLabel(normalizeLspStatus({ enabled: true, servers: [] })), '');
  assert.equal(
    lspIndicatorLabel(normalizeLspStatus({
      enabled: true, servers: [{ server_id: 'clangd', root: '.' }],
    })),
    'clangd');
  assert.equal(
    lspIndicatorLabel(normalizeLspStatus({
      enabled: true,
      servers: [{ server_id: 'clangd' }, { server_id: 'pyright' }],
    })),
    '2 个 LSP');
});

run('tooltip 行:root="." 与 open_files=0 省略', () => {
  assert.equal(lspServerLine({ serverId: 'clangd', root: '.', openFiles: 0 }), 'clangd');
  assert.equal(
    lspServerLine({ serverId: 'clangd', root: 'web', openFiles: 3 }),
    'clangd · web · 3 个文件');
  assert.equal(
    lspServerLine({ serverId: 'pyright', root: '', openFiles: 1 }),
    'pyright · 1 个文件');
});

console.log('lspStatus.test.js: all tests passed');
