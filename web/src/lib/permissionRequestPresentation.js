export function planPermissionPresentation(request) {
  const requestKind = request?.args && typeof request.args === 'object' ? request.args.kind : '';
  const isPlanEnter = request?.tool === 'EnterPlanMode' || requestKind === 'enter_plan_mode';
  const isPlanApproval = request?.tool === 'ExitPlanMode' || requestKind === 'plan_approval';
  const planText = isPlanApproval && typeof request?.args?.plan === 'string'
    ? request.args.plan
    : '';
  const planFilePath = (isPlanApproval || isPlanEnter) && typeof request?.args?.plan_file_path === 'string'
    ? request.args.plan_file_path
    : '';

  return {
    isPlanEnter,
    isPlanApproval,
    planText,
    planFilePath,
    hideAllowSession: isPlanEnter || isPlanApproval,
    title: isPlanApproval ? '计划审批' : isPlanEnter ? '进入 Plan 模式' : '权限请求',
    body: isPlanApproval
      ? 'Agent 已完成计划,请求批准后退出 Plan 模式。'
      : isPlanEnter
        ? 'Agent 请求进入 Plan 模式,先探索并写计划,批准后再改代码。'
        : 'Agent 请求执行以下操作:',
    primaryLabel: isPlanApproval ? '批准计划' : isPlanEnter ? '进入 Plan' : '允许一次',
  };
}
