export const TOPBAR_QUICK_ACTIONS = Object.freeze([
  Object.freeze({
    id: 'new-session',
    label: '新对话',
    icon: 'newSession',
    iconSize: 16,
    callback: 'onNewSession',
  }),
  Object.freeze({
    id: 'new-loop',
    label: '新建循环',
    icon: 'alarm',
    iconSize: 16,
    callback: 'onOpenLoop',
  }),
  Object.freeze({
    id: 'find-content',
    label: '查找内容',
    icon: 'search',
    iconSize: 14,
    callback: 'onOpenSearch',
  }),
  Object.freeze({
    id: 'settings',
    label: '设置',
    icon: 'settings',
    iconSize: 16,
    callback: 'onSettings',
  }),
]);

export function invokeTopBarQuickAction(actionId, callbacks = {}) {
  const action = TOPBAR_QUICK_ACTIONS.find((candidate) => candidate.id === actionId);
  if (!action) return false;
  const handler = callbacks[action.callback];
  if (typeof handler !== 'function') return false;
  handler();
  return true;
}
