// SlashDropdown 排序与首段解析的纯函数测试。
//
// 覆盖 design.md D4 的加权规则与首段 chip 解析:
//   - 空查询保留 flatten 原顺序(builtin 先,skill 字典序)
//   - name 前缀 > name 子串 > desc 子串
//   - 完全不匹配过滤
//   - case-insensitive
//   - parseLeadingCommand:命中 / 未命中(命令名未知)/ 含空格 / 空串

import assert from 'node:assert/strict';
import { rankCommands, flattenCommands, parseLeadingCommand, parseExecutableBuiltinCommand } from './slashCommands.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const ITEMS = flattenCommands({
  builtins: [
    { name: 'init', description: 'Generate ACECODE.md' },
    { name: 'compact', description: 'Compress conversation history' },
  ],
  skills: [
    { name: 'code-review', description: 'Review code changes' },
    { name: 'frontend-design', description: 'Design UI components' },
    { name: 'init-helper', description: 'Help with init scaffolding' },
  ],
});

run('flattenCommands 注入 kind 字段并保留 builtin/skill 顺序', () => {
  assert.equal(ITEMS.length, 5);
  assert.equal(ITEMS[0].kind, 'builtin');
  assert.equal(ITEMS[0].name, 'init');
  assert.equal(ITEMS[1].kind, 'builtin');
  assert.equal(ITEMS[2].kind, 'skill');
  assert.equal(ITEMS[2].name, 'code-review');
});

run('空查询返回原 flatten 顺序(builtin 在前)', () => {
  const r = rankCommands('', ITEMS);
  assert.equal(r.length, 5);
  assert.equal(r[0].name, 'init');
  assert.equal(r[1].name, 'compact');
  assert.equal(r[2].name, 'code-review');
});

run('name 前缀匹配 > name 子串 > description 子串', () => {
  // "init" 命中:init(前缀+1000)、init-helper(前缀+1000)、code-review(无)、
  // frontend-design(无),compact(无)
  // 同分按字典序:"init" < "init-helper"
  const r = rankCommands('init', ITEMS);
  assert.equal(r.length, 2);
  assert.equal(r[0].name, 'init');
  assert.equal(r[1].name, 'init-helper');
});

run('description 子串匹配在 name 不命中时仍然被收录', () => {
  // "history" 只在 compact 的 description 里出现 → 该项分数 100
  const r = rankCommands('history', ITEMS);
  assert.equal(r.length, 1);
  assert.equal(r[0].name, 'compact');
});

run('不区分大小写', () => {
  const r = rankCommands('INIT', ITEMS);
  assert.equal(r.length, 2);
  assert.equal(r[0].name, 'init');
});

run('完全不匹配过滤', () => {
  const r = rankCommands('xyz123', ITEMS);
  assert.equal(r.length, 0);
});

run('name 子串非前缀的命中', () => {
  // "review" 不是任何 name 的前缀,但是 code-review 包含子串 → 分数 500
  const r = rankCommands('review', ITEMS);
  assert.equal(r.length, 1);
  assert.equal(r[0].name, 'code-review');
});

run('parseLeadingCommand:已知命令名命中', () => {
  const r = parseLeadingCommand('/init 看一下', ['init', 'compact']);
  assert.equal(r.name, 'init');
  assert.equal(r.headLength, 5); // "/init"
});

run('parseLeadingCommand:首段是未知名 → name null,但 headLength 仍计算', () => {
  const r = parseLeadingCommand('/foobar test', ['init']);
  assert.equal(r.name, null);
  assert.equal(r.headLength, 7); // "/foobar"
});

run('parseLeadingCommand:无空格的纯首段', () => {
  const r = parseLeadingCommand('/init', ['init']);
  assert.equal(r.name, 'init');
  assert.equal(r.headLength, 5);
});

run('parseLeadingCommand:不以 / 开头返回 null + headLength=0', () => {
  const r = parseLeadingCommand('hello', ['init']);
  assert.equal(r.name, null);
  assert.equal(r.headLength, 0);
});

run('parseLeadingCommand:空串安全', () => {
  const r = parseLeadingCommand('', ['init']);
  assert.equal(r.name, null);
  assert.equal(r.headLength, 0);
});

run('parseLeadingCommand:tab 与换行也算空白边界', () => {
  const r1 = parseLeadingCommand('/init\thello', ['init']);
  assert.equal(r1.name, 'init');
  assert.equal(r1.headLength, 5);
  const r2 = parseLeadingCommand('/init\nnext', ['init']);
  assert.equal(r2.name, 'init');
  assert.equal(r2.headLength, 5);
});

run('parseExecutableBuiltinCommand:只识别 init 和 compact', () => {
  assert.deepEqual(parseExecutableBuiltinCommand('/init'), {
    name: 'init',
    args: '',
    display_text: '/init',
  });
  assert.deepEqual(parseExecutableBuiltinCommand('/compact now'), {
    name: 'compact',
    args: 'now',
    display_text: '/compact now',
  });
  assert.equal(parseExecutableBuiltinCommand('/code-review check this'), null);
  assert.equal(parseExecutableBuiltinCommand('/unknown'), null);
  assert.equal(parseExecutableBuiltinCommand('plain text'), null);
});
