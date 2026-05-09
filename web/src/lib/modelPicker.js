// web/src/lib/modelPicker.js
// 当顶栏下拉的当前值不在 list 里(saved_models 改名/删除导致),插一条
// disabled 灰条显式提示用户。

export function isCurrentValueOrphaned(currentName, list) {
  if (!currentName) return false;
  if (!Array.isArray(list)) return false;
  return list.findIndex((m) => m && m.name === currentName) === -1;
}

export function buildOptionsWithOrphan(currentName, list) {
  if (!Array.isArray(list)) return [];
  const out = [...list];
  if (isCurrentValueOrphaned(currentName, list)) {
    out.unshift({
      name: currentName,
      provider: '?',
      model: '?',
      is_legacy: false,
      orphan: true,
      label: `${currentName} (已被改名/删除)`,
    });
  }
  return out;
}
