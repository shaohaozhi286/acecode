// SlashDropdown 的纯逻辑层:命令排序 + 输入框首段命令解析。
//
// 排序规则(design.md D4):
//   1. name 前缀匹配优先(case-insensitive)
//   2. name 子串匹配次之
//   3. description 子串匹配最后
//   4. 同档内按 name 字典序
// 完全不命中 → 过滤掉。空查询返回原顺序(builtin 在前 + skill 字典序),不打分。

function lower(s) {
  return typeof s === 'string' ? s.toLowerCase() : '';
}

const FALLBACK_BUILTINS = Object.freeze([
  Object.freeze({
    kind: 'builtin',
    name: 'init',
    description: 'Analyze this codebase and generate (or improve) AGENT.md',
  }),
  Object.freeze({
    kind: 'builtin',
    name: 'compact',
    description: 'Compress conversation history',
  }),
  Object.freeze({
    kind: 'builtin',
    name: 'goal',
    description: 'Create, view, pause, resume, edit, or clear the thread goal',
  }),
  Object.freeze({
    kind: 'builtin',
    name: 'plan',
    description: 'Enter plan mode or start planning a described task',
  }),
]);

export function fallbackCommands() {
  return FALLBACK_BUILTINS.map((c) => ({ ...c }));
}

// 把后端返回的 {builtins, commands, skills} 拍平成统一项数组,加 kind 字段以备扩展。
// 顺序:builtins 先(后端固定 init→compact→goal→plan),opencode command 后,skills 后。
export function flattenCommands(payload) {
  const out = [];
  if (payload && Array.isArray(payload.builtins)) {
    for (const c of payload.builtins) {
      if (c && c.name) out.push({ kind: 'builtin', name: c.name, description: c.description || '' });
    }
  }
  if (payload && Array.isArray(payload.commands)) {
    for (const c of payload.commands) {
      if (c && c.name) out.push({ kind: 'command', name: c.name, description: c.description || '' });
    }
  }
  if (payload && Array.isArray(payload.skills)) {
    for (const c of payload.skills) {
      if (c && c.name) out.push({ kind: 'skill', name: c.name, description: c.description || '' });
    }
  }
  return out;
}

export function commandsWithFallback(payload) {
  const commands = flattenCommands(payload);
  const fallbackBuiltins = fallbackCommands();
  const fallbackNames = new Set(fallbackBuiltins.map((c) => c.name));
  const payloadBuiltins = new Map(
    commands
      .filter((c) => c.kind === 'builtin' && fallbackNames.has(c.name))
      .map((c) => [c.name, c]),
  );
  const mergedBuiltins = fallbackBuiltins.map((c) => payloadBuiltins.get(c.name) || c);
  const rest = commands.filter((c) => c.kind !== 'builtin' || !fallbackNames.has(c.name));
  return [...mergedBuiltins, ...rest];
}

// 给单条命令打分。query 已 lowercase。
function scoreCommand(item, query) {
  const name = lower(item.name);
  const desc = lower(item.description);
  if (!query) return 1; // 空查询人人有份,排序回退到原顺序
  if (name.startsWith(query)) return 1000;
  if (name.includes(query)) return 500;
  if (desc.includes(query)) return 100;
  return 0;
}

// 排序:分数降序,同分按 name 字典序。返回新数组,不修改输入。
export function rankCommands(query, items) {
  const q = lower(query || '').trim();
  const scored = [];
  for (const it of items || []) {
    const s = scoreCommand(it, q);
    if (s > 0) scored.push({ it, s });
  }
  if (!q) {
    // 空查询保留 flattenCommands 的原始顺序(builtin 在前)
    return scored.map((x) => x.it);
  }
  scored.sort((a, b) => {
    if (b.s !== a.s) return b.s - a.s;
    return a.it.name.localeCompare(b.it.name);
  });
  return scored.map((x) => x.it);
}

export function slashCommandKindPresentation(item) {
  if (item && item.kind === 'builtin') {
    return { icon: 'tool', label: '内置工具' };
  }
  if (item && item.kind === 'command') {
    return { icon: 'command', label: 'Command' };
  }
  return { icon: 'lightbulb', label: 'Skill' };
}

// 解析输入框值的首段命令名:从首字符 `/` 到第一个空白(或字符串末尾)之间的串,
// 去掉前导 `/`。返回 {name, headLength}:
//   - name: 已知命令名(在 knownNames 中)→ 该名;否则 null
//   - headLength: 首段(含 `/`)在原串中的字符长度,无论是否命中
// 空白判定:空格 / \t / \n / \r。
export function parseLeadingCommand(value, knownNames = []) {
  if (typeof value !== 'string' || value.length === 0 || value[0] !== '/') {
    return { name: null, headLength: 0 };
  }
  let end = 1;
  while (end < value.length && !/\s/.test(value[end])) end++;
  const head = value.slice(1, end);
  const known = new Set(knownNames || []);
  return {
    name: known.has(head) ? head : null,
    headLength: end,
  };
}

export function leadingCommandBlockEnd(value, leading) {
  if (typeof value !== 'string' || !leading?.name || !Number.isFinite(leading.headLength)) {
    return 0;
  }
  let end = Math.max(0, Math.min(value.length, leading.headLength));
  if (end < value.length && /\s/.test(value[end])) end += 1;
  return end;
}

export function deleteLeadingCommandBlock(value, leading, selectionStart, selectionEnd, direction = 'backward') {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));
  const hasSelection = start !== end;

  const touchesBlock = hasSelection
    ? start < blockEnd && end > 0
    : direction === 'forward'
      ? start < blockEnd
      : start > 0 && start <= blockEnd;
  if (!touchesBlock) return null;

  let deleteEnd = hasSelection ? Math.max(blockEnd, end) : blockEnd;
  if (deleteEnd < value.length && /\s/.test(value[deleteEnd])) deleteEnd += 1;
  return {
    value: value.slice(deleteEnd),
    selectionStart: 0,
    selectionEnd: 0,
  };
}

export function normalizeLeadingCommandSelection(value, leading, selectionStart, selectionEnd) {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));

  if (start === end) {
    if (start > 0 && start < blockEnd) {
      return { selectionStart: blockEnd, selectionEnd: blockEnd };
    }
    return null;
  }

  let nextStart = start;
  let nextEnd = end;
  if (nextStart > 0 && nextStart < blockEnd) nextStart = 0;
  if (nextEnd > 0 && nextEnd < blockEnd) nextEnd = blockEnd;

  if (nextStart === start && nextEnd === end) return null;
  return { selectionStart: nextStart, selectionEnd: nextEnd };
}

export function moveAcrossLeadingCommandBlock(value, leading, selectionStart, selectionEnd, direction) {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));
  if (start !== end) return null;

  if (direction === 'backward' && start > 0 && start <= blockEnd) {
    return { selectionStart: 0, selectionEnd: 0 };
  }
  if (direction === 'forward' && start >= 0 && start < blockEnd) {
    return { selectionStart: blockEnd, selectionEnd: blockEnd };
  }
  return null;
}

// 解析一条已落库消息的首段斜杠命令,供 transcript 渲染成徽标(chip)使用。
// 与 parseLeadingCommand 的差别:这里额外把命中的命令在 commands 清单里找回
// kind / description,并把首段(含 `/`)与剩余正文切开,方便 UI 分段渲染。
//
// 返回:
//   - 命中已知命令 → { token, name, kind, description, rest }
//       token   = 首段含前导 `/`(如 "/taobao-compare")
//       rest    = 首段之后的全部原文(含分隔空白,交给 whitespace-pre-wrap 保留)
//   - 不以 `/` 开头 / 首段不是已知命令(未命中 skill)→ null
//     调用方据此回退到纯文本渲染,避免把 `/foobar` 误当命令高亮。
export function resolveLeadingSlashCommand(text, commands = []) {
  if (typeof text !== 'string' || text.length === 0 || text[0] !== '/') return null;
  const list = Array.isArray(commands) ? commands : [];
  const knownNames = list.map((c) => c && c.name).filter(Boolean);
  const leading = parseLeadingCommand(text, knownNames);
  if (!leading.name) return null;
  const item = list.find((c) => c && c.name === leading.name) || null;
  return {
    token: text.slice(0, leading.headLength),
    name: leading.name,
    kind: item && item.kind ? item.kind : 'skill',
    description: item && item.description ? item.description : '',
    rest: text.slice(leading.headLength),
  };
}

export function parseExecutableBuiltinCommand(value) {
  const text = typeof value === 'string' ? value.trim() : '';
  const leading = parseLeadingCommand(text, ['init', 'compact', 'goal', 'plan']);
  if (!leading.name) return null;
  return {
    name: leading.name,
    args: text.slice(leading.headLength).trim(),
    display_text: text,
  };
}
