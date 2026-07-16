export const UI_PREFS_STORAGE_KEY = 'acecode.uiPrefs.v1';
export const DEFAULT_FONT_SIZE = 'medium';
export const FONT_SIZE_VALUES = ['small', 'medium', 'large'];

export const DEFAULT_UI_PREFS = {
  view: 'single',
  // 整个右侧工作区(列表 + 详情)的总折叠开关。
  sidePanelCollapsed: false,
  // 最右侧导航列表的独立折叠开关。缺省为 false,兼容旧版 v1 偏好。
  sidePanelListCollapsed: false,
  sidebarCollapsed: false,
  // 右侧面板"最大化":用整个聊天主区显示 SidePanel,聊天列表/输入框被
  // 隐藏,只剩左侧 sidebar(若 sidebar 也折叠就是全屏 SidePanel)。再点一
  // 次回到默认布局。跨刷新持久化,符合"用户操作过最大化就保留状态"。
  sidePanelMaximized: false,
  showAceCodeAvatar: false,
  fontSize: DEFAULT_FONT_SIZE,
};

const ALLOWED_VIEWS = new Set(['single', 'grid4', 'grid9']);
const ALLOWED_FONT_SIZES = new Set(FONT_SIZE_VALUES);

export function validateUiPrefs(v) {
  return v && typeof v === 'object'
    && ALLOWED_VIEWS.has(v.view)
    && typeof v.sidePanelCollapsed === 'boolean'
    && (v.sidePanelListCollapsed == null || typeof v.sidePanelListCollapsed === 'boolean')
    && (v.sidebarCollapsed == null || typeof v.sidebarCollapsed === 'boolean')
    && (v.sidePanelMaximized == null || typeof v.sidePanelMaximized === 'boolean')
    && (v.showAceCodeAvatar == null || typeof v.showAceCodeAvatar === 'boolean')
    && (v.fontSize == null || ALLOWED_FONT_SIZES.has(v.fontSize));
}

export function effectiveShowAceCodeAvatar(uiPrefs) {
  return false;
}

export function effectiveSidePanelListCollapsed(uiPrefs) {
  return uiPrefs?.sidePanelListCollapsed === true;
}

export function effectiveFontSize(uiPrefs) {
  return ALLOWED_FONT_SIZES.has(uiPrefs?.fontSize)
    ? uiPrefs.fontSize
    : DEFAULT_FONT_SIZE;
}
