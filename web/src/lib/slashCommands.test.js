// SlashDropdown 排序与首段解析的纯函数测试。
//
// 覆盖 design.md D4 的加权规则与首段 chip 解析:
//   - 空查询保留 flatten 原顺序(builtin 先,skill 字典序)
//   - name 前缀 > name 子串 > desc 子串
//   - 完全不匹配过滤
//   - case-insensitive
//   - parseLeadingCommand:命中 / 未命中(命令名未知)/ 含空格 / 空串

import assert from 'node:assert/strict';
import {
  rankCommands,
  flattenCommands,
  commandsWithFallback,
  fallbackCommands,
  parseLeadingCommand,
  deleteLeadingCommandBlock,
  moveAcrossLeadingCommandBlock,
  normalizeLeadingCommandSelection,
  parseExecutableBuiltinCommand,
  resolveLeadingSlashCommand,
  slashCommandKindPresentation,
} from './slashCommands.js';

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
    { name: 'init', description: 'Generate AGENT.md' },
    { name: 'compact', description: 'Compress conversation history' },
    { name: 'goal', description: 'Manage thread goal' },
    { name: 'plan', description: 'Enter plan mode' },
  ],
  skills: [
    { name: 'code-review', description: 'Review code changes' },
    { name: 'frontend-design', description: 'Design UI components' },
    { name: 'init-helper', description: 'Help with init scaffolding' },
  ],
});

run('flattenCommands 注入 kind 字段并保留 builtin/skill 顺序', () => {
  assert.equal(ITEMS.length, 7);
  assert.equal(ITEMS[0].kind, 'builtin');
  assert.equal(ITEMS[0].name, 'init');
  assert.equal(ITEMS[1].kind, 'builtin');
  assert.equal(ITEMS[2].kind, 'builtin');
  assert.equal(ITEMS[2].name, 'goal');
  assert.equal(ITEMS[3].kind, 'builtin');
  assert.equal(ITEMS[3].name, 'plan');
  assert.equal(ITEMS[4].kind, 'skill');
  assert.equal(ITEMS[4].name, 'code-review');
});

run('fallbackCommands 返回基础 builtin 命令', () => {
  const r = fallbackCommands();
  assert.equal(r.length, 10);
  assert.ok(r.every((x) => x.kind === 'builtin'));
  assert.deepEqual(r.map((x) => x.name), [
    'init', 'compact', 'goal', 'plan', 'turn', 'btw', 'side', 'lsp', 'rc', 'remote-control',
  ]);
});

run('slashCommandKindPresentation 只返回 glyph 与 label,颜色由 UI 统一处理', () => {
  const builtin = slashCommandKindPresentation({ kind: 'builtin' });
  const command = slashCommandKindPresentation({ kind: 'command' });
  const skill = slashCommandKindPresentation({ kind: 'skill' });
  const fallback = slashCommandKindPresentation({});
  assert.deepEqual(builtin, {
    icon: 'tool',
    label: '内置工具',
  });
  assert.deepEqual(command, {
    icon: 'command',
    label: 'Command',
  });
  assert.deepEqual(skill, {
    icon: 'lightbulb',
    label: 'Skill',
  });
  assert.deepEqual(fallback, {
    icon: 'lightbulb',
    label: 'Skill',
  });
  assert.equal(Object.hasOwn(builtin, 'color'), false);
  assert.equal(Object.hasOwn(command, 'color'), false);
  assert.equal(Object.hasOwn(skill, 'className'), false);
});

run('flattenCommands 把 opencode commands 放在 builtin 和 skill 之间', () => {
  const items = flattenCommands({
    builtins: [{ name: 'init', description: 'Generate AGENT.md' }],
    commands: [{ name: 'opsx-apply', description: 'Apply OpenSpec change' }],
    skills: [{ name: 'openspec-apply-change', description: 'Apply change skill' }],
  });
  assert.deepEqual(items.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'command:opsx-apply',
    'skill:openspec-apply-change',
  ]);
});

run('commandsWithFallback:空响应回退到基础命令', () => {
  const r1 = commandsWithFallback(null);
  const r2 = commandsWithFallback({ builtins: [], skills: [] });
  const expected = [
    'init', 'compact', 'goal', 'plan', 'turn', 'btw', 'side', 'lsp', 'rc', 'remote-control',
  ];
  assert.deepEqual(r1.map((x) => x.name), expected);
  assert.deepEqual(r2.map((x) => x.name), expected);
});

run('commandsWithFallback:后端返回 skills 时保留 skill + builtin 组合', () => {
  const r = commandsWithFallback({
    builtins: [
      { name: 'init', description: 'Generate AGENT.md' },
      { name: 'compact', description: 'Compress conversation history' },
      { name: 'goal', description: 'Manage thread goal' },
      { name: 'plan', description: 'Enter plan mode' },
    ],
    skills: [{ name: 'calculator', description: 'Exact math' }],
  });
  assert.deepEqual(r.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'builtin:compact',
    'builtin:goal',
    'builtin:plan',
    'builtin:turn',
    'builtin:btw',
    'builtin:side',
    'builtin:lsp',
    'builtin:rc',
    'builtin:remote-control',
    'skill:calculator',
  ]);
});

run('commandsWithFallback:保留 command kind 并放在基础 builtin 后', () => {
  const r = commandsWithFallback({
    commands: [{ name: 'opsx-apply', description: 'Apply OpenSpec change' }],
    skills: [{ name: 'calculator', description: 'Exact math' }],
  });
  assert.deepEqual(r.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'builtin:compact',
    'builtin:goal',
    'builtin:plan',
    'builtin:turn',
    'builtin:btw',
    'builtin:side',
    'builtin:lsp',
    'builtin:rc',
    'builtin:remote-control',
    'command:opsx-apply',
    'skill:calculator',
  ]);
});

run('commandsWithFallback:skills-only 响应也补上基础命令', () => {
  const r = commandsWithFallback({
    skills: [{ name: 'calculator', description: 'Exact math' }],
  });
  assert.deepEqual(r.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'builtin:compact',
    'builtin:goal',
    'builtin:plan',
    'builtin:turn',
    'builtin:btw',
    'builtin:side',
    'builtin:lsp',
    'builtin:rc',
    'builtin:remote-control',
    'skill:calculator',
  ]);
});

run('commandsWithFallback:partial builtin 响应补齐缺失基础命令', () => {
  const r = commandsWithFallback({
    builtins: [{ name: 'init', description: 'custom init' }],
    skills: [{ name: 'calculator', description: 'Exact math' }],
  });
  assert.deepEqual(r.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'builtin:compact',
    'builtin:goal',
    'builtin:plan',
    'builtin:turn',
    'builtin:btw',
    'builtin:side',
    'builtin:lsp',
    'builtin:rc',
    'builtin:remote-control',
    'skill:calculator',
  ]);
  assert.equal(r[0].description, 'custom init');
});

run('commandsWithFallback:额外 builtin 保留在基础命令之后', () => {
  const r = commandsWithFallback({
    builtins: [
      { name: 'init', description: 'custom init' },
      { name: 'custom', description: 'Custom command' },
    ],
  });
  assert.deepEqual(r.map((x) => `${x.kind}:${x.name}`), [
    'builtin:init',
    'builtin:compact',
    'builtin:goal',
    'builtin:plan',
    'builtin:turn',
    'builtin:btw',
    'builtin:side',
    'builtin:lsp',
    'builtin:rc',
    'builtin:remote-control',
    'builtin:custom',
  ]);
});

run('空查询返回原 flatten 顺序(builtin 在前)', () => {
  const r = rankCommands('', ITEMS);
  assert.equal(r.length, 7);
  assert.equal(r[0].name, 'init');
  assert.equal(r[1].name, 'compact');
  assert.equal(r[2].name, 'goal');
  assert.equal(r[3].name, 'plan');
  assert.equal(r[4].name, 'code-review');
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

run('deleteLeadingCommandBlock:Backspace 在命令块内会整块删除', () => {
  const text = '/init hello';
  const leading = parseLeadingCommand(text, ['init']);
  assert.deepEqual(deleteLeadingCommandBlock(text, leading, 3, 3, 'backward'), {
    value: 'hello',
    selectionStart: 0,
    selectionEnd: 0,
  });
});

run('deleteLeadingCommandBlock:Backspace 在命令后的空格处也整块删除', () => {
  const text = '/init hello';
  const leading = parseLeadingCommand(text, ['init']);
  assert.equal(deleteLeadingCommandBlock(text, leading, 6, 6, 'backward')?.value, 'hello');
});

run('deleteLeadingCommandBlock:Delete 在命令块前会整块删除', () => {
  const text = '/compact now';
  const leading = parseLeadingCommand(text, ['compact']);
  assert.equal(deleteLeadingCommandBlock(text, leading, 0, 0, 'forward')?.value, 'now');
});

run('deleteLeadingCommandBlock:选区碰到命令块时删除整个命令和选区', () => {
  const text = '/init hello world';
  const leading = parseLeadingCommand(text, ['init']);
  assert.equal(deleteLeadingCommandBlock(text, leading, 2, 11, 'backward')?.value, 'world');
});

run('deleteLeadingCommandBlock:不碰到命令块时返回 null', () => {
  const text = '/init hello';
  const leading = parseLeadingCommand(text, ['init']);
  assert.equal(deleteLeadingCommandBlock(text, leading, 7, 7, 'backward'), null);
  assert.equal(deleteLeadingCommandBlock('plain', { name: null, headLength: 0 }, 1, 1, 'backward'), null);
});

run('normalizeLeadingCommandSelection:光标落在命令块内部时归到块后', () => {
  const text = '/init hello';
  const leading = parseLeadingCommand(text, ['init']);
  assert.deepEqual(normalizeLeadingCommandSelection(text, leading, 3, 3), {
    selectionStart: 6,
    selectionEnd: 6,
  });
  assert.equal(normalizeLeadingCommandSelection(text, leading, 0, 0), null);
  assert.equal(normalizeLeadingCommandSelection(text, leading, 6, 6), null);
  assert.equal(normalizeLeadingCommandSelection(text, leading, 7, 7), null);
});

run('normalizeLeadingCommandSelection:选区碰到命令块内部时扩成整块', () => {
  const text = '/init hello world';
  const leading = parseLeadingCommand(text, ['init']);
  assert.deepEqual(normalizeLeadingCommandSelection(text, leading, 0, 3), {
    selectionStart: 0,
    selectionEnd: 6,
  });
  assert.deepEqual(normalizeLeadingCommandSelection(text, leading, 3, 11), {
    selectionStart: 0,
    selectionEnd: 11,
  });
  assert.equal(normalizeLeadingCommandSelection(text, leading, 6, 11), null);
});

run('moveAcrossLeadingCommandBlock:左右方向键一次跨过命令块', () => {
  const text = '/init hello';
  const leading = parseLeadingCommand(text, ['init']);
  assert.deepEqual(moveAcrossLeadingCommandBlock(text, leading, 6, 6, 'backward'), {
    selectionStart: 0,
    selectionEnd: 0,
  });
  assert.deepEqual(moveAcrossLeadingCommandBlock(text, leading, 0, 0, 'forward'), {
    selectionStart: 6,
    selectionEnd: 6,
  });
  assert.equal(moveAcrossLeadingCommandBlock(text, leading, 7, 7, 'backward'), null);
  assert.equal(moveAcrossLeadingCommandBlock(text, leading, 0, 6, 'forward'), null);
});

// resolveLeadingSlashCommand:transcript 用户气泡把首段命令渲染成徽标的解析层。
// 触发场景:用户发送 "/<skill> 参数",UI 要把 "/skill" 切出来高亮 + 取描述做 hover。
run('resolveLeadingSlashCommand:命中 skill,切出 token/rest 并带回 kind 与描述', () => {
  const r = resolveLeadingSlashCommand('/code-review 看看第三个改动', ITEMS);
  assert.equal(r.token, '/code-review');
  assert.equal(r.name, 'code-review');
  assert.equal(r.kind, 'skill');
  assert.equal(r.description, 'Review code changes');
  // rest 保留分隔空白,交给 whitespace-pre-wrap 还原视觉间距
  assert.equal(r.rest, ' 看看第三个改动');
});

run('resolveLeadingSlashCommand:命中 builtin 也返回 builtin kind', () => {
  const r = resolveLeadingSlashCommand('/init', ITEMS);
  assert.equal(r.name, 'init');
  assert.equal(r.kind, 'builtin');
  assert.equal(r.token, '/init');
  assert.equal(r.rest, '');
});

run('resolveLeadingSlashCommand:命中 opencode command 返回 command kind', () => {
  const items = flattenCommands({
    commands: [{ name: 'opsx-apply', description: 'Apply OpenSpec change' }],
  });
  const r = resolveLeadingSlashCommand('/opsx-apply change-123', items);
  assert.equal(r.name, 'opsx-apply');
  assert.equal(r.kind, 'command');
  assert.equal(r.token, '/opsx-apply');
  assert.equal(slashCommandKindPresentation(r).icon, 'command');
});

run('resolveLeadingSlashCommand:输入框 chip 可从首段命令取回对应 glyph', () => {
  const selectedBuiltin = resolveLeadingSlashCommand('/init ', ITEMS);
  const typedSkill = resolveLeadingSlashCommand('/code-review 看看第三个改动', ITEMS);
  assert.equal(slashCommandKindPresentation(selectedBuiltin).icon, 'tool');
  assert.equal(selectedBuiltin.token, '/init');
  assert.equal(selectedBuiltin.rest, ' ');
  assert.equal(slashCommandKindPresentation(typedSkill).icon, 'lightbulb');
  assert.equal(typedSkill.token, '/code-review');
  assert.equal(typedSkill.rest, ' 看看第三个改动');
});

// 期望行为:未命中 skill(命令名未知)必须返回 null,UI 据此回退纯文本,
// 不能把 /foobar 误高亮成命令。这是本次美化的核心边界。
run('resolveLeadingSlashCommand:未知命令名返回 null(未命中 skill)', () => {
  assert.equal(resolveLeadingSlashCommand('/foobar test', ITEMS), null);
});

run('resolveLeadingSlashCommand:不以 / 开头 / 空串 / 空清单均返回 null', () => {
  assert.equal(resolveLeadingSlashCommand('看看第三个球衣', ITEMS), null);
  assert.equal(resolveLeadingSlashCommand('', ITEMS), null);
  // 命令清单还没加载完(空数组)时,任何命令都视为未命中,回退纯文本
  assert.equal(resolveLeadingSlashCommand('/init', []), null);
});

run('resolveLeadingSlashCommand:命中但该命令无描述时 description 为空串', () => {
  const items = flattenCommands({ skills: [{ name: 'nodesc' }] });
  const r = resolveLeadingSlashCommand('/nodesc 干活', items);
  assert.equal(r.name, 'nodesc');
  assert.equal(r.description, '');
});

run('parseExecutableBuiltinCommand:识别 init、compact、goal 和 plan', () => {
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
  assert.deepEqual(parseExecutableBuiltinCommand('/goal --tokens 50K finish'), {
    name: 'goal',
    args: '--tokens 50K finish',
    display_text: '/goal --tokens 50K finish',
  });
  assert.deepEqual(parseExecutableBuiltinCommand('/plan inspect first'), {
    name: 'plan',
    args: 'inspect first',
    display_text: '/plan inspect first',
  });
  assert.equal(parseExecutableBuiltinCommand('/code-review check this'), null);
  assert.equal(parseExecutableBuiltinCommand('/unknown'), null);
  assert.equal(parseExecutableBuiltinCommand('plain text'), null);
});

// 回归:B-Task 8 复审发现 /rc、/remote-control 只进了 parseExecutableBuiltinCommand
// 的可执行白名单,FALLBACK_BUILTINS 漏加。bug 表现:输入框打 "/r" 时下拉
// 自动补全里不出现 /rc(rankCommands 的数据源是 fallback/后端 builtin 清单),
// 完整敲 /rc 功能正常但用户无从发现。期望:两条命令都能被前缀匹配排到最前。
run('rankCommands:rc 与 remote-control 出现在 fallback 下拉补全里', () => {
  const r = rankCommands('r', fallbackCommands());
  // 前缀匹配(+1000)排最前,同分按字典序:rc < remote-control
  assert.equal(r[0].name, 'rc');
  assert.equal(r[1].name, 'remote-control');
  const rc = rankCommands('rc', fallbackCommands());
  assert.equal(rc[0].name, 'rc');
});

// 回归:同一漏项的第二个表现 —— 会话转写里已发送的 "/rc off" 不渲染成命令
// chip(resolveLeadingSlashCommand 在清单里找不到 rc → 返回 null → 回退纯文本)。
// 期望:fallback 清单即可命中,kind 为 builtin。
run('resolveLeadingSlashCommand:rc 与 remote-control 渲染成 builtin chip', () => {
  const list = commandsWithFallback(null);
  const rc = resolveLeadingSlashCommand('/rc off', list);
  assert.equal(rc.name, 'rc');
  assert.equal(rc.kind, 'builtin');
  assert.equal(rc.token, '/rc');
  assert.equal(rc.rest, ' off');
  const full = resolveLeadingSlashCommand('/remote-control show', list);
  assert.equal(full.name, 'remote-control');
  assert.equal(full.kind, 'builtin');
  assert.equal(full.token, '/remote-control');
});

run('parseExecutableBuiltinCommand:识别 rc 与 remote-control(builtin command HTTP 面白名单)', () => {
  assert.deepEqual(parseExecutableBuiltinCommand('/rc'), {
    name: 'rc',
    args: '',
    display_text: '/rc',
  });
  assert.deepEqual(parseExecutableBuiltinCommand('/rc off'), {
    name: 'rc',
    args: 'off',
    display_text: '/rc off',
  });
  assert.deepEqual(parseExecutableBuiltinCommand('/rc show'), {
    name: 'rc',
    args: 'show',
    display_text: '/rc show',
  });
  assert.deepEqual(parseExecutableBuiltinCommand('/remote-control show'), {
    name: 'remote-control',
    args: 'show',
    display_text: '/remote-control show',
  });
});
