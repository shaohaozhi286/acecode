// 「调用了 N 个智能体」分组卡片(spawn_subagent / wait_subagent 的聚合渲染)。
//
// 折叠:一行「调用了 N 个智能体」+ 展开箭头(样式对齐 ActivitySummaryBlock)。
// 展开:逐个智能体标题行,点击 → onOpen(sessionId) 打开后台任务面板并定位到
//       该子会话的 transcript(即「后台任务」+「查看会话」的效果)。
//
// 标题解析优先级:后台任务列表实时标题(taskDisplayTitle)> spawn 时的 prompt
// 摘要 > 兜底「智能体」。运行态优先取任务状态,拿不到时退到工具项 done 标记。

import { useState } from 'react';
import { clsx } from '../lib/format.js';
import { SUBAGENT_TASK_STATUS, taskDisplayTitle } from '../lib/subagentTasks.js';
import { VsIcon } from './Icon.jsx';

function agentTitle(agent, task) {
  if (task) {
    const t = taskDisplayTitle(task);
    if (t) return t;
  }
  if (agent.prompt) return agent.prompt;
  return '智能体';
}

function agentRunning(agent, task) {
  if (task) return task.status === SUBAGENT_TASK_STATUS.RUNNING;
  return agent.done !== true;
}

function AgentRow({ agent, task, onOpen }) {
  const running = agentRunning(agent, task);
  const title = agentTitle(agent, task);
  const clickable = !!agent.sessionId;
  return (
    <button
      type="button"
      disabled={!clickable}
      onClick={() => clickable && onOpen?.(agent.sessionId)}
      title={clickable ? '查看子会话' : '子会话尚未就绪'}
      className={clsx(
        'group/agent w-full flex items-center gap-2 px-2 py-1 rounded-md text-left transition',
        clickable ? 'hover:bg-surface-hi cursor-pointer' : 'cursor-default opacity-70',
      )}
    >
      <span
        className={clsx(
          'w-1.5 h-1.5 rounded-full shrink-0',
          running ? 'bg-ok shadow-[0_0_5px_var(--ace-ok)]' : 'bg-fg-mute',
        )}
        aria-hidden="true"
      />
      <span className="flex-1 min-w-0 truncate text-[12.5px] text-fg">{title}</span>
      {clickable && (
        <VsIcon
          name="arrowRight"
          size={12}
          className="shrink-0 text-fg-mute opacity-0 group-hover/agent:opacity-100 transition-opacity"
        />
      )}
    </button>
  );
}

export function SubagentGroupBlock({ agents = [], tasksById, onOpen }) {
  const [expanded, setExpanded] = useState(false);
  const count = agents.length;
  const byId = tasksById || new Map();

  return (
    <div className="ml-8 my-1 max-w-[88%]">
      <button
        type="button"
        className="group inline-flex max-w-full items-center gap-2 px-0 py-0.5 text-left text-fg-mute/80 transition-colors"
        onClick={() => setExpanded((v) => !v)}
        title={expanded ? '收起智能体' : '展开智能体'}
        aria-label={expanded ? '收起智能体' : '展开智能体'}
      >
        <VsIcon name="embedding" size={13} className="shrink-0 opacity-80" />
        <span className="text-[12px] font-medium min-w-0 truncate group-hover:text-fg transition-colors">
          调用了 {count} 个智能体
        </span>
        <VsIcon name={expanded ? 'expandDown' : 'expandRight'} size={11} className="shrink-0 opacity-80" />
      </button>
      {expanded ? (
        <div className="mt-1 ml-1 pl-2 border-l border-border/70 flex flex-col gap-0.5">
          {agents.map((agent) => (
            <AgentRow
              key={agent.sessionId || agent.itemId}
              agent={agent}
              task={agent.sessionId ? byId.get(agent.sessionId) : null}
              onOpen={onOpen}
            />
          ))}
        </div>
      ) : (
        <div className="mt-1 h-px w-full origin-top scale-y-50 bg-fg-mute/20" aria-hidden="true" />
      )}
    </div>
  );
}
