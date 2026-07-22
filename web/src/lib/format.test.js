import assert from 'node:assert/strict';
import {
  formatBytes,
  formatCompactNumber,
  formatCount,
  formatDateTime,
  relativeTime,
} from './format.js';

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
    assert.equal(relativeTime(now - 45_000, 'en-US'), '45 seconds ago');
    assert.equal(relativeTime(now - 7 * dayMs, 'en-US'), '7 days ago');
    assert.equal(relativeTime('not-a-timestamp'), '');
  } finally {
    Date.now = originalNow;
  }
});

test('number, byte, and date formatting follows the selected locale', () => {
  assert.equal(formatCompactNumber(79_800, {}, 'en-US'), '79.8K');
  assert.equal(formatCompactNumber(79_800, {}, 'zh-CN'), '7.98万');
  assert.equal(formatBytes(1536, 'en-US'), '1.5 KB');
  assert.equal(formatCount(1, 'filesChanged', 'en-US'), '1 file changed');
  assert.equal(formatCount(2, 'filesChanged', 'en-US'), '2 files changed');
  assert.equal(formatCount(2, 'filesChanged', 'zh-CN'), '2 个文件已更改');
  assert.equal(formatDateTime(Date.UTC(2026, 6, 22, 10, 30), {
    timeZone: 'UTC', month: 'short', day: 'numeric',
  }, 'en-US'), 'Jul 22');
});
