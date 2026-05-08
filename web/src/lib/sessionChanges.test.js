import assert from 'node:assert/strict';
import {
  aggregateHunksFromMessages,
  changeGroupsSignature,
  changeSetSignature,
  collectHunkMessagesFromItems,
  collectTurnChangeSetsFromItems,
  summarizeChangeGroups,
} from './sessionChanges.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('collectHunkMessagesFromItems 只抽取带 hunks 的 tool item', () => {
  const hunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const messages = collectHunkMessagesFromItems([
    { kind: 'msg', content: 'hello' },
    { kind: 'tool', tool: { hunks: [] } },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'a.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] },
        hunks: [hunk],
      },
    },
  ]);
  assert.deepEqual(messages, [{
    file: 'a.js',
    additions: 1,
    deletions: 0,
    hunks: [{ ...hunk, file: 'a.js' }],
  }]);
});

run('历史 tool item 中的 file_write / file_edit hunks 会进入聚合，file_read 被忽略', () => {
  const writeHunk = { old_start: 1, old_count: 0, new_start: 1, new_count: 1, lines: [] };
  const editHunk = { old_start: 4, old_count: 1, new_start: 4, new_count: 2, lines: [] };
  const messages = collectHunkMessagesFromItems([
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/new.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] },
        hunks: [writeHunk],
      },
    },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/read.js', metrics: [{ label: '+', value: '0' }, { label: '-', value: '0' }] },
        hunks: [],
      },
    },
    {
      kind: 'tool',
      tool: {
        summary: { object: 'src/edit.js', metrics: [{ label: '+', value: '2' }, { label: '-', value: '1' }] },
        hunks: [editHunk],
      },
    },
  ]);
  const groups = aggregateHunksFromMessages(messages);
  assert.deepEqual(groups.map((g) => g.file), ['src/new.js', 'src/edit.js']);
  assert.deepEqual(summarizeChangeGroups(groups), {
    fileCount: 2,
    totalAdditions: 3,
    totalDeletions: 1,
    hasChanges: true,
  });
});

run('collectTurnChangeSetsFromItems 将工具变更归到前一个用户消息', () => {
  const hunkA = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const hunkB = { old_start: 2, old_count: 0, new_start: 2, new_count: 1, lines: [] };
  const sets = collectTurnChangeSetsFromItems([
    { kind: 'msg', id: 1, messageId: 'u1', role: 'user', content: 'first turn' },
    {
      kind: 'tool',
      id: 2,
      tool: { summary: { object: 'a.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '1' }] }, hunks: [hunkA] },
    },
    { kind: 'msg', id: 3, messageId: 'a1', role: 'assistant', content: 'done' },
    { kind: 'msg', id: 4, messageId: 'u2', role: 'user', content: 'second turn' },
    {
      kind: 'tool',
      id: 5,
      tool: { summary: { object: 'b.js', metrics: [{ label: '+', value: '1' }, { label: '-', value: '0' }] }, hunks: [hunkB] },
    },
  ]);
  assert.equal(sets.length, 2);
  assert.equal(sets[0].userMessageId, 'u1');
  assert.equal(sets[0].afterItemId, 2);
  assert.equal(sets[0].groups[0].file, 'a.js');
  assert.equal(sets[1].userMessageId, 'u2');
  assert.equal(sets[1].afterItemId, 5);
  assert.equal(sets[1].groups[0].file, 'b.js');
});

run('change signatures 对同一组变更保持稳定', () => {
  const groups = aggregateHunksFromMessages([
    { file: 'a.js', additions: 2, deletions: 1, hunks: [{ old_start: 1, old_count: 1, new_start: 1, new_count: 2, lines: [] }] },
  ]);
  const first = changeGroupsSignature(groups);
  const second = changeGroupsSignature(groups.map((group) => ({ ...group, hunks: [...group.hunks] })));
  assert.equal(first, second);
  assert.equal(changeSetSignature({ userMessageId: 'u1', groups }), `u1::${first}`);
});

run('summarizeChangeGroups 汇总文件数和加删行', () => {
  const groups = aggregateHunksFromMessages([
    { hunks: [
      { file: 'a.js', additions: 2, deletions: 1 },
      { file: 'b.js', additions: 5, deletions: 0 },
      { file: 'a.js', additions: 3, deletions: 4 },
    ] },
  ]);
  assert.deepEqual(summarizeChangeGroups(groups), {
    fileCount: 2,
    totalAdditions: 10,
    totalDeletions: 5,
    hasChanges: true,
  });
});

run('aggregateHunksFromMessages 使用 message 级文件名和统计回填 hunk', () => {
  const hunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 1, lines: [] };
  const groups = aggregateHunksFromMessages([
    { file: 'a.js', additions: 3, deletions: 2, hunks: [hunk] },
  ]);
  assert.equal(groups.length, 1);
  assert.equal(groups[0].file, 'a.js');
  assert.equal(groups[0].totalAdditions, 3);
  assert.equal(groups[0].totalDeletions, 2);
  assert.deepEqual(groups[0].hunks, [{ ...hunk, file: 'a.js' }]);
});

run('summarizeChangeGroups 空列表返回无变更', () => {
  assert.deepEqual(summarizeChangeGroups([]), {
    fileCount: 0,
    totalAdditions: 0,
    totalDeletions: 0,
    hasChanges: false,
  });
});
