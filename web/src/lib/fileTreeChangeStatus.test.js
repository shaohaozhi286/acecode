import assert from 'node:assert/strict';
import {
  buildReviewStatusMap,
  entriesWithReviewRows,
  reviewStatusForGroup,
  statusForTreeEntry,
} from './fileTreeChangeStatus.js';

function run(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  } catch (err) {
    console.error('not ok - ' + name);
    throw err;
  }
}

run('reviewStatusForGroup: 空到非空标记为 U', () => {
  assert.equal(reviewStatusForGroup({
    file: '333.txt',
    totalAdditions: 1,
    totalDeletions: 0,
    hunks: [{ old_count: 0, new_count: 1 }],
  }), 'U');
});

run('reviewStatusForGroup: 非空到空标记为 D', () => {
  assert.equal(reviewStatusForGroup({
    file: 'old.txt',
    totalAdditions: 0,
    totalDeletions: 2,
    hunks: [{ old_count: 2, new_count: 0 }],
  }), 'D');
});

run('reviewStatusForGroup: 普通编辑标记为 M', () => {
  assert.equal(reviewStatusForGroup({
    file: 'main.cpp',
    totalAdditions: 2,
    totalDeletions: 1,
    hunks: [{ old_count: 3, new_count: 4 }],
  }), 'M');
});

run('statusForTreeEntry: 文件 exact match,目录聚合子级状态', () => {
  const statuses = buildReviewStatusMap([
    { file: 'src/deep/main.cpp', totalAdditions: 1, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 1 }] },
    { file: 'src/new.txt', totalAdditions: 1, totalDeletions: 0, hunks: [{ old_count: 0, new_count: 1 }] },
  ]);

  assert.equal(statusForTreeEntry({ path: 'src', kind: 'dir' }, statuses), 'M');
  assert.equal(statusForTreeEntry({ path: 'src/new.txt', kind: 'file' }, statuses), 'U');
});

run('entriesWithReviewRows: 审查里有但文件列表未返回的直接子项会补行', () => {
  const statuses = buildReviewStatusMap([
    { file: 'gone.txt', totalAdditions: 0, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 0 }] },
    { file: 'src/deep.cpp', totalAdditions: 1, totalDeletions: 1, hunks: [{ old_count: 1, new_count: 1 }] },
  ]);

  const root = entriesWithReviewRows([], '', statuses);
  assert.deepEqual(root, [{
    name: 'gone.txt',
    path: 'gone.txt',
    kind: 'file',
    review_status: 'D',
    review_synthetic: true,
  }]);

  const src = entriesWithReviewRows([], 'src', statuses);
  assert.deepEqual(src, [{
    name: 'deep.cpp',
    path: 'src/deep.cpp',
    kind: 'file',
    review_status: 'M',
    review_synthetic: true,
  }]);
});
