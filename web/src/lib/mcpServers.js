// MCP「启用服务器」开关列表的纯逻辑。
//
// 数据源是 GET /api/mcp 返回的对象(键=server 名,值=该 server 的配置片段)。
// 设置页把这份对象喂给 buildMcpServerList,得到可渲染的开关行数组。
// enabled = !disabled —— 配置里缺省 disabled 字段一律视为启用。

// stdio → 「本地」,sse/http → 大写协议名。
export function mcpTransportLabel(transport) {
  if (transport === 'sse') return 'SSE';
  if (transport === 'http') return 'HTTP';
  return '本地';
}

// 单行副标题:stdio 显示 "command args...",远程显示 url。
export function mcpCommandLine(server) {
  if (!server || typeof server !== 'object') return '';
  const transport = typeof server.transport === 'string' ? server.transport : 'stdio';
  if (transport === 'sse' || transport === 'http') {
    return typeof server.url === 'string' ? server.url : '';
  }
  const command = typeof server.command === 'string' ? server.command : '';
  const args = Array.isArray(server.args)
    ? server.args.filter((a) => typeof a === 'string')
    : [];
  return [command, ...args].join(' ').trim();
}

// 把 /api/mcp 的对象转成开关行数组,保留原始键顺序(即用户 JSON 里的顺序)。
export function buildMcpServerList(cfgMap) {
  if (!cfgMap || typeof cfgMap !== 'object' || Array.isArray(cfgMap)) return [];
  return Object.keys(cfgMap).map((name) => {
    const server = cfgMap[name] && typeof cfgMap[name] === 'object' ? cfgMap[name] : {};
    const transport = typeof server.transport === 'string' ? server.transport : 'stdio';
    return {
      name,
      transport,
      transportLabel: mcpTransportLabel(transport),
      commandLine: mcpCommandLine(server),
      enabled: !server.disabled,
    };
  });
}

export function countEnabledMcp(list) {
  if (!Array.isArray(list)) return 0;
  return list.reduce((n, s) => n + (s && s.enabled ? 1 : 0), 0);
}

// 在解析后的 mcp 配置对象上就地翻转某个 server 的 disabled 字段,返回新对象
// (不改原对象)。enabled=true 时删掉 disabled 键保持稀疏;false 时写 disabled:true。
// 设置页用它把开关结果同步回 JSON 编辑器文本,避免两处显示不一致。
export function applyMcpToggle(cfgMap, name, enabled) {
  const base = cfgMap && typeof cfgMap === 'object' && !Array.isArray(cfgMap) ? cfgMap : {};
  const next = {};
  for (const key of Object.keys(base)) {
    const server = base[key] && typeof base[key] === 'object' ? { ...base[key] } : base[key];
    if (key === name && server && typeof server === 'object') {
      if (enabled) delete server.disabled;
      else server.disabled = true;
    }
    next[key] = server;
  }
  return next;
}
