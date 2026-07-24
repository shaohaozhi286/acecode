export const COLOR_THEME_STORAGE_KEY = 'ace.colorTheme';
export const DEFAULT_COLOR_THEME = 'blue';
export const COLOR_THEME_VALUES = Object.freeze(['blue', 'orange']);

const ALLOWED_COLOR_THEMES = new Set(COLOR_THEME_VALUES);

export function isValidColorTheme(value) {
  return ALLOWED_COLOR_THEMES.has(value);
}

export function effectiveColorTheme(value) {
  return isValidColorTheme(value) ? value : DEFAULT_COLOR_THEME;
}
