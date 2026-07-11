// 连接器开关列表的纯逻辑。
//
// 数据源是 GET /api/config/connectors 返回的 {connectors: [...]}。设置页只展示
// id/name/description/enabled 四个字段,但后端每个 connector 还可能带 hooks
// (on_enable/on_auth_error,含 command/args/timeout_ms)、auth_error_scope
// (base_url_prefix)等字段 —— PUT /api/config/connectors 会严格校验这些结构,
// 回传前绝不能丢弃,否则等于清空 config.json 里的 hooks(on_enable 再也不会触发)。
// normalizeConnectorList 只兜底展示用的四个已知字段,原始对象的其余字段原样
// 透传(已知和未知的都算,面向未来字段);applyConnectorToggle 同理只翻转
// enabled,不触碰同一条目的其它字段,也不影响其它连接器。

export function normalizeConnectorList(data) {
  const raw = Array.isArray(data?.connectors) ? data.connectors : [];
  return raw
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => ({
      ...item,
      id: typeof item.id === 'string' ? item.id.trim() : '',
      name: typeof item.name === 'string' ? item.name : '',
      description: typeof item.description === 'string' ? item.description : '',
      enabled: !!item.enabled,
    }))
    .filter((item) => item.id);
}

// 在解析后的 connector 列表上翻转某一项的 enabled,返回新数组(不改源数组/源对象)。
// 只匹配 id 相同的一项,只覆盖 enabled 字段,其它字段(含 hooks / auth_error_scope /
// 任何未知字段)原样保留。
export function applyConnectorToggle(list, id, enabled) {
  const arr = Array.isArray(list) ? list : [];
  return arr.map((item) => (
    item && typeof item === 'object' && item.id === id
      ? { ...item, enabled: !!enabled }
      : item
  ));
}
