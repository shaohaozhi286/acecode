export const DESKTOP_GUIDED_TOUR_TARGETS = Object.freeze({
  sidebar: '[data-tour-target="sidebar"]',
  addProject: '[data-tour-target="sidebar-add-project"]',
  newSession: '[data-tour-target="sidebar-new-task"]',
  workspace: '[data-tour-target="home-workspace"]',
  composer: '[data-tour-target="home-composer"]',
  status: '[data-tour-target="statusbar"]',
  settings: '[data-tour-target="topbar-settings"]',
});

export const DESKTOP_GUIDED_TOUR_TARGET_LIST = Object.freeze(
  Object.values(DESKTOP_GUIDED_TOUR_TARGETS),
);

export function desktopGuidedTourModeEligible(mode) {
  return mode === 'shell' || mode === 'webapp';
}

export function desktopGuidedTourTargetsReady(root = globalThis.document) {
  if (!root?.querySelector) return false;
  return DESKTOP_GUIDED_TOUR_TARGET_LIST.every((selector) => !!root.querySelector(selector));
}

export function desktopGuidedTourHasModel(models) {
  return Array.isArray(models) && models.length > 0;
}

export function shouldAutoStartDesktopGuidedTour({
  mode,
  authState,
  stateLoaded,
  dismissed,
  startupNavigationSettled,
  hasActiveSession,
  blocked,
  targetsReady,
  attempted,
} = {}) {
  return desktopGuidedTourModeEligible(mode)
    && authState === 'ok'
    && stateLoaded === true
    && dismissed === false
    && startupNavigationSettled === true
    && hasActiveSession === false
    && blocked === false
    && targetsReady === true
    && attempted === false;
}

export function shouldPrepareDesktopGuidedTour({
  mode,
  authState,
  startupNavigationSettled,
  hasActiveSession,
  blocked,
} = {}) {
  return desktopGuidedTourModeEligible(mode)
    && authState === 'ok'
    && startupNavigationSettled === true
    && hasActiveSession === false
    && blocked === false;
}

export function buildDesktopGuidedTourSteps({ hasModel = true } = {}) {
  return [
    {
      id: 'sidebar',
      target: DESKTOP_GUIDED_TOUR_TARGETS.sidebar,
      placement: 'right',
      title: '任务与工作区',
      content: '这里可以新建和搜索任务、继续历史任务，并在工作区之间切换。',
    },
    {
      id: 'add-project',
      target: DESKTOP_GUIDED_TOUR_TARGETS.addProject,
      placement: 'right-start',
      title: '添加工作区',
      content: '点击“工作区”栏右侧的添加按钮，选择已有的本地代码目录，它就会出现在左侧工作区列表中。引导期间只做说明，不会真的打开目录选择器。',
    },
    {
      id: 'new-session',
      target: DESKTOP_GUIDED_TOUR_TARGETS.newSession,
      placement: 'right-start',
      title: '新建任务',
      content: '左侧“新建任务”会回到新任务首页，默认使用“无工作区”。它不会创建工作区目录；要加入本地代码目录，请使用上一步的“添加工作区”。',
    },
    {
      id: 'workspace',
      target: DESKTOP_GUIDED_TOUR_TARGETS.workspace,
      placement: 'top-start',
      title: '选择任务范围',
      content: '选择代码工作区，让 ACECode 知道从哪里开始；也可以选择“不使用工作区”处理通用任务。',
    },
    {
      id: 'composer',
      target: DESKTOP_GUIDED_TOUR_TARGETS.composer,
      placement: 'top',
      title: '描述你想完成的事',
      content: '直接用自然语言交代任务，或输入 / 查看可用命令。发送后 ACECode 会在所选范围内工作。',
    },
    {
      id: 'status',
      target: DESKTOP_GUIDED_TOUR_TARGETS.status,
      placement: 'top-start',
      title: hasModel ? '模型与权限' : '先配置一个模型',
      content: hasModel
        ? '底部状态栏可以切换模型和权限模式，开始任务前随时确认当前配置。'
        : '底部状态栏会显示模型和权限。你还没有配置模型，完成指引后会带你去模型设置。',
    },
    {
      id: 'settings',
      target: DESKTOP_GUIDED_TOUR_TARGETS.settings,
      placement: 'bottom-end',
      title: '设置与再次查看',
      content: hasModel
        ? '在设置中管理模型、外观与工具。以后也可以在“常规”里重新查看这份新手指引。'
        : '最后去设置里添加模型。以后也可以在“常规”里重新查看这份新手指引。',
    },
  ];
}

export function desktopGuidedTourTerminalAction(event, { hasModel = true } = {}) {
  if (event?.type !== 'tour:end') return null;
  if (event.status === 'finished') {
    return { dismiss: true, openModels: !hasModel };
  }
  if (event.status === 'skipped') {
    return { dismiss: true, openModels: false };
  }
  return null;
}

export function desktopGuidedTourLostRequiredTarget(event) {
  return event?.type === 'error:target_not_found';
}
