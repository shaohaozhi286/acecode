export function isActiveGoal(goal) {
  return !!goal && String(goal.status || '').toLowerCase() === 'active';
}

export function getGoalStopControlState({ goal = null, busy = false, stopping = false } = {}) {
  const activeGoal = isActiveGoal(goal);
  const isBusy = !!busy;
  const visible = isBusy || activeGoal;
  const action = isBusy ? 'abort' : activeGoal ? 'pause_goal' : 'none';
  return {
    visible,
    action,
    disabled: !!stopping,
    label: isBusy && activeGoal ? '中断 Goal' : isBusy ? '中断' : '停止 Goal',
    title: isBusy && activeGoal
      ? '中断当前任务并暂停 Goal'
      : isBusy
        ? '中断当前任务'
        : activeGoal
          ? '暂停 Goal，停止自动继续'
          : '',
  };
}

export function shouldAbortForStopControl(options = {}) {
  return getGoalStopControlState(options).action === 'abort';
}
