import assert from 'node:assert/strict';
import {
  LOOP_TEMPLATES,
  buildLoopPayload,
  defaultLoopForm,
  loopRunPresentation,
  loopScheduleLabel,
  loopFormForTemplate,
  validateLoopForm,
} from './loops.js';

assert.equal(LOOP_TEMPLATES.length, 4);
assert.deepEqual(LOOP_TEMPLATES.map((item) => item.name), [
  '每日代码健康巡检', '定时测试与问题修复', '每周代码变更总结', '依赖与技术债巡检',
]);
const defaults = defaultLoopForm('model-a', Date.UTC(2026, 6, 13));
assert.equal(defaults.permissionMode, 'yolo');
assert.equal(defaults.scheduleKind, 'period');
assert.equal(validateLoopForm(defaults), '请输入循环名称');

const form = { ...defaults, name: 'Review', prompt: 'Review code', workspaceHash: 'h1' };
assert.equal(validateLoopForm(form), '');
const payload = buildLoopPayload(form, [{ hash: 'h1', cwd: 'C:/repo' }]);
assert.equal(payload.workspace_cwd, 'C:/repo');
assert.equal(payload.permission_mode, 'yolo');
assert.equal(payload.schedule.kind, 'period');
assert.equal(Object.hasOwn(payload, 'schedule_expr'), false);

assert.equal(loopScheduleLabel({ kind: 'period', period: 'workdays', hour: 9, minute: 5 }), '工作日 09:05');
assert.match(loopScheduleLabel({ kind: 'interval', interval_value: 2, interval_unit: 'hours' }), /每 2 小时/);
assert.match(loopRunPresentation({ status: 'missed', reason: 'workspace_busy' }).reason, /不会补跑/);
const weekly = loopFormForTemplate(LOOP_TEMPLATES[2], 'model-a', Date.UTC(2026, 6, 13));
assert.equal(weekly.period, 'weekly');
assert.deepEqual(weekly.weekdays, [5]);
