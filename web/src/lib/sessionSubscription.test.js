import assert from 'node:assert/strict';
import { createSessionSubscriptionManager } from './connection.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function makeManager() {
  const calls = [];
  const manager = createSessionSubscriptionManager({
    subscribe: (sessionId) => calls.push(['subscribe', sessionId]),
    unsubscribe: (sessionId) => calls.push(['unsubscribe', sessionId]),
  });
  return { manager, calls };
}

run('retain 同一 session 两次只发送一次 subscribe', () => {
  const { manager, calls } = makeManager();
  assert.equal(manager.retain('s1'), 1);
  assert.equal(manager.retain('s1'), 2);
  assert.deepEqual(calls, [['subscribe', 's1']]);
  assert.equal(manager.count('s1'), 2);
});

run('release 一次不会取消仍被观察的 session', () => {
  const { manager, calls } = makeManager();
  manager.retain('s1');
  manager.retain('s1');
  assert.equal(manager.release('s1'), 1);
  assert.deepEqual(calls, [['subscribe', 's1']]);
  assert.equal(manager.count('s1'), 1);
});

run('release 最后一个 consumer 才发送 unsubscribe', () => {
  const { manager, calls } = makeManager();
  manager.retain('s1');
  assert.equal(manager.release('s1'), 0);
  assert.deepEqual(calls, [['subscribe', 's1'], ['unsubscribe', 's1']]);
  assert.equal(manager.count('s1'), 0);
});

run('切换 session 时只释放离开的 session', () => {
  const { manager, calls } = makeManager();
  manager.retain('s1');
  manager.retain('s2');
  manager.release('s1');
  assert.deepEqual(calls, [
    ['subscribe', 's1'],
    ['subscribe', 's2'],
    ['unsubscribe', 's1'],
  ]);
  assert.equal(manager.count('s1'), 0);
  assert.equal(manager.count('s2'), 1);
});
