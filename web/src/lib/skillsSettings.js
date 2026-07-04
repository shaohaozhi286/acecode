// 设置页「技能」tab 的纯数据整形:归一化 / 搜索过滤 / 项目-全局分组 / 启用计数。
// 数据源是 GET /api/skills(每条 {name, description, enabled, source})。
// 组件层(SettingsPage.jsx SectionSkills)只做渲染,不承载这些逻辑。

// 归一化后端数组:丢掉无 name 的条目;source 只认 "project",其余(含
// 幽灵禁用条目的 "")一律归入 "global" 分组显示。
export function normalizeSkillList(data) {
  if (!Array.isArray(data)) return [];
  return data
    .map((item) => ({
      name: typeof item?.name === 'string' ? item.name : '',
      description: typeof item?.description === 'string' ? item.description : '',
      source: item?.source === 'project' ? 'project' : 'global',
      enabled: !!item?.enabled,
    }))
    .filter((item) => item.name);
}

// 实时搜索过滤:同时匹配技能名与 description,大小写不敏感;空查询返回原列表。
export function filterSkills(skills, query) {
  const q = String(query || '').trim().toLowerCase();
  if (!q) return skills;
  return skills.filter((s) =>
    s.name.toLowerCase().includes(q) ||
    s.description.toLowerCase().includes(q));
}

// 按来源分成「项目技能」「全局技能」两个列表,保持原有顺序。
export function groupSkillsBySource(skills) {
  const project = [];
  const global = [];
  for (const s of skills) {
    (s.source === 'project' ? project : global).push(s);
  }
  return { project, global };
}

// 右上角「N / M 已启用」摘要。基于完整列表(不受搜索过滤影响)。
export function skillsEnabledSummary(skills) {
  const total = skills.length;
  const enabled = skills.reduce((n, s) => n + (s.enabled ? 1 : 0), 0);
  return { enabled, total, label: `${enabled} / ${total} 已启用` };
}

// 工作区折叠行右侧的紧凑计数,如 "1/2"。
export function enabledRatioLabel(skills) {
  const list = Array.isArray(skills) ? skills : [];
  const enabled = list.reduce((n, s) => n + (s.enabled ? 1 : 0), 0);
  return `${enabled}/${list.length}`;
}

// 归一化 GET /api/workspaces 的返回:丢掉无 hash/cwd 的条目。该端点只返回
// 已注册(desktop_visible)的工作区,无工作区会话的临时项天然不在其中。
export function normalizeWorkspaceList(data) {
  if (!Array.isArray(data)) return [];
  return data
    .map((w) => ({
      hash: typeof w?.hash === 'string' ? w.hash : '',
      cwd: typeof w?.cwd === 'string' ? w.cwd : '',
      name: typeof w?.name === 'string' && w.name ? w.name : (w?.cwd || ''),
    }))
    .filter((w) => w.hash && w.cwd);
}

// 搜索态下某工作区是否应自动展开:已加载且有命中。未加载(skills 为 null)
// 时不展开 — 展开与否由用户点击决定。
export function workspaceAutoExpand(skills, query) {
  if (!Array.isArray(skills)) return false;
  return filterSkills(skills, query).length > 0;
}
