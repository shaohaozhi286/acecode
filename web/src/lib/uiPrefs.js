export const UI_PREFS_STORAGE_KEY = 'acecode.uiPrefs.v1';

export const DEFAULT_UI_PREFS = {
  view: 'single',
  sidePanelCollapsed: false,
  sidebarCollapsed: false,
  // 右侧面板"最大化":用整个聊天主区显示 SidePanel,聊天列表/输入框被
  // 隐藏,只剩左侧 sidebar(若 sidebar 也折叠就是全屏 SidePanel)。再点一
  // 次回到默认布局。跨刷新持久化,符合"用户操作过最大化就保留状态"。
  sidePanelMaximized: false,
  showAceCodeAvatar: true,
};

const ALLOWED_VIEWS = new Set(['single', 'grid4', 'grid9']);

export function validateUiPrefs(v) {
  return v && typeof v === 'object'
    && ALLOWED_VIEWS.has(v.view)
    && typeof v.sidePanelCollapsed === 'boolean'
    && (v.sidebarCollapsed == null || typeof v.sidebarCollapsed === 'boolean')
    && (v.sidePanelMaximized == null || typeof v.sidePanelMaximized === 'boolean')
    && (v.showAceCodeAvatar == null || typeof v.showAceCodeAvatar === 'boolean');
}

export function effectiveShowAceCodeAvatar(uiPrefs) {
  return uiPrefs?.showAceCodeAvatar !== false;
}
