// 决定 transcript 中每条 assistant 消息的连续 run 边界和底部 footer。
//
// 规则:
// - 用户消息出现 → 重置 run
// - 同一 run 内的多条 assistant 消息(中间穿插 tool 行 / system 行 不影响),
//   只有第一条是非 continuation,其余为 continuation
// - 工具行(kind === 'tool')和折叠活动摘要不影响 run 状态
// - 空内容(trim 后为空)的 assistant 消息隐藏整行, 且不消耗
//   header 名额(避免 LLM 仅发起 tool_call 时, 头一个空气泡偷走 header)
// - 同一 run 内只有最后一条可见 assistant 消息显示时间和复制/分叉操作栏

export function buildAssistantRunDirectives(items) {
  const directives = new Map();
  if (!Array.isArray(items)) return directives;
  let headerUsed = false;
  let visibleAssistantIds = [];

  const finalizeRun = () => {
    const footerId = visibleAssistantIds[visibleAssistantIds.length - 1];
    for (const id of visibleAssistantIds) {
      const directive = directives.get(id);
      if (directive) directive.showFooter = id === footerId;
    }
    visibleAssistantIds = [];
  };

  for (const it of items) {
    if (!it || it.kind !== 'msg') continue;
    if (it.role === 'user') {
      finalizeRun();
      headerUsed = false;
      continue;
    }
    if (it.role !== 'assistant') continue;
    const content = typeof it.content === 'string' ? it.content : '';
    const isEmpty = !content.trim();
    if (isEmpty) {
      directives.set(it.id, { hide: true });
      continue;
    }
    directives.set(it.id, { showHeader: !headerUsed, showFooter: false });
    visibleAssistantIds.push(it.id);
    headerUsed = true;
  }
  finalizeRun();
  return directives;
}
