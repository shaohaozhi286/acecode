import assert from 'node:assert/strict';
import {
  getNextInputHistoryPointer,
  isInputHistoryNavigationMode,
  shouldNavigateInputHistory,
} from './inputHistoryNavigation.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('空输入使用历史导航模式', () => {
  assert.equal(isInputHistoryNavigationMode({ value: '', editedSinceHistory: true }), true);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowUp',
    value: '',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: -1,
  }), true);
});

run('用户编辑过的非空输入不拦截上下键', () => {
  assert.equal(isInputHistoryNavigationMode({ value: 'edited draft', editedSinceHistory: true }), false);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowUp',
    value: 'edited draft',
    editedSinceHistory: true,
    historyLength: 3,
    historyPointer: 1,
  }), false);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowDown',
    value: 'edited draft',
    editedSinceHistory: true,
    historyLength: 3,
    historyPointer: 1,
  }), false);
});

run('未编辑的历史项继续使用历史导航模式', () => {
  assert.equal(isInputHistoryNavigationMode({ value: 'history b', editedSinceHistory: false }), true);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowUp',
    value: 'history b',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: 1,
  }), true);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowDown',
    value: 'history b',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: 1,
  }), true);
});

run('清空编辑过的历史项后按当前历史指针继续浏览', () => {
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowUp',
    value: '',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: 1,
  }), true);
  assert.equal(getNextInputHistoryPointer({ key: 'ArrowUp', historyLength: 3, historyPointer: 1 }), 0);
  assert.equal(getNextInputHistoryPointer({ key: 'ArrowDown', historyLength: 3, historyPointer: 1 }), 2);
});

run('从空输入向上从最新历史开始', () => {
  assert.equal(getNextInputHistoryPointer({ key: 'ArrowUp', historyLength: 3, historyPointer: -1 }), 2);
});

run('历史末尾向下回到空输入', () => {
  assert.equal(getNextInputHistoryPointer({ key: 'ArrowDown', historyLength: 3, historyPointer: 2 }), -1);
});

run('无历史目标时不拦截', () => {
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowUp',
    value: '',
    editedSinceHistory: false,
    historyLength: 0,
    historyPointer: -1,
  }), false);
  assert.equal(shouldNavigateInputHistory({
    key: 'ArrowDown',
    value: '',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: -1,
  }), false);
  assert.equal(shouldNavigateInputHistory({
    key: 'Enter',
    value: '',
    editedSinceHistory: false,
    historyLength: 3,
    historyPointer: 1,
  }), false);
});

run('带修饰键的方向键不进入历史导航', () => {
  for (const modifier of ['altKey', 'ctrlKey', 'metaKey', 'shiftKey']) {
    assert.equal(shouldNavigateInputHistory({
      key: 'ArrowUp',
      value: '',
      editedSinceHistory: false,
      historyLength: 3,
      historyPointer: -1,
      [modifier]: true,
    }), false);
  }
});
