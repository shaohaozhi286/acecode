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
    description: 'Analyze this codebase and generate (or improve) ACECODE.md',
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
]);

export function fallbackCommands() {
  return FALLBACK_BUILTINS.map((c) => ({ ...c }));
}

// 把后端返回的 {builtins, skills} 拍平成统一项数组,加 kind 字段以备扩展。
// 顺序:builtins 先(后端固定 init→compact),skills 后(后端按字典序)。
export function flattenCommands(payload) {
  const out = [];
  if (payload && Array.isArray(payload.builtins)) {
    for (const c of payload.builtins) {
      if (c && c.name) out.push({ kind: 'builtin', name: c.name, description: c.description || '' });
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

export function parseExecutableBuiltinCommand(value) {
  const text = typeof value === 'string' ? value.trim() : '';
  const leading = parseLeadingCommand(text, ['init', 'compact', 'goal']);
  if (!leading.name) return null;
  return {
    name: leading.name,
    args: text.slice(leading.headLength).trim(),
    display_text: text,
  };
}
