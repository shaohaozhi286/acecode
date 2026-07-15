import { DEFAULT_SINGLE_LAYOUT } from './singleLayout.js';

export const TOPBAR_QUICK_ACTIONS = Object.freeze([
  Object.freeze({
    id: 'new-session',
    label: '新对话',
    icon: 'newSession',
    iconSize: 16,
    callback: 'onNewSession',
    group: 'navigation',
  }),
  Object.freeze({
    id: 'new-loop',
    label: '新建循环',
    icon: 'alarm',
    iconSize: 16,
    callback: 'onOpenLoop',
    group: 'navigation',
  }),
  Object.freeze({
    id: 'find-content',
    label: '查找内容',
    icon: 'search',
    iconSize: 14,
    callback: 'onOpenSearch',
    group: 'navigation',
  }),
  Object.freeze({
    id: 'settings',
    label: '设置',
    icon: 'settings',
    iconSize: 16,
    callback: 'onSettings',
    group: 'application',
  }),
  Object.freeze({
    id: 'about',
    label: '关于 ACECode',
    icon: 'info',
    iconSize: 16,
    callback: 'onAbout',
    group: 'application',
  }),
  Object.freeze({
    id: 'check-updates',
    label: '检查更新',
    icon: 'refresh',
    iconSize: 16,
    callback: 'onCheckUpdates',
    group: 'application',
  }),
  Object.freeze({
    id: 'exit',
    label: '退出 ACECode',
    icon: 'close',
    iconSize: 16,
    callback: 'onExit',
    group: 'exit',
  }),
]);

export function topBarQuickActionNeedsSeparator(index, actions = TOPBAR_QUICK_ACTIONS) {
  return Number.isInteger(index)
    && index > 0
    && index < actions.length
    && actions[index - 1]?.group !== actions[index]?.group;
}

export function topBarQuickActionsMenuWidth(sidebarWidth) {
  return typeof sidebarWidth === 'number' && Number.isFinite(sidebarWidth) && sidebarWidth > 0
    ? Math.round(sidebarWidth)
    : DEFAULT_SINGLE_LAYOUT.sidebar;
}

export function invokeTopBarQuickAction(actionId, callbacks = {}) {
  const action = TOPBAR_QUICK_ACTIONS.find((candidate) => candidate.id === actionId);
  if (!action) return false;
  const handler = callbacks[action.callback];
  if (typeof handler !== 'function') return false;
  handler();
  return true;
}
