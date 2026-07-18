import assert from 'node:assert/strict';
import {
  isGeneratedErrorTitle,
  isUntitledNewSession,
  newSessionDisplayTitle,
  sessionDisplayTitle,
  withNewSessionDisplayTitles,
} from './sessionTitle.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('sessionDisplayTitle 不把 session id 当标题显示', () => {
  assert.equal(sessionDisplayTitle({ id: 'abc-123' }, 'abc-123'), '新会话1');
  assert.equal(sessionDisplayTitle({ sessionId: 'sid-1' }, 'sid-1'), '新会话1');
});

run('sessionDisplayTitle 优先真实标题和摘要', () => {
  assert.equal(sessionDisplayTitle({ id: 's1', title: '  标题  ' }), '标题');
  assert.equal(sessionDisplayTitle({ id: 's2', summary: '摘要' }), '摘要');
});

run('sessionDisplayTitle 忽略 generated error 标题并回退到摘要', () => {
  const session = {
    id: 's1',
    title: '  [Error] Connection failed',
    title_source: 'generated',
    summary: '恢复后的真实任务',
  };
  assert.equal(isGeneratedErrorTitle(session), true);
  assert.equal(sessionDisplayTitle(session, '新会话2'), '恢复后的真实任务');
});

run('sessionDisplayTitle 保留用户主动设置的 error 形状标题', () => {
  const session = {
    id: 's1',
    title: '[Error] 自定义故障记录',
    title_source: 'user',
  };
  assert.equal(isGeneratedErrorTitle(session), false);
  assert.equal(sessionDisplayTitle(session, '新会话2'), '[Error] 自定义故障记录');
});

run('sessionDisplayTitle 有消息但无标题时显示消息计数而不是 id', () => {
  assert.equal(
    sessionDisplayTitle({ id: 's1', message_count: 2, updated_at: '2026-05-08T00:00:00Z' }, 's1'),
    '2026-05-08T00:00:00Z  2 msgs',
  );
});

run('withNewSessionDisplayTitles 按 workspace 内创建顺序编号', () => {
  const sessions = withNewSessionDisplayTitles([
    { id: 'newer', workspace_hash: 'w1', created_at: '2026-05-08T02:00:00Z' },
    { id: 'older', workspace_hash: 'w1', created_at: '2026-05-08T01:00:00Z' },
    { id: 'other', workspace_hash: 'w2', created_at: '2026-05-08T03:00:00Z' },
  ]);
  const byId = new Map(sessions.map((s) => [s.id, s]));
  assert.equal(byId.get('older').displayTitle, '新会话1');
  assert.equal(byId.get('newer').displayTitle, '新会话2');
  assert.equal(byId.get('other').displayTitle, '新会话1');
});

run('withNewSessionDisplayTitles 不覆盖有内容的会话', () => {
  const [titled, summarized, counted] = withNewSessionDisplayTitles([
    { id: 't', title: '标题' },
    { id: 's', summary: '摘要' },
    { id: 'c', message_count: 1 },
  ]);
  assert.equal(isUntitledNewSession(titled), false);
  assert.equal(isUntitledNewSession(summarized), false);
  assert.equal(isUntitledNewSession(counted), false);
  assert.equal(titled.displayTitle, undefined);
  assert.equal(summarized.displayTitle, undefined);
  assert.equal(counted.displayTitle, undefined);
});

run('withNewSessionDisplayTitles 把无摘要的 generated error 标题视为新会话', () => {
  const [session] = withNewSessionDisplayTitles([
    {
      id: 'stale-error',
      title: '[Error] upstream unavailable',
      titleSource: 'generated',
    },
  ]);
  assert.equal(isUntitledNewSession(session), true);
  assert.equal(session.displayTitle, '新会话1');
});

run('newSessionDisplayTitle 至少从 1 开始', () => {
  assert.equal(newSessionDisplayTitle(0), '新会话1');
  assert.equal(newSessionDisplayTitle(3), '新会话3');
});
