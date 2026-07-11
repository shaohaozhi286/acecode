// InputBar 上方的"排队卡片栈"。
// 排队消息(QUEUED / SENDING / FAILED)从聊天 transcript 中独立出来,
// 作为 InputBar 紧邻上方的垂直卡片列表呈现:每张卡片自带删除按钮,
// FAILED 状态额外露出"重试"按钮;COMPLETED / CANCELLED 完全不显示。
// 设计上参考 Codex 的待发送草稿堆,与已发送 user 气泡视觉明显区分。
//
// 设计约束(取自 design.md):
//  - 空 items 时返回 null,不渲染任何容器
//  - max-height: 30vh + overflow,避免吃掉聊天可见区
//  - 卡片整体不变色 hover,按钮自身才有 hover
//  - SENDING 短暂窗口卡片仍渲染但 opacity-60,接力到 transcript 由 WS 帧驱动

import { clsx } from '../lib/format.js';
import { buildQueueCardItem } from '../lib/queueCardItem.js';
import { VsIcon } from './Icon.jsx';

function QueueCard({ card, onCancel, onRetry, onGuide, guideDisabled }) {
  const { queuedId, content, statusKind, statusLabel, dimmed, showRetry, canGuide } = card;
  return (
    <div
      role="listitem"
      data-queue-card-state={statusKind}
      className={clsx(
        'ace-queue-card relative flex items-center gap-2 pl-4 pr-2 py-2 rounded-lg border bg-surface-hi border-border text-[13px]',
        dimmed && 'opacity-60',
      )}
    >
      <span
        aria-hidden="true"
        className={clsx(
          'ace-queue-card-indicator',
          statusKind === 'failed' ? 'bg-danger' : 'bg-fg-mute/60',
        )}
      />
      <span
        className="flex-1 min-w-0 truncate text-fg"
        title={content}
      >
        {content}
      </span>
      <span
        className={clsx(
          'shrink-0 text-[11px]',
          statusKind === 'failed' ? 'text-danger' : 'text-fg-mute',
        )}
        title={statusKind === 'failed' ? statusLabel : undefined}
      >
        {statusLabel}
      </span>
      {showRetry && (
        <button
          type="button"
          aria-label="重试发送"
          onClick={() => onRetry?.(queuedId)}
          className="shrink-0 px-1.5 h-6 rounded text-[11px] text-accent hover:bg-accent-bg transition"
        >
          重试
        </button>
      )}
      {canGuide && (
        <button
          type="button"
          aria-label="将排队消息作为引导问题"
          onClick={() => onGuide?.(queuedId, content)}
          disabled={guideDisabled}
          className="shrink-0 h-6 px-2 rounded-full border border-accent/40 flex items-center gap-1 text-[11px] text-accent hover:bg-accent-bg transition disabled:opacity-50 disabled:cursor-not-allowed"
          title="不打断当前任务，立即旁路提问"
        >
          <span>引导</span>
          <VsIcon name="glyphUp" size={10} />
        </button>
      )}
      <button
        type="button"
        aria-label="取消排队"
        onClick={() => onCancel?.(queuedId)}
        className="shrink-0 w-6 h-6 rounded flex items-center justify-center text-fg-mute hover:text-fg hover:bg-surface transition"
        title="取消"
      >
        <VsIcon name="close" size={12} />
      </button>
    </div>
  );
}

export function QueueCardList({ items, onCancel, onRetry, onGuide, guideDisabled = false }) {
  const list = Array.isArray(items) ? items : [];
  if (list.length === 0) return null;
  const cards = list.map(buildQueueCardItem).filter((c) => c.queuedId);
  if (cards.length === 0) return null;
  return (
    <div
      role="list"
      aria-label="排队中的待发送消息"
      className="ace-queue-card-strip flex flex-col gap-1 px-2.5 pt-2 max-h-[30vh] overflow-y-auto"
    >
      {cards.map((card) => (
        <QueueCard
          key={card.queuedId}
          card={card}
          onCancel={onCancel}
          onRetry={onRetry}
          onGuide={onGuide}
          guideDisabled={guideDisabled}
        />
      ))}
    </div>
  );
}

export default QueueCardList;
