export const LEGACY_DEFAULT_SINGLE_LAYOUT = { sidebar: 200, sidePanel: 280 };
export const DEFAULT_SINGLE_LAYOUT = {
  sidebar: 270,
  sidePanel: 280,
  previewPanel: 640,
  previewPanelUserSized: false,
};

export const MIN_SIDEBAR_WIDTH = 160;
export const MAX_SIDEBAR_WIDTH = 360;
export const MIN_SIDE_PANEL_WIDTH = 240;
export const MIN_PREVIEW_PANEL_WIDTH = 420;
export const MIN_CHAT_WIDTH = 350;
export const MIN_SINGLE_CONTENT_WIDTH =
  MIN_CHAT_WIDTH + MIN_PREVIEW_PANEL_WIDTH + MIN_SIDE_PANEL_WIDTH;
export const MIN_SINGLE_SHELL_WIDTH = MIN_SIDEBAR_WIDTH + MIN_SINGLE_CONTENT_WIDTH;

export function validateLayoutWidths(v) {
  return v && typeof v === 'object'
    && typeof v.sidebar === 'number' && Number.isFinite(v.sidebar)
    && typeof v.sidePanel === 'number' && Number.isFinite(v.sidePanel)
    && (v.previewPanel == null
      || (typeof v.previewPanel === 'number' && Number.isFinite(v.previewPanel)))
    && (v.previewPanelUserSized == null
      || typeof v.previewPanelUserSized === 'boolean');
}

export function previewPanelWidthIsUserSized(layout) {
  if (typeof layout?.previewPanelUserSized === 'boolean') {
    return layout.previewPanelUserSized;
  }
  return typeof layout?.previewPanel === 'number'
    && Number.isFinite(layout.previewPanel)
    && layout.previewPanel !== DEFAULT_SINGLE_LAYOUT.previewPanel;
}

export function normalizeSingleLayoutPreference(layout) {
  const current = validateLayoutWidths(layout) ? layout : DEFAULT_SINGLE_LAYOUT;
  const legacyDefaults = current.sidebar === LEGACY_DEFAULT_SINGLE_LAYOUT.sidebar
    && current.sidePanel === LEGACY_DEFAULT_SINGLE_LAYOUT.sidePanel;
  const sidebar = legacyDefaults ? DEFAULT_SINGLE_LAYOUT.sidebar : current.sidebar;
  const sidePanel = legacyDefaults ? DEFAULT_SINGLE_LAYOUT.sidePanel : current.sidePanel;
  const previewPanel = current.previewPanel == null
    ? DEFAULT_SINGLE_LAYOUT.previewPanel
    : current.previewPanel;
  const previewPanelUserSized = previewPanelWidthIsUserSized(current);

  if (
    current.sidebar === sidebar
    && current.sidePanel === sidePanel
    && current.previewPanel === previewPanel
    && current.previewPanelUserSized === previewPanelUserSized
  ) {
    return current;
  }
  return {
    ...current,
    sidebar,
    sidePanel,
    previewPanel,
    previewPanelUserSized,
  };
}

function finiteRoundedWidth(value) {
  const rounded = Math.round(Number(value));
  return Number.isFinite(rounded) ? rounded : 0;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function contentWidthOptions(options) {
  if (options && typeof options === 'object') return options;
  return { contentWidth: options };
}

function visiblePanelWidth(width, visible, minWidth) {
  return visible ? Math.max(minWidth, finiteRoundedWidth(width)) : 0;
}

export function maxSidebarWidthForShell({
  shellWidth = 0,
  sidePanelWidth = 0,
  sidePanelVisible = false,
  previewPanelWidth = 0,
  previewPanelVisible = false,
} = {}) {
  if (!(shellWidth > 0)) return MAX_SIDEBAR_WIDTH;
  const sidePanelReserve = sidePanelVisible ? Math.max(0, Number(sidePanelWidth) || 0) : 0;
  const previewReserve = previewPanelVisible ? Math.max(0, Number(previewPanelWidth) || 0) : 0;
  return Math.max(MIN_SIDEBAR_WIDTH, shellWidth - sidePanelReserve - previewReserve - MIN_CHAT_WIDTH);
}

export function normalizeSidebarWidth(nextWidth, options = {}) {
  const maxByShell = maxSidebarWidthForShell(options);
  return clamp(
    finiteRoundedWidth(nextWidth),
    MIN_SIDEBAR_WIDTH,
    Math.min(MAX_SIDEBAR_WIDTH, maxByShell),
  );
}

export function maxSidePanelWidthForContent(options = 0) {
  const {
    contentWidth = 0,
    previewPanelVisible = false,
    previewPanelMaximized = false,
  } = contentWidthOptions(options);
  if (!(contentWidth > 0)) return Number.POSITIVE_INFINITY;
  const previewReserve = previewPanelVisible ? MIN_PREVIEW_PANEL_WIDTH : 0;
  const chatReserve = previewPanelMaximized ? 0 : MIN_CHAT_WIDTH;
  return Math.max(MIN_SIDE_PANEL_WIDTH, contentWidth - previewReserve - chatReserve);
}

export function normalizeSidePanelWidth(nextWidth, options = 0) {
  return clamp(
    finiteRoundedWidth(nextWidth),
    MIN_SIDE_PANEL_WIDTH,
    maxSidePanelWidthForContent(options),
  );
}

export function normalizePreviewPanelWidth(nextWidth, options = 0) {
  return clamp(
    finiteRoundedWidth(nextWidth),
    MIN_PREVIEW_PANEL_WIDTH,
    maxPreviewPanelWidthForContent(options),
  );
}

export function maxPreviewPanelWidthForContent(options = 0) {
  const {
    contentWidth = 0,
    sidePanelWidth = 0,
    sidePanelVisible = false,
    sidePanelCollapsed = false,
  } = contentWidthOptions(options);
  if (!(contentWidth > 0)) return Number.POSITIVE_INFINITY;
  const sideReserve = visiblePanelWidth(
    sidePanelWidth,
    !!sidePanelVisible && !sidePanelCollapsed,
    MIN_SIDE_PANEL_WIDTH,
  );
  return Math.max(MIN_PREVIEW_PANEL_WIDTH, contentWidth - sideReserve - MIN_CHAT_WIDTH);
}

function visibleWidth(width, visible) {
  return visible ? Math.max(0, finiteRoundedWidth(width)) : 0;
}

export function solveSingleContentLayout({
  contentWidth = 0,
  sidePanelWidth = DEFAULT_SINGLE_LAYOUT.sidePanel,
  previewPanelWidth = DEFAULT_SINGLE_LAYOUT.previewPanel,
  sidePanelVisible = true,
  sidePanelCollapsed = false,
  previewPanelVisible = false,
  previewPanelMaximized = false,
  previewPanelAutoFit = false,
} = {}) {
  const total = Math.max(0, finiteRoundedWidth(contentWidth));
  const sideVisible = !!sidePanelVisible && !sidePanelCollapsed;
  const previewVisible = !!previewPanelVisible;
  if (!(total > 0)) {
    return {
      chatWidth: 0,
      previewPanelWidth: previewVisible ? Math.max(MIN_PREVIEW_PANEL_WIDTH, finiteRoundedWidth(previewPanelWidth)) : 0,
      sidePanelWidth: sideVisible ? Math.max(MIN_SIDE_PANEL_WIDTH, finiteRoundedWidth(sidePanelWidth)) : 0,
    };
  }

  let side = visibleWidth(sidePanelWidth, sideVisible);
  if (sideVisible) side = Math.max(MIN_SIDE_PANEL_WIDTH, side);

  if (!previewVisible) {
    return {
      chatWidth: Math.max(0, total - side),
      previewPanelWidth: 0,
      sidePanelWidth: side,
    };
  }

  let preview = Math.max(MIN_PREVIEW_PANEL_WIDTH, finiteRoundedWidth(previewPanelWidth));

  if (previewPanelMaximized) {
    const available = Math.max(0, total - side);
    return {
      chatWidth: 0,
      previewPanelWidth: available,
      sidePanelWidth: side,
    };
  }

  const availableForChatAndPreview = Math.max(0, total - side);
  if (previewPanelAutoFit) {
    const balancedPreviewMaximum = Math.max(
      MIN_PREVIEW_PANEL_WIDTH,
      Math.floor(availableForChatAndPreview / 2),
    );
    preview = Math.min(preview, balancedPreviewMaximum);
  }
  let chat = availableForChatAndPreview - preview;
  if (chat < MIN_CHAT_WIDTH) {
    const maxPreviewWithChatMinimum = Math.max(
      MIN_PREVIEW_PANEL_WIDTH,
      availableForChatAndPreview - MIN_CHAT_WIDTH,
    );
    preview = Math.min(preview, maxPreviewWithChatMinimum);
    chat = Math.max(0, availableForChatAndPreview - preview);
  }

  return {
    chatWidth: chat,
    previewPanelWidth: preview,
    sidePanelWidth: side,
  };
}
