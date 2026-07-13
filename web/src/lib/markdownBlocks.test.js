import assert from 'node:assert/strict';
import { renderMarkdown, renderMarkdownBlocks } from './markdown.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 核心不变量:按块渲染的 HTML 拼接必须与全文渲染逐字节一致。全文只
// parse 一次再按 top-level token 组分别 render,所以两者理应等价 ——
// 一旦分组逻辑漏 token 或切错边界,这条会最先失败。
run('block html concatenation equals whole-document render', () => {
  const src = [
    '# 标题',
    '',
    '第一段,有 **加粗** 和 `inline code`。',
    '',
    '```js',
    'const x = 1;',
    '```',
    '',
    '- 列表项 1',
    '- 列表项 2',
    '  - 嵌套项',
    '',
    '| a | b |',
    '| - | - |',
    '| 1 | 2 |',
    '',
    '> 引用块',
    '',
    '---',
    '',
    '结尾段落 <script>alert(1)</script>',
  ].join('\n');
  const blocks = renderMarkdownBlocks(src);
  assert.ok(blocks.length >= 7, `期望切出多个块,实际 ${blocks.length}`);
  assert.equal(blocks.map((b) => b.html).join(''), renderMarkdown(src));
});

// 回归测试(fix 流式出字时消息区上下跳动):流式 append 时,已定稿的
// 前缀块 HTML 字符串必须保持不变 —— React 靠这个字符串比较跳过 DOM
// 更新,前缀 DOM 不动才不会破坏滚动稳定性。
run('streaming append keeps finished prefix block html unchanged', () => {
  const prefix = '第一段。\n\n```python\nprint("hi")\n```\n\n第二段完整。\n\n';
  const before = renderMarkdownBlocks(`${prefix}正在输出的第三`);
  const after = renderMarkdownBlocks(`${prefix}正在输出的第三段继续变长,还带 **格式**。`);
  assert.ok(before.length >= 4);
  assert.equal(after.length, before.length);
  for (let i = 0; i < before.length - 1; i += 1) {
    assert.equal(after[i].html, before[i].html, `前缀块 ${i} 的 HTML 不应随流式追加变化`);
    assert.equal(after[i].key, before[i].key, `前缀块 ${i} 的 key 不应随流式追加变化`);
  }
  assert.notEqual(after[after.length - 1].html, before[before.length - 1].html);
});

// 场景:reference link 的使用和定义分在不同块([text][ref] 在第一段,
// [ref]: url 在文末)。切块渲染共享同一个 parse env,引用必须仍能解析
// 成 <a>,不能退化成字面方括号文本。
run('reference links resolve across block boundaries', () => {
  const src = '看这个 [链接][ref] 的效果。\n\n另一段。\n\n[ref]: https://example.com';
  const joined = renderMarkdownBlocks(src).map((b) => b.html).join('');
  assert.ok(joined.includes('href="https://example.com"'), joined);
});

// 边界:空输入 / 非字符串输入不产生块;纯文本单段落切出恰好一块。
run('empty and trivial inputs', () => {
  assert.deepEqual(renderMarkdownBlocks(''), []);
  assert.deepEqual(renderMarkdownBlocks(null), []);
  const single = renderMarkdownBlocks('只有一段');
  assert.equal(single.length, 1);
  assert.equal(single[0].html, renderMarkdown('只有一段'));
});

// 场景:流式中途的未闭合代码围栏(``` 开了没关)。markdown-it 会把其后
// 内容全部并入 fence 直到文末 —— 未闭合 fence 应落在最后一块里正常渲染,
// 且拼接仍与全文渲染一致(不崩、不丢内容)。
run('unclosed streaming code fence stays consistent with whole render', () => {
  const src = '前一段。\n\n```cpp\nint main() {\n  return 0;';
  const blocks = renderMarkdownBlocks(src);
  assert.equal(blocks.map((b) => b.html).join(''), renderMarkdown(src));
  // fence 内容会被 highlight.js 包 span,断言块结构而不是字面代码文本
  const tail = blocks[blocks.length - 1].html;
  assert.ok(tail.includes('language-cpp'), tail);
  assert.ok(tail.includes('main'), tail);
});
