// SearchPalette 排序与时间分桶的纯函数测试。
//
// 覆盖 design.md D2 的加权规则与 D5 的相对时间格式:
//   - 空查询 → 按 updated_at 降序
//   - title 前缀优于子串优于 fuzzy 兜底
//   - 不区分大小写,CJK 子串
//   - workspaceName 命中
//   - 时间衰减影响同档排序
//   - searchRelativeTime 的日历分桶(今天/昨天/本周/上周/上月/更早)边界

import assert from 'node:assert/strict';
import { rankSessions, scoreSession, searchRelativeTime, freshnessScore } from './searchSessions.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 把 ms 时间戳转 ISO 串(模拟后端 updated_at 字段的格式)。
function iso(ms) {
  return new Date(ms).toISOString();
}

const NOW = new Date(2026, 4, 5, 14, 0, 0).getTime(); // 2026-05-05 14:00 周二

run('空查询返回所有 session 按 updated_at 降序', () => {
  const sessions = [
    { id: 'a', title: 'A', updated_at: iso(NOW - 86400000) },
    { id: 'b', title: 'B', updated_at: iso(NOW - 3600000) },
    { id: 'c', title: 'C', updated_at: iso(NOW - 60000) },
  ];
  const r = rankSessions(sessions, '', NOW);
  assert.deepEqual(r.map((s) => s.id), ['c', 'b', 'a']);
});

run('title 前缀匹配优先于子串匹配', () => {
  const sessions = [
    { id: 'pre',  title: 'Testing flow',  updated_at: iso(NOW) },
    { id: 'sub',  title: 'Run test suite', updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, 'test', NOW);
  assert.deepEqual(r.map((s) => s.id), ['pre', 'sub']);
});

run('不区分大小写匹配', () => {
  const sessions = [
    { id: 'a', title: '提取 PDF 文本', updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, 'pdf', NOW);
  assert.equal(r.length, 1);
  assert.equal(r[0].id, 'a');
});

run('CJK 子串匹配', () => {
  const sessions = [
    { id: 'a', title: '测试 PDF 提取', updated_at: iso(NOW) },
    { id: 'b', title: '其它会话',       updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, '测试', NOW);
  assert.equal(r.length, 1);
  assert.equal(r[0].id, 'a');
});

run('summary 命中分数低于 title 命中', () => {
  const sessions = [
    { id: 't', title: 'pdf reader', summary: '其它', updated_at: iso(NOW) },
    { id: 's', title: '其它',       summary: 'pdf 解析逻辑', updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, 'pdf', NOW);
  assert.deepEqual(r.map((x) => x.id), ['t', 's']);
});

run('workspaceName 命中,无 title/summary 命中也能出现在结果中', () => {
  const sessions = [
    { id: 'a', title: 'foo bar',  workspaceName: 'acecode', updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, 'acecode', NOW);
  assert.equal(r.length, 1);
  assert.equal(r[0].id, 'a');
  // 分数应该 = 100 + freshness
  const sc = scoreSession(r[0], 'acecode', NOW);
  assert.ok(sc >= 100 && sc < 500, `score ${sc} 应该在 [100, 500)`);
});

run('fuzzy 兜底:字符按顺序出现命中,非子串', () => {
  const sessions = [
    { id: 'a', title: 'desktop frameless window', updated_at: iso(NOW) },
  ];
  // "dskw" 不是子串,但按顺序出现
  const r = rankSessions(sessions, 'dskw', NOW);
  assert.equal(r.length, 1);
  assert.equal(r[0].id, 'a');
});

run('完全不匹配过滤掉', () => {
  const sessions = [
    { id: 'a', title: 'hello world', summary: 'foo bar', workspaceName: 'baz', updated_at: iso(NOW) },
  ];
  const r = rankSessions(sessions, 'xyz', NOW);
  assert.equal(r.length, 0);
});

run('同档分数下按 updated_at 降序', () => {
  // 两条都 title 前缀匹配,且新鲜度桶相同(都在 24h 内 → freshness=50)
  const t1 = NOW - 1000;       // 比 t2 新
  const t2 = NOW - 5000;
  const sessions = [
    { id: 'old', title: 'foo bar', updated_at: iso(t2) },
    { id: 'new', title: 'foo baz', updated_at: iso(t1) },
  ];
  const r = rankSessions(sessions, 'foo', NOW);
  assert.deepEqual(r.map((s) => s.id), ['new', 'old']);
});

run('freshness 桶:今天 50,本周 30,本月 15,更早 0', () => {
  assert.equal(freshnessScore(iso(NOW - 1000), NOW), 50);          // 1s 前
  assert.equal(freshnessScore(iso(NOW - 86400000 * 3), NOW), 30);  // 3 天前
  assert.equal(freshnessScore(iso(NOW - 86400000 * 15), NOW), 15); // 15 天前
  assert.equal(freshnessScore(iso(NOW - 86400000 * 60), NOW), 0);  // 60 天前
});

run('searchRelativeTime: 今天 0:01 与 23:59 都返回"今天"', () => {
  const today0001 = new Date(2026, 4, 5, 0, 1).getTime();
  const today2359 = new Date(2026, 4, 5, 23, 59).getTime();
  assert.equal(searchRelativeTime(iso(today0001), NOW), '今天');
  assert.equal(searchRelativeTime(iso(today2359), NOW), '今天');
});

run('searchRelativeTime: 昨天与本周的边界', () => {
  // NOW = 2026-05-05 周二 14:00
  const yesterday = new Date(2026, 4, 4, 12, 0).getTime(); // 周一
  const thisMonday0001 = new Date(2026, 4, 4, 0, 1).getTime();
  const lastSunday = new Date(2026, 4, 3, 23, 59).getTime();
  assert.equal(searchRelativeTime(iso(yesterday), NOW), '昨天');
  assert.equal(searchRelativeTime(iso(thisMonday0001), NOW), '昨天');
  assert.equal(searchRelativeTime(iso(lastSunday), NOW), '上周');
});

run('searchRelativeTime: 本周/上周/上月/更早', () => {
  // NOW = 2026-05-05 周二
  // 本周内一天:周日(2026-05-03)的前一天周六 = 上周
  // 注意 dayOfWeek(周一=0..周日=6),所以 weekStart = 2026-05-04 00:00
  const sameWeek = new Date(2026, 4, 4, 8, 0).getTime();   // 周一,本周(其实是昨天会先命中)
  const lastWeek = new Date(2026, 3, 30, 12, 0).getTime(); // 4-30 周四,上周
  const lastMonth = new Date(2026, 3, 10, 0, 0).getTime(); // 4-10,上月
  const older = new Date(2025, 11, 1, 0, 0).getTime();     // 2025-12,更早
  assert.equal(searchRelativeTime(iso(sameWeek), NOW), '昨天'); // 同时命中昨天与本周,昨天优先
  assert.equal(searchRelativeTime(iso(lastWeek), NOW), '上周');
  assert.equal(searchRelativeTime(iso(lastMonth), NOW), '上月');
  assert.equal(searchRelativeTime(iso(older), NOW), '更早');
});

run('searchRelativeTime: 无效输入返回空串', () => {
  assert.equal(searchRelativeTime(null, NOW), '');
  assert.equal(searchRelativeTime('', NOW), '');
  assert.equal(searchRelativeTime('not a date', NOW), '');
});
