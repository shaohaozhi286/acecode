// /api/lsp/status 响应 → 聊天头部 LSP 指示器的展示模型(纯函数,Node 单测)。
//
// 后端返回 {enabled, servers:[{server_id, root, open_files}]},servers 只含
// root 落在当前会话 cwd 之内的已连接 server。指示器仅在 active(有 server)时显示。

export function normalizeLspStatus(raw) {
  const enabled = !!(raw && raw.enabled);
  const list = raw && Array.isArray(raw.servers) ? raw.servers : [];
  const servers = list
    .filter((s) => s && typeof s.server_id === 'string' && s.server_id)
    .map((s) => ({
      serverId: s.server_id,
      // root 为后端展示串(workspace 相对,根目录时是 ".")。
      root: typeof s.root === 'string' ? s.root : '',
      openFiles:
        Number.isFinite(s.open_files) && s.open_files > 0
          ? Math.floor(s.open_files)
          : 0,
    }));
  return { enabled, servers, active: servers.length > 0 };
}

// 指示器主体文案:单个 server 显示其名字,多个显示 "N 个 LSP"。
export function lspIndicatorLabel(status) {
  const n = status.servers.length;
  if (n === 0) return '';
  if (n === 1) return status.servers[0].serverId;
  return `${n} 个 LSP`;
}

// hover tooltip 每行:server 名 + 可选 root + 可选打开文件数。
export function lspServerLine(server) {
  const parts = [server.serverId];
  if (server.root && server.root !== '.') parts.push(server.root);
  if (server.openFiles > 0) parts.push(`${server.openFiles} 个文件`);
  return parts.join(' · ');
}
