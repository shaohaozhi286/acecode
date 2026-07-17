import assert from 'node:assert/strict';
import {
  createDesktopNotificationMonitor,
  notificationEventKey,
} from './desktopNotificationMonitor.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('运行 session 只 retain 一次，终态只 release 一次', () => {
  const retained = [];
  const released = [];
  const monitor = createDesktopNotificationMonitor({
    retainSession: (sid) => retained.push(sid),
    releaseSession: (sid) => released.push(sid),
  });
  assert.equal(monitor.retain('s1'), true);
  assert.equal(monitor.retain('s1'), false);
  assert.equal(monitor.isRetained('s1'), true);
  assert.deepEqual(retained, ['s1']);
  assert.equal(monitor.release('s1'), true);
  assert.equal(monitor.release('s1'), false);
  assert.deepEqual(released, ['s1']);
});

run('dispose 释放所有后台 session 引用', () => {
  const released = [];
  const monitor = createDesktopNotificationMonitor({
    retainSession: () => {},
    releaseSession: (sid) => released.push(sid),
  });
  monitor.retain('s1');
  monitor.retain('s2');
  monitor.dispose();
  assert.deepEqual(released.sort(), ['s1', 's2']);
  assert.equal(monitor.isRetained('s1'), false);
});

run('request_id 与事件 seq 构造稳定去重键', () => {
  assert.equal(
    notificationEventKey('permission', 's1', {}, { request_id: 'r1' }),
    'permission:request:r1',
  );
  assert.equal(
    notificationEventKey('completion', 's1', { seq: 42 }, {}),
    'completion:session:s1:seq:42',
  );
});

run('相同事件只 markSeen 一次且集合有界', () => {
  const monitor = createDesktopNotificationMonitor({ maxSeen: 2 });
  assert.equal(monitor.markSeen('a'), true);
  assert.equal(monitor.markSeen('a'), false);
  assert.equal(monitor.markSeen('b'), true);
  assert.equal(monitor.markSeen('c'), true);
  // a 已被最早淘汰，可再次记录。
  assert.equal(monitor.markSeen('a'), true);
});
