export function normalizeHookDiagnostic(diag = {}) {
  return {
    severity: String(diag.severity || 'warning'),
    code: String(diag.code || ''),
    message: String(diag.message || ''),
    sourceId: String(diag.source_id || diag.sourceId || ''),
    hookId: String(diag.hook_id || diag.hookId || ''),
    eventName: String(diag.event_name || diag.eventName || ''),
  };
}

export function normalizeHookForDisplay(hook = {}) {
  const trustStatus = String(hook.trust_status || hook.trustStatus || '');
  const managed = !!hook.managed || trustStatus === 'managed_trusted';
  const disabled = !!hook.disabled || trustStatus === 'disabled';
  const pendingReview = !!hook.pending_review || trustStatus === 'pending_review';
  const trusted = !!hook.trusted || trustStatus === 'trusted' || trustStatus === 'managed_trusted';
  const skipped = !!hook.skipped || trustStatus === 'skipped_unsupported';
  const matcher = String(hook.matcher || '').trim();
  return {
    id: String(hook.id || ''),
    sourceId: String(hook.source_id || hook.sourceId || ''),
    sourcePath: String(hook.source_path || hook.sourcePath || ''),
    sourceScope: String(hook.source_scope || hook.sourceScope || ''),
    sourceFormat: String(hook.source_format || hook.sourceFormat || ''),
    eventName: String(hook.event_name || hook.eventName || ''),
    matcher: matcher || '*',
    kind: String(hook.kind || ''),
    command: String(hook.command || ''),
    commandWindows: String(hook.command_windows || hook.commandWindows || ''),
    timeoutSeconds: Number(hook.timeout_seconds || hook.timeoutSeconds || 0),
    statusMessage: String(hook.status_message || hook.statusMessage || ''),
    definitionHash: String(hook.definition_hash || hook.definitionHash || ''),
    trustStatus,
    managed,
    disabled,
    pendingReview,
    trusted,
    skipped,
    skipReason: String(hook.skip_reason || hook.skipReason || ''),
    diagnostics: Array.isArray(hook.diagnostics)
      ? hook.diagnostics.map(normalizeHookDiagnostic)
      : [],
  };
}

export function normalizeHookSnapshot(snapshot = {}) {
  const hooks = Array.isArray(snapshot.hooks)
    ? snapshot.hooks.map(normalizeHookForDisplay)
    : [];
  const diagnostics = Array.isArray(snapshot.diagnostics)
    ? snapshot.diagnostics.map(normalizeHookDiagnostic)
    : [];
  return {
    featureEnabled: snapshot.feature_enabled !== false,
    sources: Array.isArray(snapshot.sources) ? snapshot.sources : [],
    hooks,
    diagnostics,
    isEmpty: hooks.length === 0,
  };
}

export function hookStatusLabel(hook = {}) {
  if (hook.managed) return '受管理';
  if (hook.disabled) return '已禁用';
  if (hook.pendingReview) return '待信任';
  if (hook.skipped) return '已跳过';
  if (hook.trusted) return '已信任';
  return '未知';
}

export function hookActionState(hook = {}) {
  const managed = !!hook.managed;
  const disabled = !!hook.disabled;
  return {
    canTrust: !!hook.pendingReview && !managed && !hook.skipped,
    canDisable: !managed && !disabled,
    canEnable: !managed && disabled,
    showDisableControl: !managed,
  };
}

export function hookEmptyState(snapshot = {}) {
  if (snapshot.featureEnabled === false) {
    return {
      title: '钩子已关闭',
      body: '启用 hooks 功能后,已配置的钩子会显示在此处',
    };
  }
  return {
    title: '未找到钩子',
    body: '已配置的钩子将显示在此处',
  };
}

export function hookSettingsErrorMessage(error) {
  if (!error) return '';
  return error.message || String(error);
}
