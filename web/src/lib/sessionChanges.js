// SidePanel「变更」tab 的核心数据源:把当前 session 的 messages 数组里所有
// `tool_end` 携带的 hunks 字段聚合成 per-file 的变更列表。
//
// 协议参考:enhance-webui-chat-rendering 给 tool_end payload 加了
// `hunks: [{file, before, after, additions, deletions, lines: [...]}]`,
// 由 src/web/tool_event_payload.cpp 序列化(只有 file_edit / file_write 工具会带)。
//
// **已知 limitation**: 用户授权 agent 用 `bash sed` / `awk` / `git checkout` 改的
// 文件**抓不到**(无 hunks metadata)。前端面板的空态 / 头部应该明示此限制。
// 后续 follow-up 思路见 openspec/changes/add-webui-side-panel/proposal.md。
//
// 输入(messages 形状,REST `/messages` 与 WS `tool_end` 都满足):
//   [
//     { role:'tool', hunks: [{file, before, after, additions, deletions, lines}] },
//     ...
//   ]
//   message 没有 hunks 字段就跳过(普通 user/assistant/system 消息不参与)。
//
// 输出:
//   [
//     {
//       file: 'src/foo.cpp',
//       hunks: [hunk1, hunk2, ...],     // 同名多次编辑全部追加,按消息顺序
//       totalAdditions: N,
//       totalDeletions: M,
//     },
//     ...
//   ]
// 顺序 = 文件名首次出现的消息顺序(稳定),便于 UI 跟踪 agent 的改动轨迹。

/**
 * @param {Array<{hunks?: Array<{file:string, additions?:number, deletions?:number, [k:string]:any}>}>} messages
 * @returns {Array<{file:string, hunks:Array<object>, totalAdditions:number, totalDeletions:number}>}
 */
export function aggregateHunksFromMessages(messages) {
  if (!Array.isArray(messages)) return [];

  // file → group{file, hunks[], totalAdditions, totalDeletions}
  // 用 Map 保持插入顺序(= 文件名首次出现顺序)
  const groups = new Map();

  for (const m of messages) {
    if (!m || !Array.isArray(m.hunks)) continue;
    for (const h of m.hunks) {
      if (!h || typeof h.file !== 'string' || !h.file) continue;
      let g = groups.get(h.file);
      if (!g) {
        g = {
          file: h.file,
          hunks: [],
          totalAdditions: 0,
          totalDeletions: 0,
        };
        groups.set(h.file, g);
      }
      g.hunks.push(h);
      g.totalAdditions += Number(h.additions || 0);
      g.totalDeletions += Number(h.deletions || 0);
    }
  }

  return Array.from(groups.values());
}
