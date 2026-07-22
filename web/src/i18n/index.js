import { createInstance } from 'i18next';
import { initReactI18next } from 'react-i18next';
import { translationCatalogs } from './catalogs.js';
import {
  cacheLocalePreference,
  initialLocaleState,
  normalizeLocalePreference,
  resolveLocalePreference,
} from './locale.js';

const initial = initialLocaleState();

export const i18n = createInstance();
i18n.use(initReactI18next).init({
  resources: Object.fromEntries(
    Object.entries(translationCatalogs).map(([locale, translation]) => [
      locale,
      { translation },
    ]),
  ),
  lng: initial.locale,
  fallbackLng: 'zh-CN',
  supportedLngs: ['zh-CN', 'en-US'],
  load: 'currentOnly',
  initImmediate: false,
  interpolation: { escapeValue: false },
  react: { useSuspense: false },
});

let currentPreference = initial.preference;

export function tr(key, options) {
  return i18n.t(key, options);
}

export function localePreference() {
  return currentPreference;
}

export function effectiveLocale() {
  return i18n.resolvedLanguage || i18n.language || 'zh-CN';
}

export async function applyLocalePreference(preference, options = {}) {
  const normalized = normalizeLocalePreference(preference);
  const systemLocale = options.systemLocale
    || globalThis?.navigator?.languages?.[0]
    || globalThis?.navigator?.language
    || '';
  const locale = options.effectiveLocale
    || resolveLocalePreference(normalized, systemLocale);
  currentPreference = normalized;
  if (options.cache !== false) cacheLocalePreference(normalized);
  await i18n.changeLanguage(locale);
  if (typeof document !== 'undefined') {
    document.documentElement.lang = locale;
  }
  return { preference: normalized, locale };
}

if (typeof document !== 'undefined') {
  document.documentElement.lang = effectiveLocale();
}
