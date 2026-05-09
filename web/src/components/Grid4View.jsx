// 4 宫格视图:仅显示当前 workspace 中**已置顶**的会话(最多 4 条)。
//
// 数据源 = listWorkspaceSessions ∩ getPinnedSessions —— 用 sidebar 的 pin
// 状态决定哪些会话出现在宫格,跟 sidebar 的「置顶」区语义对齐。
// 详见 openspec/changes/fix-desktop-grid-show-pinned-only。
//
// 0 pin → 整体替换为引导文案;1..3 pin → 已 pin 占前几格,剩余位置显示
// 既有的「空闲」虚线占位卡;>=4 pin → 取前 4 个(按 pin 顺序,新 pin 在前)。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { withNewSessionDisplayTitles } from '../lib/sessionTitle.js';
import { pickPinnedSessionsForGrid } from '../lib/gridPinnedSessions.js';
import { MiniSession } from './MiniSession.jsx';

const GRID4_LIMIT = 4;

export function Grid4View({ activeRef, onExpand }) {
  const [sessions, setSessions] = useState([]);

  useEffect(() => {
    let off = false;
    const tick = () => {
      const hash = activeRef?.workspaceHash;
      if (!hash) {
        // 没有激活 workspace 时不发请求;UI 层自然进 0-pin 空态。
        if (!off) setSessions([]);
        return;
      }
      // sessions / pinned IDs 并行拉。pinned 拉失败降级为空 ids,
      // 整个宫格走 0-pin 空态(而不是退回旧版"显示前 4 个"行为)。
      Promise.all([
        api.listWorkspaceSessions(hash).catch(() => []),
        api.getPinnedSessions(hash).catch(() => null),
      ]).then(([list, pinnedResp]) => {
        if (off) return;
        const pinnedIds = Array.isArray(pinnedResp?.session_ids)
          ? pinnedResp.session_ids
          : [];
        const decorated = withNewSessionDisplayTitles(
          Array.isArray(list) ? list : [],
        );
        setSessions(pickPinnedSessionsForGrid(decorated, pinnedIds, GRID4_LIMIT));
      });
    };
    tick();
    const t = setInterval(tick, 3000);
    return () => { off = true; clearInterval(t); };
  }, [activeRef?.workspaceHash]);

  // 0 个 pin → 居中提示,引导用户去 sidebar 操作。
  if (sessions.length === 0) {
    return (
      <div className="flex-1 overflow-hidden bg-bg p-1.5 flex items-center justify-center">
        <div className="max-w-md text-center px-6 py-8 rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt">
          <div className="text-fg text-[13px] mb-1.5">宫格里还没有会话</div>
          <div className="text-fg-mute text-[11px] leading-relaxed">
            请在左侧 sidebar 把要在宫格中监控的会话置顶,
            <br />
            最多可同时监控 {GRID4_LIMIT} 个。
          </div>
        </div>
      </div>
    );
  }

  // 已 pin 的占前几格,剩余位置仍显示既有「空闲」占位卡。
  const idleSlots = Math.max(0, GRID4_LIMIT - sessions.length);
  return (
    <div className="flex-1 overflow-hidden bg-bg p-1.5 grid grid-cols-2 grid-rows-2 gap-1.5">
      {sessions.map((s) => (
        <MiniSession key={s.id} session={s} onClick={onExpand} />
      ))}
      {Array.from({ length: idleSlots }).map((_, i) => (
        <div
          key={`empty-${i}`}
          className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex items-center justify-center text-fg-mute text-[11px]"
        >
          空闲
        </div>
      ))}
    </div>
  );
}
