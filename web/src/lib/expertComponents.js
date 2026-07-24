export const EXPERT_FILTERS = Object.freeze([
  Object.freeze({ id: 'all', label: '全部' }),
  Object.freeze({ id: 'agent', label: '专家' }),
  Object.freeze({ id: 'team', label: '专家团' }),
]);

export function normalizeExperts(value) {
  const list = Array.isArray(value) ? value : (Array.isArray(value?.experts) ? value.experts : []);
  return list
    .filter((item) => item && typeof item === 'object' && typeof item.id === 'string')
    .map((item) => ({
      ...item,
      id: String(item.id || ''),
      type: item.type === 'team' ? 'team' : 'agent',
      display_name: String(item.display_name || item.id || ''),
      profession: String(item.profession || ''),
      description: String(item.description || ''),
      source: item.source === 'workspace' ? 'workspace' : 'global',
      managed_global: item.managed_global === true,
      quick_prompts: Array.isArray(item.quick_prompts)
        ? item.quick_prompts.filter((prompt) => typeof prompt === 'string')
        : [],
      agents: Array.isArray(item.agents) ? item.agents : [],
      references_existing_experts: item.references_existing_experts === true,
      lead_expert_id: String(item.lead_expert_id || ''),
      member_expert_ids: Array.isArray(item.member_expert_ids)
        ? item.member_expert_ids.filter((id) => typeof id === 'string')
        : [],
    }));
}

export function filterExperts(experts, { query = '', type = 'all' } = {}) {
  const needle = String(query || '').trim().toLocaleLowerCase();
  return normalizeExperts(experts).filter((expert) => {
    if (type !== 'all' && expert.type !== type) return false;
    if (!needle) return true;
    return [expert.display_name, expert.profession, expert.description]
      .some((value) => String(value || '').toLocaleLowerCase().includes(needle));
  });
}

export function createExpertInternalId(type = 'agent') {
  const prefix = type === 'team' ? 'team' : 'expert';
  const randomPart = globalThis.crypto?.randomUUID
    ? globalThis.crypto.randomUUID().replaceAll('-', '').slice(0, 12)
    : Math.random().toString(36).slice(2, 14);
  return `${prefix}-${Date.now().toString(36)}-${randomPart}`.slice(0, 64);
}

export function emptyExpertForm(type = 'agent', initialExpertId = '') {
  const normalizedType = type === 'team' ? 'team' : 'agent';
  const selectedExpertIds = initialExpertId ? [String(initialExpertId)] : [];
  return {
    id: createExpertInternalId(normalizedType),
    displayName: '',
    profession: '',
    description: '',
    type: normalizedType,
    instructions: '',
    quickPromptsText: '',
    selectedExpertIds,
    leadExpertId: selectedExpertIds[0] || '',
  };
}

export function expertFormFromDetail(expert) {
  const normalized = normalizeExperts([expert])[0] || {};
  const agents = Array.isArray(expert?.agents) ? expert.agents : [];
  const lead = agents.find((agent) => agent?.id === expert?.lead_agent_id) || agents[0] || {};
  const selectedExpertIds = normalized.type === 'team'
    ? [normalized.lead_expert_id, ...normalized.member_expert_ids].filter(Boolean)
    : [];
  return {
    id: normalized.id || '',
    displayName: normalized.display_name || '',
    profession: normalized.profession || '',
    description: normalized.description || '',
    type: normalized.type || 'agent',
    instructions: normalized.type === 'agent' ? String(lead.instructions || '') : '',
    quickPromptsText: normalized.quick_prompts.join('\n'),
    selectedExpertIds,
    leadExpertId: normalized.lead_expert_id || selectedExpertIds[0] || '',
  };
}

export function validateExpertForm(form) {
  if (!/^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/.test(String(form?.id || ''))) {
    return '无法保存，请重新打开后再试';
  }
  if (!String(form?.displayName || '').trim()) return '请填写名称';
  if (form?.type === 'team') {
    const selected = Array.isArray(form.selectedExpertIds)
      ? [...new Set(form.selectedExpertIds.filter(Boolean))]
      : [];
    if (selected.length < 2) return '专家团至少需要两位专家';
    if (!form.leadExpertId || !selected.includes(form.leadExpertId)) {
      return '请选择一位主理专家';
    }
  } else if (!String(form?.instructions || '').trim()) {
    return '请填写这个专家的工作方式';
  }
  return '';
}

export function expertPayloadFromForm(form) {
  const common = {
    id: String(form.id || '').trim(),
    type: form.type === 'team' ? 'team' : 'agent',
    display_name: String(form.displayName || '').trim(),
    profession: String(form.profession || '').trim(),
    description: String(form.description || '').trim(),
    quick_prompts: String(form.quickPromptsText || '')
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean),
  };
  if (common.type === 'team') {
    const selected = [...new Set((form.selectedExpertIds || []).filter(Boolean))];
    return {
      ...common,
      lead_expert_id: String(form.leadExpertId || ''),
      member_expert_ids: selected.filter((id) => id !== form.leadExpertId),
    };
  }
  return {
    ...common,
    lead: {
      id: 'lead',
      display_name: common.display_name,
      profession: common.profession,
      instructions: String(form.instructions || '').trim(),
    },
  };
}

export function singleExpertsForTeam(experts, editingTeamId = '') {
  return normalizeExperts(experts).filter(
    (expert) => expert.type === 'agent' && expert.id !== editingTeamId,
  );
}

export function selectedTeamExperts(form, experts) {
  const byId = new Map(singleExpertsForTeam(experts).map((expert) => [expert.id, expert]));
  return (form?.selectedExpertIds || []).map((id) => byId.get(id)).filter(Boolean);
}

export function toggleTeamExpert(form, expertId) {
  const id = String(expertId || '');
  if (!id) return form;
  const selected = Array.isArray(form?.selectedExpertIds) ? form.selectedExpertIds : [];
  if (selected.includes(id)) {
    const next = selected.filter((item) => item !== id);
    return {
      ...form,
      selectedExpertIds: next,
      leadExpertId: form.leadExpertId === id ? (next[0] || '') : form.leadExpertId,
    };
  }
  return {
    ...form,
    selectedExpertIds: [...selected, id],
    leadExpertId: form.leadExpertId || id,
  };
}
