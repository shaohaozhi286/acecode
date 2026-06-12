// 控制台 + 旁 shell 下拉框纯逻辑(plan: 控制台 Shell 选择器)。
// 归一化 /api/pty/shells 响应、构造下拉项,供 ConsoleDock 渲染。不碰 DOM,Node 单测覆盖。

// 后端响应 → { shells:[{id,label,available,needsPath}], defaultId }。防御坏数据。
export function normalizeShells(resp) {
  const list = Array.isArray(resp?.shells) ? resp.shells : [];
  const shells = list
    .filter((s) => s && typeof s.id === 'string' && s.id.length > 0)
    .map((s) => ({
      id: s.id,
      label: typeof s.label === 'string' && s.label ? s.label : s.id,
      available: s.available !== false,
      needsPath: s.needs_path === true,
    }));
  const defaultId = typeof resp?.default === 'string' ? resp.default : '';
  return { shells, defaultId };
}

// 构造下拉菜单项(标注当前默认项)。
export function buildShellMenuItems(shells, defaultId) {
  return (Array.isArray(shells) ? shells : []).map((s) => ({
    id: s.id,
    label: s.label || s.id,
    available: s.available !== false,
    needsPath: s.needsPath === true,
    isDefault: s.id === defaultId,
  }));
}

// 按 id 找 shell(找不到返回 null)。
export function shellById(shells, id) {
  return (Array.isArray(shells) ? shells : []).find((s) => s && s.id === id) || null;
}
