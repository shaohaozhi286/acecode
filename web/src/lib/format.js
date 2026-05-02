// 工具函数:相对时间(刚刚 / N分钟前 / N小时前 / 日期),字节单位换算等。

export function relativeTime(ts) {
  if (!ts) return '';
  const now  = Date.now();
  const ms   = typeof ts === 'number' ? ts : Date.parse(ts);
  if (!Number.isFinite(ms)) return '';
  const diff = Math.max(0, now - ms);
  const sec  = Math.floor(diff / 1000);
  if (sec < 30)        return '刚刚';
  if (sec < 60)        return `${sec}秒前`;
  if (sec < 3600)      return `${Math.floor(sec / 60)}分钟前`;
  if (sec < 86_400)    return `${Math.floor(sec / 3600)}小时前`;
  if (sec < 86_400*7)  return `${Math.floor(sec / 86_400)}天前`;
  const d = new Date(ms);
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  return `${d.getFullYear()}-${mm}-${dd}`;
}

export function formatBytes(n) {
  if (!Number.isFinite(n)) return '';
  if (n < 1024)            return `${n}B`;
  if (n < 1024 * 1024)     return `${(n / 1024).toFixed(1)}KB`;
  return `${(n / (1024 * 1024)).toFixed(1)}MB`;
}

export function formatElapsed(s) {
  if (!Number.isFinite(s)) return '0s';
  if (s < 60)  return `${s.toFixed(1)}s`;
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
