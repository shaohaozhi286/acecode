// Locale-aware presentation formatting. Technical identifiers and raw user
// content must bypass these helpers.

import { effectiveLocale, tr } from '../i18n/index.js';

export function formatNumber(value, options = {}, locale = effectiveLocale()) {
  const n = Number(value);
  if (!Number.isFinite(n)) return '';
  return new Intl.NumberFormat(locale, options).format(n);
}

export function formatCompactNumber(value, options = {}, locale = effectiveLocale()) {
  return formatNumber(value, {
    notation: 'compact',
    maximumFractionDigits: 2,
    ...options,
  }, locale);
}

export function formatDateTime(value, options = {}, locale = effectiveLocale()) {
  const date = value instanceof Date ? value : new Date(value);
  if (!Number.isFinite(date.getTime())) return '';
  return new Intl.DateTimeFormat(locale, options).format(date);
}

export function formatCount(count, key, locale = effectiveLocale()) {
  const numeric = Number.isFinite(Number(count)) ? Number(count) : 0;
  return tr(`counts.${key}`, {
    count: numeric,
    formattedCount: formatNumber(numeric, {}, locale),
    lng: locale,
  });
}

export function relativeTime(ts, locale = effectiveLocale()) {
  if (!ts) return '';
  const now  = Date.now();
  const ms   = typeof ts === 'number' ? ts : Date.parse(ts);
  if (!Number.isFinite(ms)) return '';
  const diff = Math.max(0, now - ms);
  const sec  = Math.floor(diff / 1000);
  if (sec < 30) return tr('format.now', { lng: locale });
  const formatter = new Intl.RelativeTimeFormat(locale, {
    numeric: 'always',
    style: locale === 'zh-CN' ? 'short' : 'long',
  });
  if (sec < 60) return formatter.format(-sec, 'second');
  if (sec < 3600) return formatter.format(-Math.floor(sec / 60), 'minute');
  if (sec < 86_400) return formatter.format(-Math.floor(sec / 3600), 'hour');
  return formatter.format(-Math.floor(sec / 86_400), 'day');
}

export function formatBytes(n, locale = effectiveLocale()) {
  if (!Number.isFinite(n)) return '';
  if (n < 1024) return `${formatNumber(n, { maximumFractionDigits: 0 }, locale)} B`;
  if (n < 1024 * 1024) {
    return `${formatNumber(n / 1024, {
      minimumFractionDigits: 1,
      maximumFractionDigits: 1,
    }, locale)} KB`;
  }
  return `${formatNumber(n / (1024 * 1024), {
    minimumFractionDigits: 1,
    maximumFractionDigits: 1,
  }, locale)} MB`;
}

export function formatElapsed(s, locale = effectiveLocale()) {
  if (!Number.isFinite(s)) return '0s';
  if (s < 60) {
    return `${formatNumber(s, {
      minimumFractionDigits: 1,
      maximumFractionDigits: 1,
    }, locale)}s`;
  }
  const m = Math.floor(s / 60);
  const r = (s - m * 60).toFixed(0);
  return `${m}m${r.padStart(2, '0')}s`;
}

export function clsx(...args) {
  const out = [];
  for (const a of args) {
    if (!a) continue;
    if (typeof a === 'string')  out.push(a);
    else if (Array.isArray(a))  out.push(clsx(...a));
    else if (typeof a === 'object') {
      for (const k of Object.keys(a)) if (a[k]) out.push(k);
    }
  }
  return out.join(' ');
}
