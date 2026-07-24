import assert from 'node:assert/strict';
import {
  createExpertInternalId,
  emptyExpertForm,
  expertPayloadFromForm,
  filterExperts,
  normalizeExperts,
  selectedTeamExperts,
  singleExpertsForTeam,
  toggleTeamExpert,
  validateExpertForm,
} from './expertComponents.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('expert list filtering uses plain-language fields rather than internal IDs', () => {
  const experts = normalizeExperts({ experts: [
    { id: 'reviewer', display_name: '代码审查', type: 'agent', source: 'global' },
    { id: 'delivery-team', display_name: '交付团队', type: 'team', profession: '研发交付' },
  ] });
  assert.equal(experts.length, 2);
  assert.deepEqual(filterExperts(experts, { type: 'team' }).map((item) => item.id), ['delivery-team']);
  assert.deepEqual(filterExperts(experts, { query: '审查' }).map((item) => item.id), ['reviewer']);
  assert.deepEqual(filterExperts(experts, { query: 'delivery-team' }), []);
});

test('new expert forms generate valid hidden IDs', () => {
  assert.match(createExpertInternalId('agent'), /^expert-[a-z0-9-]+$/);
  assert.match(createExpertInternalId('team'), /^team-[a-z0-9-]+$/);
});

test('expert team payload references selected existing experts', () => {
  const form = emptyExpertForm('team', 'reviewer');
  form.displayName = '交付团队';
  form.profession = '研发交付';
  form.quickPromptsText = '实现这个功能\n审查当前改动';
  form.selectedExpertIds = ['reviewer', 'tester'];
  form.leadExpertId = 'reviewer';
  assert.equal(validateExpertForm(form), '');
  assert.deepEqual(expertPayloadFromForm(form), {
    id: form.id,
    type: 'team',
    display_name: '交付团队',
    profession: '研发交付',
    description: '',
    quick_prompts: ['实现这个功能', '审查当前改动'],
    lead_expert_id: 'reviewer',
    member_expert_ids: ['tester'],
  });
});

test('team selection preserves the draft and reassigns the lead when removed', () => {
  let form = emptyExpertForm('team', 'reviewer');
  form.displayName = '交付团队';
  assert.match(validateExpertForm(form), /至少需要两位/);
  form = toggleTeamExpert(form, 'tester');
  assert.deepEqual(form.selectedExpertIds, ['reviewer', 'tester']);
  assert.equal(form.leadExpertId, 'reviewer');
  form = toggleTeamExpert(form, 'reviewer');
  assert.deepEqual(form.selectedExpertIds, ['tester']);
  assert.equal(form.leadExpertId, 'tester');
  assert.equal(form.displayName, '交付团队');
});

test('team picker only offers existing single experts and resolves selections', () => {
  const experts = normalizeExperts([
    { id: 'reviewer', display_name: '审查专家', type: 'agent' },
    { id: 'tester', display_name: '测试专家', type: 'agent' },
    { id: 'delivery-team', display_name: '交付团队', type: 'team' },
  ]);
  assert.deepEqual(
    singleExpertsForTeam(experts).map((item) => item.id),
    ['reviewer', 'tester'],
  );
  assert.deepEqual(
    selectedTeamExperts(
      { selectedExpertIds: ['tester', 'delivery-team'] },
      experts,
    ).map((item) => item.id),
    ['tester'],
  );
});
