// 聊天头部的 LSP 指示器:当前会话有已连接的 LSP server 时显示一个绿点徽标,
// hover 展开一个列出各 server(名字 / root / 打开文件数)的悬浮卡。
//
// 数据来自 GET /api/lsp/status?cwd=<会话 cwd>,只返回 root 落在该会话 workspace
// 之内的已连接 server(不触发 spawn)。server 在编辑/查询文件时惰性启动,所以
// 在 cwd 变化 与 refreshKey 变化(回合结束)时各拉一次。展示逻辑纯函数在
// lib/lspStatus.js(Node 单测)。

import { useEffect, useRef, useState } from 'react';
import { VsIcon } from './Icon.jsx';
import {
  normalizeLspStatus,
  lspIndicatorLabel,
  lspServerLine,
} from '../lib/lspStatus.js';

const EMPTY = { enabled: false, servers: [], active: false };

export function LspIndicator({ api, cwd, refreshKey }) {
  const [status, setStatus] = useState(EMPTY);
  const [hover, setHover] = useState(false);
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;

  useEffect(() => {
    let alive = true;
    if (!cwd) { setStatus(EMPTY); return undefined; }
    api.lspStatus(cwd)
      .then((r) => { if (alive && cwdRef.current === cwd) setStatus(normalizeLspStatus(r)); })
      .catch(() => { if (alive && cwdRef.current === cwd) setStatus(EMPTY); });
    return () => { alive = false; };
  }, [api, cwd, refreshKey]);

  if (!status.active) return null;
  const label = lspIndicatorLabel(status);

  return (
    <div
      className="relative shrink-0"
      onMouseEnter={() => setHover(true)}
      onMouseLeave={() => setHover(false)}
    >
      <div
        className="h-7 px-2 rounded-md flex items-center gap-1.5 text-[11px] font-medium text-ok bg-ok-bg border border-ok-border cursor-default select-none"
        aria-label={`LSP 已开启:${label}`}
      >
        <span className="w-1.5 h-1.5 rounded-full bg-ok shrink-0" aria-hidden="true" />
        <VsIcon name="code" size={13} className="opacity-90" />
        <span className="truncate max-w-[120px]">{label}</span>
      </div>
      {hover && (
        <div className="absolute top-full right-0 mt-1.5 z-50 min-w-[200px] max-w-[320px] bg-surface border border-border ace-shadow rounded-lg py-1.5 text-[12px]">
          <div className="px-3 pb-1 mb-1 text-[11px] font-semibold text-fg-mute border-b border-border/50">
            LSP 服务器（{status.servers.length}）
          </div>
          {status.servers.map((s, i) => (
            <div
              key={`${s.serverId}-${s.root}-${i}`}
              className="px-3 py-1 flex items-center gap-2 text-fg"
            >
              <span className="w-1.5 h-1.5 rounded-full bg-ok shrink-0" aria-hidden="true" />
              <span className="truncate">{lspServerLine(s)}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
