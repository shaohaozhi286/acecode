import assert from 'node:assert/strict';
import {
  buildMcpServerList,
  countEnabledMcp,
  applyMcpToggle,
  mcpTransportLabel,
  mcpCommandLine,
} from './mcpServers.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('mcpTransportLabel maps transport to human label', () => {
  assert.equal(mcpTransportLabel('stdio'), '本地');
  assert.equal(mcpTransportLabel('sse'), 'SSE');
  assert.equal(mcpTransportLabel('http'), 'HTTP');
  // 缺省 / 未知 transport 视为本地(stdio)
  assert.equal(mcpTransportLabel(undefined), '本地');
});

run('mcpCommandLine joins stdio command with args, shows url for remote', () => {
  assert.equal(
    mcpCommandLine({ command: 'npx', args: ['-y', '@mcp/server-fs', '/path'] }),
    'npx -y @mcp/server-fs /path',
  );
  // 远程只显示 url,不拼 endpoint(与设计稿一致)
  assert.equal(
    mcpCommandLine({ transport: 'sse', url: 'https://mcp.example.com', sse_endpoint: '/sse' }),
    'https://mcp.example.com',
  );
  // 非法输入不抛
  assert.equal(mcpCommandLine(null), '');
  assert.equal(mcpCommandLine({ command: 'x', args: 'not-array' }), 'x');
});

run('buildMcpServerList preserves key order and derives enabled = !disabled', () => {
  const list = buildMcpServerList({
    filesystem: { command: 'npx', args: ['-y', '@mcp/server-fs', '/path'] },
    'remote-sse': { transport: 'sse', url: 'https://mcp.example.com' },
    'remote-http': { transport: 'http', url: 'https://mcp.example.com', disabled: true },
  });
  // 保留插入顺序(设计稿依赖用户 JSON 里的顺序)
  assert.deepEqual(list.map((s) => s.name), ['filesystem', 'remote-sse', 'remote-http']);
  // 缺省 disabled 字段 → enabled
  assert.equal(list[0].enabled, true);
  assert.equal(list[1].enabled, true);
  // disabled:true → 关闭
  assert.equal(list[2].enabled, false);
  assert.equal(list[2].transportLabel, 'HTTP');
});

run('buildMcpServerList tolerates malformed maps', () => {
  assert.deepEqual(buildMcpServerList(null), []);
  assert.deepEqual(buildMcpServerList([]), []);
  assert.deepEqual(buildMcpServerList('x'), []);
  // server 值不是对象时用空壳兜底(仍出现在列表里,视为启用)
  const list = buildMcpServerList({ broken: 'oops' });
  assert.equal(list.length, 1);
  assert.equal(list[0].name, 'broken');
  assert.equal(list[0].enabled, true);
});

run('countEnabledMcp counts only enabled rows', () => {
  const list = buildMcpServerList({
    a: { command: 'a' },
    b: { command: 'b', disabled: true },
    c: { command: 'c' },
  });
  assert.equal(countEnabledMcp(list), 2);
  assert.equal(countEnabledMcp(null), 0);
});

run('applyMcpToggle flips disabled without mutating the source', () => {
  const src = {
    a: { command: 'a' },
    b: { command: 'b', disabled: true },
  };
  // 关闭 a → 写 disabled:true
  const off = applyMcpToggle(src, 'a', false);
  assert.equal(off.a.disabled, true);
  // 启用 b → 删掉 disabled 键(保持稀疏)
  const on = applyMcpToggle(src, 'b', true);
  assert.equal('disabled' in on.b, false);
  // 源对象不被改动(回归:开关失败时要能回滚到原文本)
  assert.equal(src.a.disabled, undefined);
  assert.equal(src.b.disabled, true);
  // 未点到的 server 原样保留
  assert.deepEqual(off.b, { command: 'b', disabled: true });
});
