// 变更审查视图(ChangeReviewPanel / DiffPreview)流式期间的渲染稳定性纯逻辑。
//
// 背景(fix-preview-scroll-during-stream):agent 飙字期间每个 WS 帧都让
// items → changeGroups 全链路换新引用(内容多数时候没变)。引用抖动会让
// DiffPreview 的 useMemo 每帧失效重跑 diff2html,tool_end 落地时整棵替换
// diff DOM —— 用户正用滚动条阅读的位置被重置回顶部,老引擎(WebView2 103)
// 上还伴随明显闪烁。这里的三个纯函数分别解决:
//   - stableBySignature: 内容签名一致时复用旧引用,从源头掐断引用抖动;
//   - reconcileOpenFiles: 展开集合内容不变时保留旧 Set 身份,避免冗余渲染;
//   - restoredScrollTop: diff 内容替换导致浏览器钳制 scrollTop 后算出恢复值。

function finiteNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : 0;
}

/**
 * 按内容签名稳定引用:prev/next 形如 { signature, value }。
 * 签名一致 → 返回 prev(value 保持旧对象身份,下游 memo 不失效);
 * 签名变化或没有 prev → 返回 next。
 *
 * @template T
 * @param {{signature: string, value: T} | null | undefined} prev
 * @param {{signature: string, value: T}} next
 * @returns {{signature: string, value: T}}
 */
export function stableBySignature(prev, next) {
  if (prev && prev.value !== undefined && prev.signature === next.signature) return prev;
  return next;
}

/**
 * 依据当前变更文件名列表调和「已展开文件」集合:
 * - 剔除已不存在的文件;
 * - 集合变空(含初始为空)且仍有文件时回退展开第一个文件(保持原 UI 行为);
 * - 调和结果与 prev 内容一致时返回 prev 本身(保身份,React 跳过重渲染)。
 *
 * @param {Set<string>} prev 当前展开集合
 * @param {string[]} files 最新的变更文件名列表(保持出现顺序)
 * @returns {Set<string>} 内容未变时为 prev 本身,否则为新 Set
 */
export function reconcileOpenFiles(prev, files) {
  const prevSet = prev instanceof Set ? prev : new Set();
  const list = Array.isArray(files) ? files.filter(Boolean) : [];
  if (list.length === 0) {
    return prevSet.size === 0 ? prevSet : new Set();
  }
  const valid = new Set(list);
  const next = new Set([...prevSet].filter((file) => valid.has(file)));
  if (next.size === 0) next.add(list[0]);
  if (next.size === prevSet.size && [...next].every((file) => prevSet.has(file))) {
    return prevSet;
  }
  return next;
}

/**
 * diff 内容替换后判断滚动位置是否需要恢复。
 *
 * 浏览器在滚动容器内容变矮时会把 scrollTop 钳到新的最大值(内容塌缩到不
 * 溢出时即钳到 0);内容随后长回来 scrollTop 也不会自动还原。调用方在
 * useLayoutEffect(此时钳制产生的 scroll 事件尚未派发,savedScrollTop 仍是
 * 用户最后的真实位置)里用本函数算恢复目标。
 *
 * 只在「当前位置比目标更靠上」时给出恢复值 —— 用户主动滚动产生的位置
 * 经 onScroll 已同步进 savedScrollTop,此时 target === current 返回 null,
 * 不会与用户意图竞争。
 *
 * @returns {number | null} 需要写回的 scrollTop;null 表示无需干预
 */
export function restoredScrollTop({
  savedScrollTop = 0,
  currentScrollTop = 0,
  scrollHeight = 0,
  clientHeight = 0,
} = {}) {
  const maxScroll = Math.max(0, finiteNumber(scrollHeight) - finiteNumber(clientHeight));
  const target = Math.min(Math.max(0, finiteNumber(savedScrollTop)), maxScroll);
  const current = Math.max(0, finiteNumber(currentScrollTop));
  return target > current ? target : null;
}
