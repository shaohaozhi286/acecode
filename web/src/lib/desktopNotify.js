// 桌面端系统通知封装。
//
// 设计参见 openspec/changes/add-desktop-attention-notifications/design.md。
// 浏览器直连 daemon(无 acecode-desktop 桌面壳)模式下,window.aceDesktop_notify
// 不存在 → 所有 notify 调用 no-op,这与 search palette 跨 workspace bridge 的
// 降级模式一致。
//
// 抑制规则(决策 4):
//   - cfg.enabled=false → 一律跳过
//   - cfg.on_question=false → question 类型跳过
//   - cfg.on_completion=false → completion 类型跳过
//   - cfg.suppress_when_focused=true 且窗口聚焦 + 事件 session 是当前可见 session
//     → 跳过(in-app sidebar 红点已经在做提醒,native 通知会重复打扰)
//
// payload 构造抽到 buildNotificationPayload — 长文本截断 80 字 + 省略号,
// 空文本回退到默认占位,纯函数,可测。

const NOTIFICATION_BODY_LIMIT = 80;
const DEFAULT_COMPLETION_BODY = '(空白回合)';

function defaultCfg() {
  return {
    enabled: true,
    on_question: true,
    on_completion: true,
    suppress_when_focused: true,
  };
}

function normalizeCfg(cfg) {
  const base = defaultCfg();
  if (!cfg || typeof cfg !== 'object') return base;
  return {
    enabled: cfg.enabled !== false,
    on_question: cfg.on_question !== false,
    on_completion: cfg.on_completion !== false,
    suppress_when_focused: cfg.suppress_when_focused !== false,
  };
}

// codepoint-aware 截断(避免在多字节字符中间断开)。
export function truncateForNotification(text, limit = NOTIFICATION_BODY_LIMIT) {
  const s = String(text == null ? '' : text);
  // Array.from 按 codepoint 拆,中文 / emoji 不会按 UTF-16 半个 surrogate 截。
  const codepoints = Array.from(s);
  if (codepoints.length <= limit) return s;
  return codepoints.slice(0, limit).join('') + '…';
}

export function buildNotificationPayload({
  type,
  sessionId,
  workspaceHash = '',
  sessionTitle = '',
  bodyText = '',
}) {
  const safeType = type === 'completion' ? 'completion' : 'question';
  const titlePrefix = safeType === 'question' ? '需要你回答' : '已完成';
  const titleSuffix = sessionTitle && String(sessionTitle).trim() ? String(sessionTitle).trim() : '会话';
  const title = `${titlePrefix} · ${titleSuffix}`;
  const trimmedBody = String(bodyText || '').trim();
  const body = trimmedBody
    ? truncateForNotification(trimmedBody)
    : (safeType === 'completion' ? DEFAULT_COMPLETION_BODY : '');
  return {
    id: `${safeType}-${sessionId || 'unknown'}-${Date.now()}`,
    workspace_hash: workspaceHash || '',
    session_id: sessionId || '',
    title,
    body,
  };
}

export function shouldSuppress(payload, activeRef, hasFocus, cfg) {
  const c = normalizeCfg(cfg);
  if (!c.enabled) return true;
  // payload.id 形如 "question-..." / "completion-...",取首段判类型。
  const type = String(payload?.id || '').split('-')[0];
  if (type === 'question' && !c.on_question) return true;
  if (type === 'completion' && !c.on_completion) return true;
  if (c.suppress_when_focused && hasFocus) {
    const sameSession = activeRef?.sessionId === payload?.session_id;
    const sameWorkspace = !payload?.workspace_hash
      || activeRef?.workspaceHash === payload?.workspace_hash;
    if (sameSession && sameWorkspace) return true;
  }
  return false;
}

function bridgeAvailable() {
  return typeof window !== 'undefined'
    && typeof window.aceDesktop_notify === 'function';
}

// 派发到 native 桥。前端必须**自己**先调 shouldSuppress 判抑制 — 这里不替你过滤,
// 让调用方明确知道事件何时被吞。返回 boolean 指示是否真投递了。
export function notify(payload) {
  if (!bridgeAvailable()) return false;
  if (!payload || (!payload.title && !payload.body)) return false;
  try {
    window.aceDesktop_notify(JSON.stringify(payload));
    return true;
  } catch {
    return false;
  }
}

// 触发"切到某 session"。toast 点击与代码主动调用走同一 native click_handler,
// UX 与 webview 内 SearchPalette 选中等场景一致。
export function focusSession(workspaceHash, sessionId) {
  if (!sessionId) return false;
  if (typeof window === 'undefined'
      || typeof window.aceDesktop_focusSession !== 'function') {
    return false;
  }
  try {
    window.aceDesktop_focusSession(JSON.stringify({
      workspace_hash: workspaceHash || '',
      session_id: sessionId,
    }));
    return true;
  } catch {
    return false;
  }
}

// 一站式入口:构造 payload + 判抑制 + 投递。前端事件源(sessionTranscript)调这一个。
// activeRef = { sessionId, workspaceHash }。hasFocus 由调用方读 document.hasFocus()。
export function maybeNotify({
  type,
  sessionId,
  workspaceHash,
  sessionTitle,
  bodyText,
  activeRef,
  hasFocus,
  cfg,
}) {
  if (!bridgeAvailable()) return false;
  const payload = buildNotificationPayload({
    type, sessionId, workspaceHash, sessionTitle, bodyText,
  });
  if (shouldSuppress(payload, activeRef, hasFocus, cfg)) return false;
  return notify(payload);
}
