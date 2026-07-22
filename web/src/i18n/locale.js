export const GUI_LOCALE_STORAGE_KEY = 'acecode.guiLocale.v1';

export const GUI_LOCALE_PREFERENCES = Object.freeze([
  'auto',
  'zh-CN',
  'en-US',
]);

export const GUI_LOCALES = Object.freeze(['zh-CN', 'en-US']);

export function normalizeLocalePreference(value, fallback = 'zh-CN') {
  const candidate = typeof value === 'string' ? value.trim() : '';
  return GUI_LOCALE_PREFERENCES.includes(candidate) ? candidate : fallback;
}

export function systemLocaleToSupported(locale) {
  const normalized = typeof locale === 'string' ? locale.trim().toLowerCase() : '';
  return normalized === 'zh' || normalized.startsWith('zh-') ? 'zh-CN' : 'en-US';
}

export function resolveLocalePreference(preference, systemLocale = '') {
  const normalized = normalizeLocalePreference(preference);
  return normalized === 'auto' ? systemLocaleToSupported(systemLocale) : normalized;
}

export function initialLocaleState(scope = globalThis) {
  const injectedPreference = normalizeLocalePreference(
    scope?.__ACECODE_LOCALE_PREFERENCE__,
    '',
  );
  const storedPreference = (() => {
    try {
      return normalizeLocalePreference(scope?.localStorage?.getItem(GUI_LOCALE_STORAGE_KEY), '');
    } catch {
      return '';
    }
  })();
  // A missing ui.locale belongs to pre-localization configurations. Preserve
  // their historical Chinese UI until the daemon explicitly returns a value.
  const preference = injectedPreference || storedPreference || 'zh-CN';
  const injectedLocale = GUI_LOCALES.includes(scope?.__ACECODE_LOCALE__)
    ? scope.__ACECODE_LOCALE__
    : '';
  const browserLocale = scope?.navigator?.languages?.[0]
    || scope?.navigator?.language
    || '';
  return {
    preference,
    locale: injectedLocale || resolveLocalePreference(preference, browserLocale),
  };
}

export function cacheLocalePreference(preference, scope = globalThis) {
  const normalized = normalizeLocalePreference(preference);
  try {
    scope?.localStorage?.setItem(GUI_LOCALE_STORAGE_KEY, normalized);
  } catch {
    // A disabled/private localStorage must not prevent locale switching.
  }
  return normalized;
}
