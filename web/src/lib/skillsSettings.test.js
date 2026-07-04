// skillsSettings.js 的单元测试。
//
// 设置页「技能」tab 的过滤/分组/计数全部走这些纯函数;组件层只做渲染。
// 覆盖:
//  - normalizeSkillList 对脏数据(非数组 / 缺 name / 未知 source)的容错
//  - filterSkills 同时匹配 name 与 description、大小写不敏感、空查询直通
//  - groupSkillsBySource 项目/全局二分,幽灵条目(source="")落全局组
//  - skillsEnabledSummary 的 N/M 文案

import assert from 'node:assert/strict';
import {
  filterSkills,
  groupSkillsBySource,
  normalizeSkillList,
  skillsEnabledSummary,
} from './skillsSettings.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('normalizeSkillList 非数组输入返回空数组', () => {
  assert.deepEqual(normalizeSkillList(null), []);
  assert.deepEqual(normalizeSkillList(undefined), []);
  assert.deepEqual(normalizeSkillList({}), []);
});

run('normalizeSkillList 丢掉无 name 条目;未知 source 归 global', () => {
  const out = normalizeSkillList([
    { name: 'a', description: 'x', source: 'project', enabled: true },
    { name: '', description: 'dropped' },
    { description: 'no name at all' },
    // 幽灵禁用条目:后端给 source=""
    { name: 'ghost', description: '', source: '', enabled: false },
    { name: 'weird', source: 'somewhere-else', enabled: 1 },
  ]);
  assert.deepEqual(out, [
    { name: 'a', description: 'x', source: 'project', enabled: true },
    { name: 'ghost', description: '', source: 'global', enabled: false },
    { name: 'weird', description: '', source: 'global', enabled: true },
  ]);
});

const SKILLS = normalizeSkillList([
  { name: 'code-review', description: '代码审查和建议', source: 'project', enabled: true },
  { name: 'git-workflow', description: 'Git 操作自动化', source: 'global', enabled: true },
  { name: 'test-writer', description: '自动生成测试用例', source: 'global', enabled: false },
]);

run('filterSkills 空查询/全空白查询直通原列表', () => {
  assert.equal(filterSkills(SKILLS, ''), SKILLS);
  assert.equal(filterSkills(SKILLS, '   '), SKILLS);
  assert.equal(filterSkills(SKILLS, null), SKILLS);
});

run('filterSkills 按名称匹配(大小写不敏感)', () => {
  const out = filterSkills(SKILLS, 'GIT');
  assert.deepEqual(out.map((s) => s.name), ['git-workflow']);
});

run('filterSkills 按 description 匹配(用户要求:搜索同时搜描述)', () => {
  const out = filterSkills(SKILLS, '测试用例');
  assert.deepEqual(out.map((s) => s.name), ['test-writer']);
});

run('filterSkills 无命中返回空数组', () => {
  assert.deepEqual(filterSkills(SKILLS, 'nonexistent-keyword'), []);
});

run('groupSkillsBySource 项目/全局二分并保序', () => {
  const { project, global } = groupSkillsBySource(SKILLS);
  assert.deepEqual(project.map((s) => s.name), ['code-review']);
  assert.deepEqual(global.map((s) => s.name), ['git-workflow', 'test-writer']);
});

run('skillsEnabledSummary 统计全量启用数(图稿右上角 N/M 已启用)', () => {
  const summary = skillsEnabledSummary(SKILLS);
  assert.equal(summary.enabled, 2);
  assert.equal(summary.total, 3);
  assert.equal(summary.label, '2 / 3 已启用');
});

run('skillsEnabledSummary 空列表', () => {
  assert.equal(skillsEnabledSummary([]).label, '0 / 0 已启用');
});
