// 「后台任务」面板(spawn_subagent 子会话)。
//
// 归属主会话窗口:作为 ChatView transcript 区(flex 行)的一个静态 flex 兄弟项,
// 打开时**挤压**聊天消息区(而非浮层遮挡),关闭时返回 null 不占位;只影响
// 聊天会话区,不动 Sidebar/右侧文件预览面板、也不压输入框(输入框在 transcript
// 区之外仍占满宽)。数据与操作全部来自 useSubagentTasks(ChatView 持有),
// 本组件只渲染。
//
// 两个视图:
//   - 列表:运行中 / 已完成 分组卡片。运行中卡片右上有中止(stop);
//     已完成组标题行有「清除」(purge 全部已结束任务,永久删除)。
//   - transcript:点卡片「查看会话」原地切换,复用主会话的 Message/ToolBlock
//     渲染(紧凑只读)。AskUserQuestion 工具行不显示——子代理的提问/权限
//     确认冒泡到主会话 UI 回答,这里只看执行过程。

import { useEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { useSessionTranscript } from '../lib/sessionTranscript.js';
import {
  SUBAGENT_TASK_STATUS,
  formatElapsed,
  subagentTaskGroups,
  taskDisplayTitle,
  taskElapsedSeconds,
  taskStatsParts,
  taskStatusLabel,
} from '../lib/subagentTasks.js';
import { VsIcon } from './Icon.jsx';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';

function TaskCard({ task, nowMs, onAbort, onOpenTranscript }) {
  const running = task.status === SUBAGENT_TASK_STATUS.RUNNING;
  const stats = taskStatsParts(task);
  const elapsed = formatElapsed(taskElapsedSeconds(task, nowMs));
  return (
    <div className="rounded-lg border border-border bg-surface px-3 py-2.5 flex flex-col gap-1 ace-shadow">
      <div className="flex items-start gap-2">
        <span className="flex-1 min-w-0 text-[12.5px] font-semibold text-fg break-words">
          {taskDisplayTitle(task)}
        </span>
        {running && (
          <button
            type="button"
            onClick={() => onAbort?.(task)}
            className="w-6 h-6 shrink-0 rounded-md border border-border text-fg-mute flex items-center justify-center transition hover:text-danger hover:border-danger/40 hover:bg-danger-bg"
            title="中止任务"
            aria-label="中止任务"
          >
            <VsIcon name="stop" size={12} />
          </button>
        )}
      </div>
      <div className="flex items-center gap-1.5 text-[11px] text-fg-2">
        <span>Agent</span>
        {running ? (
          <>
            <span className="w-1.5 h-1.5 rounded-full bg-ok shadow-[0_0_5px_var(--ace-ok)]" />
            <span className="tabular-nums">{elapsed}</span>
          </>
        ) : (
          <span className="text-fg-mute">
            {taskStatusLabel(task)} {elapsed}
          </span>
        )}
      </div>
      {stats.length > 0 && (
        <div className="text-[11px] text-fg-mute truncate" title={stats.join(' · ')}>
          {stats.join(' · ')}
        </div>
      )}
      <button
        type="button"
        onClick={() => onOpenTranscript?.(task)}
        className="self-start text-[11.5px] text-accent hover:underline"
      >
        查看会话
      </button>
    </div>
  );
}

// AskUserQuestion 的工具行不进窄条(冒泡到主会话回答);其余 kind 里只保留
// msg + tool,聚合/提示类行(activity_summary 等)在简化视图中省略。
function transcriptItemsForPanel(items) {
  return (Array.isArray(items) ? items : []).filter((it) => {
    if (it.kind === 'tool') return (it.tool?.tool || '') !== 'AskUserQuestion';
    return it.kind === 'msg';
  });
}

function SubagentTranscriptView({ task }) {
  const sessionRef = useMemo(() => ({
    sessionId: task.id,
    busy: task.status === SUBAGENT_TASK_STATUS.RUNNING,
    title: taskDisplayTitle(task),
  }), [task.id, task.status, task.title, task.summary]);
  const transcript = useSessionTranscript(sessionRef, { live: 'auto' });
  const items = useMemo(
    () => transcriptItemsForPanel(transcript.items),
    [transcript.items]);

  // 跟尾:接近底部时新内容到达自动滚到底;用户上翻则不打扰。
  const scrollRef = useRef(null);
  const followRef = useRef(true);
  useEffect(() => {
    const el = scrollRef.current;
    if (el && followRef.current) el.scrollTop = el.scrollHeight;
  }, [items]);

  return (
    <div
      ref={scrollRef}
      onScroll={(e) => {
        const el = e.currentTarget;
        followRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 48;
      }}
      className="ace-subagent-transcript flex-1 min-h-0 overflow-y-auto px-2.5 py-2 flex flex-col gap-2"
    >
      {transcript.loadState === 'loading' && (
        <div className="text-[12px] text-fg-mute px-1 py-2 flex items-center gap-2">
          <span className="ace-spinner" /> 加载会话记录…
        </div>
      )}
      {transcript.loadState === 'error' && (
        <div className="text-[12px] text-danger px-1 py-2">加载失败:{transcript.error || ''}</div>
      )}
      {items.map((it) => (
        <div key={it.id} data-chat-row="true">
          {it.kind === 'tool' ? (
            <ToolBlock
              entry={it.tool}
              sessionRunning={transcript.busy || task.status === SUBAGENT_TASK_STATUS.RUNNING}
            />
          ) : (
            <Message
              role={it.role}
              content={it.content}
              contentParts={it.contentParts}
              ts={it.ts}
              streaming={it.streaming}
              messageId={it.messageId}
              metadata={it.metadata}
              showFooter={false}
            />
          )}
        </div>
      ))}
      {transcript.loadState === 'loaded' && items.length === 0 && (
        <div className="text-[12px] text-fg-mute px-1 py-2">暂无会话内容</div>
      )}
    </div>
  );
}

export function SubagentPanel({ open, focus, onClose, tasks, onAbort, onClearSettled }) {
  const [transcriptTaskId, setTranscriptTaskId] = useState('');
  const [clearing, setClearing] = useState(false);

  // 面板关闭后回到列表视图,重开不残留上一次的 transcript。
  useEffect(() => {
    if (!open) setTranscriptTaskId('');
  }, [open]);

  // 外部(聊天流「调用了 N 个智能体」分组点某个智能体)请求定位到某子会话。
  // focus.n 单调递增,同一 id 的重复点击也会重新触发。
  useEffect(() => {
    if (focus?.id) setTranscriptTaskId(focus.id);
  }, [focus?.n, focus?.id]);

  const groups = useMemo(() => subagentTaskGroups(tasks), [tasks]);
  // 目标任务不在列表(如已清除但聊天流仍留有分组项)时,合成一个最小任务对象,
  // transcript 仍能按 session_id 拉取展示。
  const transcriptTask = transcriptTaskId
    ? (tasks.find((t) => t.id === transcriptTaskId)
       || { id: transcriptTaskId, status: SUBAGENT_TASK_STATUS.COMPLETED, title: '', summary: '' })
    : null;

  // 运行中卡片的耗时每秒 tick(仅面板打开且列表视图有运行中任务时)。
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    if (!open || transcriptTask || groups.running.length === 0) return undefined;
    const timer = setInterval(() => setNowMs(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [open, transcriptTask, groups.running.length]);

  if (!open) return null;

  const clearSettled = async () => {
    if (clearing) return;
    setClearing(true);
    try {
      await onClearSettled?.();
    } finally {
      setClearing(false);
    }
  };

  return (
    <div
      className="shrink-0 w-[380px] max-w-[85%] h-full flex flex-col bg-surface border-l border-border"
      data-subagent-panel="true"
    >
      <div className="h-10 px-3 flex items-center gap-2 border-b border-border shrink-0">
        {transcriptTask ? (
          <>
            <button
              type="button"
              onClick={() => setTranscriptTaskId('')}
              className="w-7 h-7 rounded-md text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg"
              title="返回任务列表"
              aria-label="返回任务列表"
            >
              <VsIcon name="arrowLeft" size={14} />
            </button>
            <span className="flex-1 min-w-0 text-[13px] font-semibold text-fg truncate">
              {taskDisplayTitle(transcriptTask)}
            </span>
          </>
        ) : (
          <>
            <VsIcon name="embedding" size={15} className="text-fg-2" />
            <span className="flex-1 text-[13px] font-semibold text-fg">后台任务</span>
          </>
        )}
        <button
          type="button"
          onClick={onClose}
          className="w-7 h-7 rounded-md text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg"
          title="关闭"
          aria-label="关闭后台任务面板"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>

      {transcriptTask ? (
        <SubagentTranscriptView key={transcriptTask.id} task={transcriptTask} />
      ) : (
        <div className="flex-1 min-h-0 overflow-y-auto px-3 py-3 flex flex-col gap-3">
          {tasks.length === 0 && (
            <div className="text-[12px] text-fg-mute py-4 text-center">
              当前会话还没有后台任务。
              <br />让代理调用 spawn_subagent 即可在这里看到。
            </div>
          )}
          {groups.running.length > 0 && (
            <div className="flex flex-col gap-2">
              <div className="text-[11.5px] font-medium text-fg-2">运行中</div>
              {groups.running.map((task) => (
                <TaskCard
                  key={task.id}
                  task={task}
                  nowMs={nowMs}
                  onAbort={onAbort}
                  onOpenTranscript={(t) => setTranscriptTaskId(t.id)}
                />
              ))}
            </div>
          )}
          {groups.settled.length > 0 && (
            <div className="flex flex-col gap-2">
              <div className="flex items-center justify-between">
                <span className="text-[11.5px] font-medium text-fg-2">
                  已完成 {groups.settled.length}
                </span>
                <button
                  type="button"
                  onClick={clearSettled}
                  disabled={clearing}
                  className={clsx(
                    'text-[11.5px] text-fg-mute transition hover:text-danger',
                    clearing && 'opacity-50 cursor-default',
                  )}
                  title="永久删除全部已结束任务(不影响主会话)"
                >
                  {clearing ? '清除中…' : '清除'}
                </button>
              </div>
              {groups.settled.map((task) => (
                <TaskCard
                  key={task.id}
                  task={task}
                  nowMs={nowMs}
                  onAbort={onAbort}
                  onOpenTranscript={(t) => setTranscriptTaskId(t.id)}
                />
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
