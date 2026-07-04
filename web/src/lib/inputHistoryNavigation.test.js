import assert from 'node:assert/strict';
import {
  buildComposerHistory,
  getNextInputHistoryPointer,
  isInputHistoryNavigationMode,
  isUserComposerEdit,
  shouldNavigateInputHistory,
  userInputTextFromTranscriptItem,
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

// 回归测试:上下键翻的历史只有 per-cwd 输入历史(全局、与会话无关),
// 当前 transcript 会话里用户发过的消息翻不出来(尤其 per-cwd 文件受
// input_history.max_entries 截断、或消息来自 TUI/其它页面生命周期时)。
// 期望:历史 = per-cwd 历史 + 当前会话 user 消息;会话消息排尾部(↑ 优先翻到),
// 同文本去重保留最后一次出现。
run('合并 per-cwd 历史与当前会话 user 消息,会话消息在尾部', () => {
  const merged = buildComposerHistory({
    cwdHistory: ['old cwd a', 'old cwd b'],
    transcriptItems: [
      { kind: 'msg', role: 'user', content: 'session msg 1' },
      { kind: 'msg', role: 'assistant', content: 'reply ignored' },
      { kind: 'msg', role: 'user', content: 'session msg 2' },
    ],
  });
  assert.deepEqual(merged, ['old cwd a', 'old cwd b', 'session msg 1', 'session msg 2']);
});

run('同文本去重保留最后一次出现(会话内消息胜过 per-cwd 旧条目)', () => {
  const merged = buildComposerHistory({
    cwdHistory: ['dup', 'other', 'dup'],
    transcriptItems: [
      { kind: 'msg', role: 'user', content: 'dup' },
      { kind: 'msg', role: 'user', content: 'newest' },
    ],
  });
  // 'dup' 只出现一次,位置取最后一次出现(会话段);翻历史不会连续撞到重复文本
  assert.deepEqual(merged, ['other', 'dup', 'newest']);
});

run('跳过空白输入、非 user 消息与非 msg 项', () => {
  const merged = buildComposerHistory({
    cwdHistory: ['', '   ', 'kept'],
    transcriptItems: [
      { kind: 'msg', role: 'user', content: '   ' },
      { kind: 'tool', role: 'user', content: 'tool row' },
      { kind: 'msg', role: 'system', content: 'sys' },
      null,
    ],
  });
  assert.deepEqual(merged, ['kept']);
});

run('user 消息优先取 metadata.display_text 原文', () => {
  // 场景:/skill 命令落库的 content 是展开后的注入提示,原文在 display_text;
  // 翻历史必须还原用户敲的 '/foo args',而不是 [SYSTEM: ...] 提示文本
  const item = {
    kind: 'msg',
    role: 'user',
    content: '[SYSTEM: User invoked /foo skill] ...',
    metadata: { display_text: '/foo args' },
  };
  assert.equal(userInputTextFromTranscriptItem(item), '/foo args');
  assert.equal(
    userInputTextFromTranscriptItem({ kind: 'msg', role: 'user', content: 'plain' }),
    'plain',
  );
  assert.equal(userInputTextFromTranscriptItem({ kind: 'msg', role: 'assistant', content: 'x' }), '');
});

run('非数组输入容错返回空历史', () => {
  assert.deepEqual(buildComposerHistory({ cwdHistory: null, transcriptItems: undefined }), []);
  assert.deepEqual(buildComposerHistory(), []);
});

// 回归测试:desktop/web 输入框上下键翻历史,第一次填入历史文本后立刻失效。
// bug 表现:按 ↑ 填入历史项 → Lexical ValueSyncPlugin 把同一文本同步进编辑器时
// 触发 onChange 回声(文本与当前 value 相同)→ editedSinceHistory 被误置 true →
// 再按 ↑/↓ 被 shouldNavigateInputHistory 拒绝,只能翻一条。
// 期望:文本与当前 value 相同的 onChange 回声不算用户编辑;真实输入才算。
run('onChange 回声(文本未变)不算用户编辑', () => {
  // 场景:历史导航填入 'history b' 后,Lexical 同步回声携带同一文本
  assert.equal(isUserComposerEdit({ nextValue: 'history b', currentValue: 'history b' }), false);
  // 场景:selection-only 变化(光标移动)时 Lexical 也回调 onChange,文本相同
  assert.equal(isUserComposerEdit({ nextValue: '', currentValue: '' }), false);
});

run('文本真实变化算用户编辑', () => {
  // 场景:用户在历史文本 'history b' 上追加字符
  assert.equal(isUserComposerEdit({ nextValue: 'history bx', currentValue: 'history b' }), true);
  // 场景:用户把历史文本整段删空(应重新进入"空即可翻历史"态)
  assert.equal(isUserComposerEdit({ nextValue: '', currentValue: 'history b' }), true);
  // 场景:空输入框敲下第一个字符
  assert.equal(isUserComposerEdit({ nextValue: 'a', currentValue: '' }), true);
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
