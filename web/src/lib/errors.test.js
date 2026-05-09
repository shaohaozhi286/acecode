// web/src/lib/errors.test.js
import assert from 'node:assert/strict';
import { lookupErrorMessage } from './errors.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('已知 code 返中文', () => {
  assert.equal(lookupErrorMessage('NAME_TAKEN'), '已存在同名条目');
});

run('未识别 code 退 fallback', () => {
  assert.equal(lookupErrorMessage('UNKNOWN_X', '后端原文'), '后端原文');
});

run('null code + fallback', () => {
  assert.equal(lookupErrorMessage(null, '兜底'), '兜底');
});

run('空 code 与无 fallback → 默认未知错误', () => {
  assert.equal(lookupErrorMessage(null), '未知错误');
});
