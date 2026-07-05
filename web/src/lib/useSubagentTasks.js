// 后台任务(spawn_subagent 子会话)的数据 hook。挂在 ChatView(主会话视图)。
//
// 职责:
//   1. REST GET /api/sessions?parent=<id> 拉任务快照(parent 切换时重置)。
//   2. 监听 connection 消息:父会话的 spawn_subagent tool_end → refetch;
//      子会话事件 → applySubagentSessionEvent 增量(纯函数,见 subagentTasks.js)。
//   3. 对运行中的子任务保持 WS 订阅(connection.retainSession)——
//      **不依赖面板是否打开**:这是子会话 permission_request / question_request
//      能到达 App 全局监听并冒泡到主会话 UI 的前提。已结束任务不订阅。
//   4. 操作:中止(sendAbort + 本地标记)、清除(purge REST + 本地移除)。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from './api.js';
import { connection } from './connection.js';
import {
  SUBAGENT_TASK_STATUS,
  applySubagentSessionEvent,
  markSubagentTaskAborted,
  mergeSubagentTaskList,
  removeSubagentTask,
  runningSubagentCount,
} from './subagentTasks.js';

export function useSubagentTasks(parentSessionId) {
  const [tasks, setTasks] = useState([]);
  const retainedRef = useRef(new Set());
  const parentRef = useRef(parentSessionId);
  useEffect(() => { parentRef.current = parentSessionId; }, [parentSessionId]);
  const taskIdsRef = useRef(new Set());
  useEffect(() => { taskIdsRef.current = new Set(tasks.map((t) => t.id)); }, [tasks]);

  const refresh = useCallback(async () => {
    if (!parentSessionId) return;
    try {
      const sessions = await api.listSessions({ parent: parentSessionId });
      // 迟到响应守卫:切换主会话后,旧 parent 的响应直接丢弃。
      if (parentRef.current !== parentSessionId) return;
      setTasks((prev) => mergeSubagentTaskList(prev, Array.isArray(sessions) ? sessions : []));
    } catch {
      // 静默:列表拉取失败不打断主会话;下一次事件/打开面板会重试。
    }
  }, [parentSessionId]);

  useEffect(() => {
    setTasks([]);
    if (!parentSessionId) return;
    refresh();
  }, [parentSessionId, refresh]);

  useEffect(() => {
    if (!parentSessionId) return undefined;
    const handler = (event) => {
      const msg = event.detail || {};
      const sid = msg.session_id || msg.payload?.session_id || '';
      if (sid === parentSessionId) {
        if (msg.type === 'tool_end') {
          const p = msg.payload || {};
          if ((p.tool === 'spawn_subagent' && p.metadata?.subagent_session_id) ||
              p.tool === 'wait_subagent') {
            refresh();
          }
        }
        return;
      }
      // spawn_subagent(wait=true) 期间父 turn 还没产生 tool_end,新子会话的
      // 出现只能靠 workspace session_status 广播感知:未知 busy 会话 → refetch
      // (?parent= 过滤保证不会把无关会话混进来)。
      if (msg.type === 'session_status') {
        const p = msg.payload || {};
        const statusSid = p.session_id || '';
        if (statusSid && p.busy === true && !taskIdsRef.current.has(statusSid)) {
          refresh();
          return;
        }
      }
      setTasks((prev) => applySubagentSessionEvent(prev, msg));
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [parentSessionId, refresh]);

  // 运行中任务的订阅管理:目标集合 = running task ids,与已 retain 集合 diff。
  useEffect(() => {
    const target = new Set(
      tasks.filter((t) => t.status === SUBAGENT_TASK_STATUS.RUNNING).map((t) => t.id));
    const retained = retainedRef.current;
    for (const id of target) {
      if (!retained.has(id)) {
        connection.retainSession(id);
        retained.add(id);
      }
    }
    for (const id of [...retained]) {
      if (!target.has(id)) {
        connection.releaseSession(id);
        retained.delete(id);
      }
    }
  }, [tasks]);

  // 卸载 / parent 切换:释放全部订阅。
  useEffect(() => () => {
    for (const id of retainedRef.current) connection.releaseSession(id);
    retainedRef.current = new Set();
  }, [parentSessionId]);

  const abortTask = useCallback((id) => {
    if (!id) return;
    connection.sendAbort(id);
    setTasks((prev) => markSubagentTaskAborted(prev, id));
  }, []);

  const purgeTask = useCallback(async (id) => {
    if (!id) return;
    await api.purgeSession(id);
    setTasks((prev) => removeSubagentTask(prev, id));
  }, []);

  const clearSettled = useCallback(async () => {
    const settled = tasks.filter((t) => t.status !== SUBAGENT_TASK_STATUS.RUNNING);
    const results = await Promise.allSettled(settled.map((t) => api.purgeSession(t.id)));
    const removed = new Set(
      settled.filter((_, i) => results[i].status === 'fulfilled').map((t) => t.id));
    if (removed.size > 0) {
      setTasks((prev) => prev.filter((t) => !removed.has(t.id)));
    }
    const failed = results.filter((r) => r.status === 'rejected').length;
    return { removed: removed.size, failed };
  }, [tasks]);

  return {
    tasks,
    runningCount: runningSubagentCount(tasks),
    refresh,
    abortTask,
    purgeTask,
    clearSettled,
  };
}
