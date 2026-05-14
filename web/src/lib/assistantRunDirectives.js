// 决定 transcript 中每条 assistant 消息是否需要展示「ACECode」头像 + 名牌,
// 以及同一连续 assistant run 的底部 footer。
//
// 规则(对应用户反馈:连续 AI 输出 不需要 ACECode+输出+ACECode+输出, 而应该
// ACECode+输出+输出):
// - 用户消息出现 → 重置 run, 下一条 assistant 重新显示头像
// - 同一 run 内的多条 assistant 消息(中间穿插 tool 行 / system 行 不影响),
//   只有第一条显示头像 + "ACECode" 名牌, 其余为 continuation(无头像 / 无名牌)
// - 工具行(kind === 'tool')和折叠活动摘要不影响 run 状态
// - 空内容(trim 后为空)且非 streaming 的 assistant 消息隐藏整行, 且不消耗
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
    if (isEmpty && !it.streaming) {
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
