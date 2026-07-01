import assert from 'node:assert/strict';
import {
  completionSummaryMarkdown,
  normalizeTaskCompleteMarkdown,
  stripCompletionSummaryLabel,
} from './taskCompleteSummary.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('task_complete summary strips duplicated summary label', () => {
  assert.equal(stripCompletionSummaryLabel('总结：修复完成'), '修复完成');
  assert.equal(normalizeTaskCompleteMarkdown('Summary: fixed tests'), 'fixed tests');
});

run('task_complete summary preserves existing markdown blocks', () => {
  const markdown = '**完成**\n\n- 修复 A\n- 修复 B';
  assert.equal(normalizeTaskCompleteMarkdown(markdown), markdown);
});

run('task_complete compact numbered text expands to markdown list', () => {
  const text = '总结：修复了编译错误。修改内容：1) 删除过期连接；2) 改为父控件；3) 编译通过';
  assert.equal(
    normalizeTaskCompleteMarkdown(text),
    '修复了编译错误。修改内容：\n\n1. 删除过期连接\n2. 改为父控件\n3. 编译通过',
  );
});

run('completion summary prefers raw summary over compact title', () => {
  assert.equal(
    completionSummaryMarkdown({
      title: '总结：**完成** - README 已更新',
      summary: '**完成**\n\n- README 已更新',
    }),
    '**完成**\n\n- README 已更新',
  );
});
