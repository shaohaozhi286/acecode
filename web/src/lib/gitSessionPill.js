// GitSessionPill 的纯状态逻辑(openspec add-webui-git-session-pill)。
// 组件只做 DOM 映射;这里的函数全部可在 Node 下单测。
//
// 语义(上游对话拍板):
//  - 分支选择只服务于 worktree 基线;未勾 worktree 时分支控件禁用,
//    不能从这里切换主仓分支。
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
} = {}) {
  if (!gitInfo || !gitInfo.is_repo) {
    return { visible: false };
  }

  const inWorktree = !!(worktreeSession && (worktreeSession.name || worktreeSession.branch));
  const started = sessionStarted || inWorktree;
  const interactive = !started && !busy;
  const branchInteractive = interactive && worktreeChecked;

  return {
    visible: true,
    branch: gitInfo.branch || 'HEAD',
    branches: Array.isArray(gitInfo.branches) ? gitInfo.branches : [],
    // 已开始 / busy → 整体只读;只有勾选 worktree 后才能选择基线分支。
    interactive,
    branchInteractive,
    worktreeChecked: inWorktree ? true : worktreeChecked,
    worktreeBadge: inWorktree ? (worktreeSession.name || worktreeSession.branch) : '',
    started,
    branchSelectMeaning: branchInteractive ? 'worktree-base' : 'disabled',
  };
}

// 首条消息发送时,pill 状态 → sendInput body 的 worktree 字段。
// 返回 null = 不带该字段(未勾选 / 会话已开始)。
export function buildWorktreeIntent({ worktreeChecked, selectedBase, sessionStarted }) {
  if (!worktreeChecked || sessionStarted) return null;
  const intent = { create: true };
  if (selectedBase) intent.base = selectedBase;
  return intent;
}
