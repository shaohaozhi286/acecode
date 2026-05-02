// 远程访问/loopback token 模式时的鉴权页。提交后调 onSubmit(token)。
// 出错信息走 inline 红字,不弹 toast(此时还没有 root toast)。

import { useState } from 'react';
import { VsIcon } from './Icon.jsx';

export function TokenPrompt({ onSubmit }) {
  const [value, setValue] = useState('');
  const [busy,  setBusy]  = useState(false);
  const [err,   setErr]   = useState('');

  const submit = async (e) => {
    e?.preventDefault?.();
    if (!value.trim()) return;
    setBusy(true); setErr('');
    try {
      await onSubmit(value.trim());
    } catch (e) {
      setErr('认证失败:' + (e.message || ''));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="h-full w-full flex items-center justify-center bg-bg p-6">
      <form
        onSubmit={submit}
        className="w-full max-w-md bg-surface border border-border rounded-xl p-6 ace-shadow-lg"
      >
        <div className="flex items-center gap-2 mb-1">
          <VsIcon name="lock" size={16} />
          <h2 className="text-base font-semibold">需要访问令牌</h2>
        </div>
        <p className="text-fg-2 text-xs mb-4 leading-relaxed">
          从 <code className="bg-surface-hi px-1 py-0.5 rounded font-mono text-[11px]">~/.acecode/run/token</code>{' '}
          或运行{' '}
          <code className="bg-surface-hi px-1 py-0.5 rounded font-mono text-[11px]">acecode daemon status</code>{' '}
          查看 token,粘贴到下方。
        </p>
        <input
          type="password"
          autoFocus
          value={value}
          onChange={(e) => setValue(e.target.value)}
          placeholder="X-ACECode-Token"
          className="w-full h-9 px-3 text-xs rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent focus:ring-2 focus:ring-accent/20 transition"
        />
        {err && <div className="text-danger text-xs mt-2">{err}</div>}
        <button
          type="submit"
          disabled={busy || !value.trim()}
          className="mt-4 w-full h-9 rounded-md bg-accent text-white text-xs font-medium hover:opacity-90 disabled:opacity-50 disabled:cursor-not-allowed transition"
        >
          {busy ? '验证中…' : '提交'}
        </button>
      </form>
    </div>
  );
}
