// 9 宫格视图:仅显示当前 workspace 中**已置顶**的会话(最多 8 条),
// 右下角永远是「+ 新对话」格。
//
// 数据源 = listWorkspaceSessions ∩ getPinnedSessions —— 用 sidebar 的 pin
// 状态决定哪些会话出现在宫格,跟 sidebar 的「置顶」区语义对齐。
// 详见 openspec/changes/fix-desktop-grid-show-pinned-only。
//
// 0 pin → 8 格区域整体替换为引导文案 + 右下角"新对话"格仍可点;
// 1..7 pin → 已 pin 占前几格,剩余位置显示既有「空闲」占位卡;
// >=8 pin → 取前 8 个(按 pin 顺序,新 pin 在前)。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { withNewSessionDisplayTitles } from '../lib/sessionTitle.js';
import { pickPinnedSessionsForGrid } from '../lib/gridPinnedSessions.js';
import { MiniSession } from './MiniSession.jsx';

const GRID9_PINNED_LIMIT = 8; // 9 格中第 9 格固定给「新对话」

export function Grid9View({ activeRef, onExpand, onOpenHome }) {
  const [sessions, setSessions] = useState([]);

  useEffect(() => {
    let off = false;
    const tick = () => {
      const hash = activeRef?.workspaceHash;
      if (!hash) {
        if (!off) setSessions([]);
        return;
      }
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
        setSessions(
          pickPinnedSessionsForGrid(decorated, pinnedIds, GRID9_PINNED_LIMIT),
        );
      });
    };
    tick();
    const t = setInterval(tick, 3000);
    return () => { off = true; clearInterval(t); };
  }, [activeRef?.workspaceHash]);

  const newSessionCell = (
    <button
      type="button"
      onClick={() => onOpenHome?.(activeRef)}
      className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex flex-col items-center justify-center gap-1 text-fg-mute hover:border-accent hover:bg-accent-bg hover:text-accent transition"
    >
      <span className="text-2xl">+</span>
      <span className="text-[11px]">新对话</span>
    </button>
  );

  // 0 个 pin → 8 格区域整体替换为引导文案,右下角「新对话」格仍可点。
  // 用 col-span-3 让提示横跨三列、row-span 占两行,留底行第三列给"新对话"。
  if (sessions.length === 0) {
    return (
      <div className="flex-1 overflow-hidden bg-bg p-1 grid grid-cols-3 grid-rows-3 gap-1">
        <div className="col-span-3 row-span-2 flex items-center justify-center">
          <div className="max-w-md text-center px-6 py-8 rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt">
            <div className="text-fg text-[13px] mb-1.5">宫格里还没有会话</div>
            <div className="text-fg-mute text-[11px] leading-relaxed">
              请在左侧 sidebar 把要在宫格中监控的会话置顶,
              <br />
              最多可同时监控 {GRID9_PINNED_LIMIT} 个。
            </div>
          </div>
        </div>
        {/* 第 3 行前两格留空,第 3 格放「新对话」按钮 */}
        <div className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt opacity-40" />
        <div className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt opacity-40" />
        {newSessionCell}
      </div>
    );
  }

  const idleSlots = Math.max(0, GRID9_PINNED_LIMIT - sessions.length);
  return (
    <div className="flex-1 overflow-hidden bg-bg p-1 grid grid-cols-3 grid-rows-3 gap-1">
      {sessions.map((s) => (
        <MiniSession key={s.id} session={s} compact onClick={onExpand} />
      ))}
      {Array.from({ length: idleSlots }).map((_, i) => (
        <div
          key={`empty-${i}`}
          className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex items-center justify-center text-fg-mute text-[11px]"
        >
          空闲
        </div>
      ))}
      {newSessionCell}
    </div>
  );
}
