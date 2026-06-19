// 斜杠命令补全使用的 workspace 选择逻辑。
//
// 命令清单必须跟随“当前输入框最终会提交到的项目”,而不是只看当前会话。
// 空态首页尚未创建 session,但用户已经在项目选择器里选定目标 workspace;
// 如果这层状态没有传给 GET /api/commands,下拉只会拿到 builtin 命令。

function pickWorkspaceHash(source) {
  if (!source || typeof source !== 'object') return '';
  return source.workspaceHash || source.workspace_hash || source.hash || '';
}

function hasExplicitHomeWorkspaceSelection(source) {
  if (!source || typeof source !== 'object') return false;
  return source.noWorkspace === true
    || Object.prototype.hasOwnProperty.call(source, 'hash')
    || Object.prototype.hasOwnProperty.call(source, 'workspaceHash')
    || Object.prototype.hasOwnProperty.call(source, 'workspace_hash');
}

export function commandWorkspaceHashForInput({ activeRef, selectedHomeWorkspace, hasSession }) {
  const activeHash = pickWorkspaceHash(activeRef);
  if (hasSession) return activeHash;
  if (hasExplicitHomeWorkspaceSelection(selectedHomeWorkspace)) {
    return pickWorkspaceHash(selectedHomeWorkspace);
  }
  return activeHash;
}
