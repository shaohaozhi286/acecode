import assert from 'node:assert/strict';
import { relativeTime } from './format.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('relativeTime keeps every old timestamp in relative day form', () => {
  const originalNow = Date.now;
  const now = Date.UTC(2026, 6, 16, 12, 0, 0);
  const dayMs = 86_400_000;
  Date.now = () => now;

  try {
    assert.equal(relativeTime(now - 29_000), '刚刚');
    assert.equal(relativeTime(now - 45_000), '45秒前');
    assert.equal(relativeTime(now - 59 * 60_000), '59分钟前');
    assert.equal(relativeTime(now - 23 * 3_600_000), '23小时前');
    assert.equal(relativeTime(now - dayMs), '1天前');
    assert.equal(relativeTime(now - 7 * dayMs), '7天前');
    assert.equal(relativeTime(now - 400 * dayMs), '400天前');
    assert.equal(relativeTime('not-a-timestamp'), '');
  } finally {
    Date.now = originalNow;
  }
});
