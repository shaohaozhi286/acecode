// GitSessionPill 的纯状态逻辑(openspec add-webui-git-session-pill)。
// 组件只做 DOM 映射;这里的函数全部可在 Node 下单测。
//
// 语义(上游对话拍板):
//  - 未勾 worktree 选非当前分支 = 立即 checkout(脏 → 409 dirty 往返弹
//    stash 确认);勾了 worktree 后分支下拉含义变为 worktree 基线,不动主仓。
//  - worktree 勾选是待生效状态,首条消息发送时才创建。
//  - 会话开始(有消息 / 已在 worktree)后 pill 退化为只读状态徽标。

// checkout / worktree 等外部 git 状态变更的广播事件名(GitSessionPill 发,
// GitChangesPanel 等监听并失效缓存)。detail: {cwd}。
export const GIT_STATE_CHANGED_EVENT = 'acecode:git-state-changed';

// 渲染模型:组件按这份结构画 UI。
export function buildPillModel({
  gitInfo = null,          // /api/git/info 响应(null = 未加载/失败)
  worktreeChecked = false, // 待生效的 worktree 勾选
  sessionStarted = false,  // 会话已有消息
  worktreeSession = null,  // 会话已激活的 worktree 信息 {name, branch}(可空)
  busy = false,            // 会话正在跑回合(禁交互)
  checkingOut = false,     // checkout 请求进行中(spinner)
} = {}) {
  if (!gitInfo || !gitInfo.is_repo) {
    return { visible: false };
  }

  const inWorktree = !!(worktreeSession && (worktreeSession.name || worktreeSession.branch));
  const started = sessionStarted || inWorktree;

  return {
    visible: true,
    branch: gitInfo.branch || 'HEAD',
    branches: Array.isArray(gitInfo.branches) ? gitInfo.branches : [],
    // 已开始 → 只读徽标;checkout 中 → 暂时禁交互。
    interactive: !started && !busy && !checkingOut,
    checkingOut,
    worktreeChecked: inWorktree ? true : worktreeChecked,
    worktreeBadge: inWorktree ? (worktreeSession.name || worktreeSession.branch) : '',
    started,
    // 勾了 worktree 时分支下拉的语义是"基线分支"(不 checkout)。
    branchSelectMeaning: worktreeChecked && !started ? 'worktree-base' : 'checkout',
  };
}

// checkout 失败(ApiError)→ UI 动作分派。
// 返回 {kind, files, message}:
//   dirty  → 弹 stash 确认框(files = 会被 stash 的 tracked 改动)
//   busy   → 提示"有会话正在运行,稍后再试"
//   error  → 普通错误 toast(message 可展示)
export function classifyCheckoutError(status, body) {
  if (status === 409 && body && body.error === 'dirty') {
    return { kind: 'dirty', files: Array.isArray(body.files) ? body.files : [] };
  }
  if (status === 409 && body && body.error === 'busy') {
    return { kind: 'busy' };
  }
  const detail = body && (body.detail || body.error);
  return { kind: 'error', message: detail || `HTTP ${status}` };
}

// 首条消息发送时,pill 状态 → sendInput body 的 worktree 字段。
// 返回 null = 不带该字段(未勾选 / 会话已开始)。
export function buildWorktreeIntent({ worktreeChecked, selectedBase, sessionStarted }) {
  if (!worktreeChecked || sessionStarted) return null;
  const intent = { create: true };
  if (selectedBase) intent.base = selectedBase;
  return intent;
}
