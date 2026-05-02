// 主聊天视图:头部(会话名 + 状态 badge + ModelPicker + abort)+ 消息流 +
// InputBar + StatusBar。
//
// 消息流是 items 数组,每个 item 形如:
//   { kind: 'msg' | 'tool' | 'task_complete', id, role?, content?, ts?, streaming?, tool? }
// 工具事件用 toolBlocks 单独的 Map 存进度态,完成时 tool 卡片切到 summary。
//
// 没有 sessionId 时显示欢迎屏(空态:logo + 新建按钮 + slash 命令提示)。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createApi } from '../lib/api.js';
import { AceConnection } from '../lib/connection.js';
import { Message } from './Message.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { InputBar } from './InputBar.jsx';
import { StatusBar } from './StatusBar.jsx';
import { ModelPicker } from './ModelPicker.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';
import { sessionDisplayTitle, titleFromMessages } from '../lib/sessionTitle.js';

// 列表项 id 生成 — 进度模式下要稳定,所以以 tool 名 + 起始 seq 当 key
let _idSeq = 0;
function nextId() { return ++_idSeq; }

function normalizeSessionRef(sessionRef, sessionId) {
  if (sessionRef && typeof sessionRef === 'object') return sessionRef;
  if (typeof sessionRef === 'string' && sessionRef) return { sessionId: sessionRef };
  if (sessionId) return { sessionId };
  return null;
}

export function ChatView({ sessionRef, sessionId, onSessionPromoted, health, onPermissionRequest, onQuestionRequest }) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef, sessionId), [sessionRef, sessionId]);
  const sid = ref?.sessionId || ref?.id || '';
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token]);
  const connection = useMemo(() => new AceConnection(), []);
  const [items,    setItems]    = useState([]);
  const [busy,     setBusy]     = useState(false);
  const [turns,    setTurns]    = useState(0);
  const [history,  setHistory]  = useState([]);
  const [model,    setModel]    = useState('—');
  const [title,    setTitle]    = useState('');
  const scrollRef = useRef(null);
  const toolMap   = useRef(new Map()); // tool name → item.id (本地数组里的 ID)
  const streamingId = useRef(null);

  // 自动滚到底
  useEffect(() => {
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [items, busy]);

  // 拉 history(per-cwd)
  useEffect(() => {
    const cwd = health?.cwd || '';
    if (!cwd) return;
    api.getHistory(cwd, 200)
      .then((r) => setHistory(Array.isArray(r) ? r : []))
      .catch(() => {});
  }, [health, api]);

  // 监听 desktop "新对话" 事件
  useEffect(() => {
    const handler = async () => {
      try {
        const r = await api.createSession({});
        const id = r && (r.session_id || r.id);
        if (id) onSessionPromoted?.({ ...(ref || {}), sessionId: id });
      } catch (e) {
        toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') });
      }
    };
    window.addEventListener('ace:new-session', handler);
    return () => window.removeEventListener('ace:new-session', handler);
  }, [api, onSessionPromoted, ref]);

  // 切 sessionId 后:重置状态 + 拉历史 + bind ws
  useEffect(() => {
    setItems([]); toolMap.current.clear(); streamingId.current = null;
    setBusy(false); setTurns(0);
    if (!sid) { connection.unbind(); return; }
    connection.reconfigure({ port: ref?.port || '', token: ref?.token || '' });
    setTitle(sessionDisplayTitle(ref, sid));
    connection.bind(sid);

    let off = false;
    api.getMessages(sid, 0).then((data) => {
      if (off || !data) return;
      const msgs = data.messages || [];
      const initialItems = msgs.map((m) => ({
        kind: 'msg', id: nextId(), role: m.role, content: m.content || '', ts: m.ts || Date.now(),
      }));
      const restoredTitle = titleFromMessages(msgs);
      if (restoredTitle) setTitle(restoredTitle);
      setItems(initialItems);
      // 把残留事件回放
      (data.events || []).forEach((ev) => onWsMessage(ev));
    }).catch((e) => {
      toast({ kind: 'err', text: '加载会话失败:' + (e.message || '') });
    });
    return () => { off = true; connection.unbind(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sid, ref?.port, ref?.token, api, connection]);

  const onWsMessage = useCallback((msg) => {
    const t = msg.type;
    const p = msg.payload || {};
    setItems((prev) => {
      let next = prev;
      switch (t) {
        case 'message':
          streamingId.current = null;
          next = [...prev, {
            kind: 'msg', id: nextId(), role: p.role || 'system',
            content: p.content || '', ts: Date.now(),
          }];
          break;
        case 'token': {
          if (streamingId.current == null) {
            const id = nextId();
            streamingId.current = id;
            next = [...prev, { kind: 'msg', id, role: 'assistant', content: p.text || '', ts: Date.now(), streaming: true }];
          } else {
            next = prev.map((x) =>
              x.id === streamingId.current ? { ...x, content: (x.content || '') + (p.text || '') } : x);
          }
          break;
        }
        case 'tool_start': {
          streamingId.current = null;
          const id = nextId();
          toolMap.current.set(p.tool || '_anon', id);
          const tool = {
            isTaskComplete: !!p.is_task_complete,
            isDone: false,
            success: null,
            title: p.display_override || p.command_preview || `${p.tool || ''}  ${JSON.stringify(p.args || {})}`,
            tailLines: [], currentPartial: '', totalLines: 0, totalBytes: 0, elapsed: 0,
            summary: p.is_task_complete ? { object: (p.args && p.args.summary) || '完成' } : null,
            output: '',
          };
          next = [...prev, { kind: 'tool', id, tool }];
          break;
        }
        case 'tool_update': {
          const id = toolMap.current.get(p.tool || '_anon');
          if (!id) break;
          next = prev.map((x) => {
            if (x.id !== id || x.kind !== 'tool') return x;
            return { ...x, tool: {
              ...x.tool,
              tailLines: p.tail_lines || x.tool.tailLines,
              currentPartial: p.current_partial || '',
              totalLines: p.total_lines || x.tool.totalLines,
              totalBytes: p.total_bytes || x.tool.totalBytes,
              elapsed: p.elapsed_seconds || x.tool.elapsed,
            }};
          });
          break;
        }
        case 'tool_end': {
          const id = toolMap.current.get(p.tool || '_anon');
          toolMap.current.delete(p.tool || '_anon');
          if (!id) break;
          next = prev.map((x) => {
            if (x.id !== id || x.kind !== 'tool') return x;
            return { ...x, tool: {
              ...x.tool,
              isDone: true,
              success: !!p.success,
              summary: p.summary || x.tool.summary,
              output: p.output || '',
            }};
          });
          break;
        }
        case 'busy_changed':
          setBusy(!!p.busy);
          if (!p.busy) {
            setTurns((n) => n + 1);
            // 完成 streaming 标记移除
            if (streamingId.current != null) {
              const sid = streamingId.current;
              streamingId.current = null;
              next = prev.map((x) => x.id === sid ? { ...x, streaming: false } : x);
            }
          }
          break;
        case 'done':
          setBusy(false);
          if (streamingId.current != null) {
            const sid = streamingId.current;
            streamingId.current = null;
            next = prev.map((x) => x.id === sid ? { ...x, streaming: false } : x);
          }
          break;
        case 'error':
          setBusy(false);
          toast({ kind: 'err', text: '错误:' + (p.reason || '') });
          break;
        case 'permission_request':
          onPermissionRequest?.(p);
          break;
        case 'question_request':
          onQuestionRequest?.(p);
          break;
        default: break;
      }
      return next;
    });
  }, [onPermissionRequest, onQuestionRequest]);

  // ws 事件订阅
  useEffect(() => {
    const handler = (e) => onWsMessage(e.detail);
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [onWsMessage]);

  const submit = useCallback((text) => {
    if (!sid) {
      // 自动新建一个会话
      api.createSession({}).then((r) => {
        const id = r && (r.session_id || r.id);
        if (id) {
          onSessionPromoted?.({ ...(ref || {}), sessionId: id });
          // 等下一帧让 sessionId 写回 + bind 完成 — 先不发,提示用户再发
          toast({ kind: 'info', text: '会话已创建,请重发消息' });
        }
      }).catch((e) => toast({ kind: 'err', text: '新建会话失败:' + (e.message || '') }));
      return;
    }
    connection.sendUserInput(text);
    api.appendHistory(text).catch(() => {});
    setHistory((h) => [...h, text]);
    if (!ref?.title) setTitle(text);
    setItems((prev) => [...prev, { kind: 'msg', id: nextId(), role: 'user', content: text, ts: Date.now() }]);
  }, [sid, api, connection, ref, onSessionPromoted]);

  const abort = useCallback(() => connection.sendAbort(), []);

  const status = useMemo(() => {
    if (!sid) return null;
    return busy ? 'running' : 'idle';
  }, [sid, busy]);

  // 空态:没选会话
  if (!sid) {
    return (
      <div className="flex-1 flex flex-col">
        <div className="h-9 px-3 flex items-center bg-surface border-b border-border shrink-0">
          <span className="text-fg-mute text-[12px]">未选择会话</span>
        </div>
        <div className="flex-1 flex flex-col items-center justify-center text-center p-8">
          <img src="/acecode-logo.png" alt="ACECode" width="64" height="64" className="mb-4 select-none" draggable="false" />
          <h2 className="text-lg font-semibold mb-2">开始一个新对话</h2>
          <p className="text-fg-2 text-sm max-w-md leading-relaxed mb-6">
            ACECode 是终端 AI 编码代理 — 让 Agent 帮你读写文件、执行命令、调用工具。
            从下方输入开始,或点 / 查看可用命令。
          </p>
          <button
            type="button"
            onClick={() => window.dispatchEvent(new CustomEvent('ace:new-session'))}
            className="px-4 h-9 rounded-md bg-accent text-white text-sm font-medium hover:opacity-90 transition"
          >
            + 新建会话
          </button>
          <div className="mt-8 grid grid-cols-2 gap-3 max-w-lg w-full">
            {[
              { icon: '📝', title: '编辑代码', desc: '让 Agent 帮你重构、修 bug、加测试' },
              { icon: '🔍', title: '探索代码库', desc: '问"这个函数在哪里被调用"' },
              { icon: '⚙️', title: '运行命令', desc: 'bash / npm / git 等,Agent 会逐步确认' },
              { icon: '🧠', title: '使用 Skills', desc: '预定义工作流,从侧边栏开启' },
            ].map((c, i) => (
              <div key={i} className="text-left bg-surface border border-border-soft rounded-lg p-3">
                <div className="text-xl mb-1">{c.icon}</div>
                <div className="text-[13px] font-semibold mb-0.5">{c.title}</div>
                <div className="text-[11px] text-fg-mute leading-relaxed">{c.desc}</div>
              </div>
            ))}
          </div>
        </div>
        <InputBar
          history={history}
          onSubmit={submit}
          placeholder="输入消息开始新会话…"
        />
        <StatusBar model="—" turns={0} branch={health?.branch || ''} />
      </div>
    );
  }

  return (
    <div className="flex-1 flex flex-col min-w-0">
      <div className="h-9 px-3 flex items-center justify-between bg-surface border-b border-border shrink-0 gap-2">
        <div className="flex items-center gap-2 min-w-0">
          <span className="text-[13px] font-semibold text-fg truncate">{title}</span>
          <span
            className={clsx(
              'px-2.5 py-0.5 rounded-full text-[10px] font-medium border whitespace-nowrap',
              status === 'running' && 'bg-ok-bg text-ok border-ok-border',
              status === 'idle'    && 'bg-surface-hi text-fg-mute border-transparent',
            )}
          >
            {status === 'running' ? '运行中' : '空闲'}
          </span>
        </div>
        <div className="flex items-center gap-2">
          <ModelPicker sessionId={sid} apiClient={api} />
        </div>
      </div>

      <div ref={scrollRef} className="flex-1 overflow-y-auto px-3.5 py-3 flex flex-col gap-3">
        {items.map((it) => {
          if (it.kind === 'tool')          return <ToolBlock key={it.id} entry={it.tool} />;
          return <Message key={it.id} role={it.role} content={it.content} ts={it.ts} streaming={it.streaming} />;
        })}
        {busy && streamingId.current == null && (
          <div className="flex gap-2 max-w-[85%]">
            <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center mt-[2px]">A</div>
            <div className="flex gap-1 py-2 px-3">
              {[0,1,2].map((i) => (
                <span
                  key={i}
                  className="w-1.5 h-1.5 rounded-full bg-fg-mute"
                  style={{ animation: `ace-pulse 1.2s ease-in-out ${i * 0.2}s infinite` }}
                />
              ))}
            </div>
          </div>
        )}
      </div>

      <InputBar
        busy={busy}
        history={history}
        onSubmit={submit}
        onAbort={abort}
      />
      <StatusBar model={model} turns={turns} branch={health?.branch || ''} />
    </div>
  );
}
