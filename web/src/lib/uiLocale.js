import {
  applyLocalePreference,
  effectiveLocale,
  localePreference,
} from '../i18n/index.js';
import { normalizeLocalePreference } from '../i18n/locale.js';

function parseBridgeResult(raw) {
  if (!raw) return null;
  return typeof raw === 'string' ? JSON.parse(raw) : raw;
}

export async function syncNativeLocale(preference, scope = globalThis) {
  if (typeof scope?.aceDesktop_applyLocale !== 'function') return null;
  const result = parseBridgeResult(await scope.aceDesktop_applyLocale(preference));
  if (!result?.ok) throw new Error(result?.error || 'native locale bridge failed');
  return result;
}

export async function loadUiLocale(apiClient, options = {}) {
  const result = await apiClient.getUiLocale();
  const preference = normalizeLocalePreference(result?.locale);
  const native = await syncNativeLocale(preference, options.scope);
  return applyLocalePreference(preference, {
    effectiveLocale: native?.locale,
  });
}

export async function persistUiLocale(preference, apiClient, options = {}) {
  const next = normalizeLocalePreference(preference);
  const previous = localePreference();
  const previousEffective = effectiveLocale();
  let persisted = false;

  await applyLocalePreference(next);
  try {
    const saved = await apiClient.setUiLocale(next);
    const confirmed = normalizeLocalePreference(saved?.locale, next);
    persisted = true;
    const native = await syncNativeLocale(confirmed, options.scope);
    const applied = await applyLocalePreference(confirmed, {
      effectiveLocale: native?.locale,
    });
    const scope = options.scope || globalThis;
    if (options.reloadStaticCopy !== false
        && typeof scope?.location?.reload === 'function') {
      setTimeout(() => scope.location.reload(), 0);
    }
    return applied;
  } catch (error) {
    if (persisted) {
      try { await apiClient.setUiLocale(previous); } catch { /* best effort */ }
    }
    try { await syncNativeLocale(previous, options.scope); } catch { /* best effort */ }
    await applyLocalePreference(previous, { effectiveLocale: previousEffective });
    throw error;
  }
}
