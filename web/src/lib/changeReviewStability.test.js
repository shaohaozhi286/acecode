import assert from 'node:assert/strict';
import {
  reconcileOpenFiles,
  restoredScrollTop,
  stableBySignature,
} from './changeReviewStability.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// ── stableBySignature ───────────────────────────────────────
// 回归背景:流式期间每个 WS 帧 items 都换新引用,changeGroups 即使内容
// 没变也是新数组,DiffPreview 的 useMemo([group]) 每帧失效重跑 diff2html,
// tool_end 时整棵替换 diff DOM 导致滚动位置丢失 + 闪烁(WebView2 103 实测)。

run('stableBySignature 签名一致时复用旧 entry(保对象身份)', () => {
  // 触发场景:纯飙字,WS 帧让 changeGroups 重算出内容相同的新数组。
  // 期望:返回旧 entry 本身,下游拿到的 value 引用不变 → memo 不失效。
  const prev = { signature: 'sig-a', value: [{ file: 'a.js' }] };
  const next = { signature: 'sig-a', value: [{ file: 'a.js' }] };
  assert.equal(stableBySignature(prev, next), prev);
});

run('stableBySignature 签名变化时采用新 entry', () => {
  // 触发场景:file_edit 落地,hunks 真实变化 → 签名变化。
  // 期望:返回新 entry,UI 渲染新内容。
  const prev = { signature: 'sig-a', value: [{ file: 'a.js' }] };
  const next = { signature: 'sig-b', value: [{ file: 'a.js' }, { file: 'b.js' }] };
  assert.equal(stableBySignature(prev, next), next);
});

run('stableBySignature 无 prev(首帧)时采用新 entry', () => {
  const next = { signature: 'sig-a', value: [] };
  assert.equal(stableBySignature(null, next), next);
  assert.equal(stableBySignature(undefined, next), next);
});

run('stableBySignature 空签名也参与比较(空变更列表保持同一引用)', () => {
  // 触发场景:会话尚无任何文件变更(签名为空字符串),流式期间反复重算。
  // 期望:空数组引用同样被稳定,SidePanel/预览面板不做无谓重渲染。
  const prev = { signature: '', value: [] };
  const next = { signature: '', value: [] };
  assert.equal(stableBySignature(prev, next), prev);
});

// ── reconcileOpenFiles ──────────────────────────────────────
// 回归背景:ChangeReviewPanel 原实现每次 effect 都 setOpenFiles(new Set(...)),
// 集合内容没变也换新身份,流式期间渲染次数翻倍。

run('reconcileOpenFiles 集合内容不变时返回原 Set 身份', () => {
  // 触发场景:已有文件新增 hunks(文件集合不变)触发 effect 重跑。
  // 期望:返回 prev 本身,setState 走 Object.is 跳过重渲染。
  const prev = new Set(['a.js']);
  assert.equal(reconcileOpenFiles(prev, ['a.js', 'b.js']), prev);
});

run('reconcileOpenFiles 剔除已不存在的文件', () => {
  const prev = new Set(['a.js', 'gone.js']);
  const next = reconcileOpenFiles(prev, ['a.js', 'b.js']);
  assert.notEqual(next, prev);
  assert.deepEqual([...next], ['a.js']);
});

run('reconcileOpenFiles 调和后为空时回退展开第一个文件', () => {
  // 期望行为与原实现一致:永远至少展开一个文件,用户能直接看到 diff。
  const prev = new Set(['gone.js']);
  const next = reconcileOpenFiles(prev, ['a.js', 'b.js']);
  assert.deepEqual([...next], ['a.js']);
});

run('reconcileOpenFiles 文件列表为空时返回空集合且保身份', () => {
  const empty = new Set();
  assert.equal(reconcileOpenFiles(empty, []), empty);
  const prev = new Set(['a.js']);
  const cleared = reconcileOpenFiles(prev, []);
  assert.notEqual(cleared, prev);
  assert.equal(cleared.size, 0);
});

// ── restoredScrollTop ───────────────────────────────────────
// 回归背景:diff 内容瞬时塌缩(空帧)时浏览器把滚动容器 scrollTop 钳到 0,
// 内容长回来后用户停在顶部 ——「屏幕一闪,滚动条滚回顶部」的直接机制。

run('restoredScrollTop 钳制后恢复到用户位置', () => {
  // 触发场景:用户在 scrollTop=4000 阅读,内容替换后浏览器钳到 0,
  // 新内容高度足够。期望:恢复到 4000。
  assert.equal(restoredScrollTop({
    savedScrollTop: 4000,
    currentScrollTop: 0,
    scrollHeight: 10000,
    clientHeight: 600,
  }), 4000);
});

run('restoredScrollTop 新内容不够高时恢复到新的最大滚动量', () => {
  // 触发场景:内容合法变矮(maxScroll 只剩 3000)。
  // 期望:不超过新内容允许的最大值。
  assert.equal(restoredScrollTop({
    savedScrollTop: 4000,
    currentScrollTop: 0,
    scrollHeight: 3600,
    clientHeight: 600,
  }), 3000);
});

run('restoredScrollTop 位置一致时不干预', () => {
  // 触发场景:没有发生钳制(内容只增不减)。期望:null,一次 DOM 写都不做。
  assert.equal(restoredScrollTop({
    savedScrollTop: 4000,
    currentScrollTop: 4000,
    scrollHeight: 10000,
    clientHeight: 600,
  }), null);
});

run('restoredScrollTop 当前位置更靠下时不回拉(尊重用户主动滚动)', () => {
  // 触发场景:抑制窗口边缘漏记了一次用户向下滚动,current > saved。
  // 期望:绝不把用户往上拉,返回 null。
  assert.equal(restoredScrollTop({
    savedScrollTop: 1000,
    currentScrollTop: 2000,
    scrollHeight: 10000,
    clientHeight: 600,
  }), null);
});

run('restoredScrollTop 非法输入按 0 处理', () => {
  assert.equal(restoredScrollTop({
    savedScrollTop: NaN,
    currentScrollTop: 0,
    scrollHeight: 1000,
    clientHeight: 600,
  }), null);
  assert.equal(restoredScrollTop(), null);
});
