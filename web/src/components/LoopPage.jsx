import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import {
  LOOP_TEMPLATES,
  buildLoopPayload,
  formFromLoop,
  formatDate,
  loopRunPresentation,
  loopScheduleLabel,
  loopFormForTemplate,
  validateLoopForm,
} from '../lib/loops.js';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const WEEKDAYS = [
  [1, '一'], [2, '二'], [3, '三'], [4, '四'], [5, '五'], [6, '六'], [0, '日'],
];
const LOOP_POLL_INTERVAL_MS = 2000;

function normalizeList(value, key) {
  if (Array.isArray(value)) return value;
  return Array.isArray(value?.[key]) ? value[key] : [];
}

function Field({ label, hint = '', children }) {
  return (
    <label className="block space-y-1">
      <span className="text-[12px] font-medium text-fg-2">{label}</span>
      {hint && <span className="ml-1 text-[11px] text-fg-mute">{hint}</span>}
      {children}
    </label>
  );
}

function inputClass(extra = '') {
  return `w-full h-8 px-2.5 rounded-md border border-border bg-bg text-[12px] text-fg outline-none focus:border-accent transition ${extra}`;
}

function runToneClass(present) {
  if (present?.tone === 'error') return 'text-danger';
  if (present?.tone === 'warn') return 'text-warning';
  if (present?.tone === 'ok') return 'text-success';
  return 'text-accent';
}

function AddLoopDialog({ loop = null, template = null, models, defaultModelName, workspaces, onRefreshModels, onClose, onSaved }) {
  const [form, setForm] = useState(() => {
    const modelName = defaultModelName || models[0]?.name || '';
    return loop ? formFromLoop(loop) : loopFormForTemplate(template, modelName);
  });
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const update = (key, value) => setForm((prev) => ({ ...prev, [key]: value }));

  useEffect(() => {
    if (!form.modelName && models[0]?.name) update('modelName', models[0].name);
  }, [form.modelName, models]);

  const submit = async () => {
    const invalid = validateLoopForm(form);
    if (invalid) {
      setError(invalid);
      return;
    }
    setSaving(true);
    setError('');
    try {
      const payload = buildLoopPayload(form, workspaces);
      const saved = loop ? await api.updateLoop(loop.id, payload) : await api.createLoop(payload);
      onSaved(saved);
    } catch (e) {
      if (e?.status === 409 && e?.body?.conflict) {
        setError(`该工作空间的执行时间与“${e.body.conflict.loop_name || '已有循环'}”冲突（首次：${formatDate(e.body.conflict.first_conflict_at_ms)}），请调整时间`);
      } else {
        setError(e?.message || '保存循环失败');
      }
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-3" style={{ backgroundColor: 'rgba(0, 0, 0, 0.55)' }} role="dialog" aria-modal="true" aria-label={loop ? '编辑循环' : '添加循环'}>
      <div className="w-full max-w-[620px] max-h-[calc(100vh-24px)] overflow-y-auto rounded-xl border border-border bg-surface shadow-2xl">
        <div className="sticky top-0 z-10 h-12 px-5 flex items-center justify-between border-b border-border bg-surface">
          <h2 className="text-[16px] font-semibold">{loop ? '编辑循环' : '添加循环'}</h2>
          <button type="button" onClick={onClose} className="w-7 h-7 rounded-md hover:bg-surface-hi flex items-center justify-center" aria-label="关闭">
            <VsIcon name="close" size={14} />
          </button>
        </div>

        <div className="px-5 py-4 space-y-3.5">
          <Field label="名称">
            <input className={inputClass()} value={form.name} onChange={(e) => update('name', e.target.value)} autoFocus />
          </Field>

          <Field label="工作空间" hint="（可选；Git 仓库会优先建立独立 worktree）">
            <select className={inputClass()} value={form.workspaceHash} onChange={(e) => update('workspaceHash', e.target.value)}>
              <option value="">不选择工作空间</option>
              {workspaces.map((workspace) => (
                <option key={workspace.hash} value={workspace.hash}>{workspace.name || workspace.cwd}</option>
              ))}
            </select>
          </Field>

          <Field label="提示词">
            <div className="rounded-lg border border-border bg-bg overflow-hidden focus-within:border-accent transition">
              <textarea
                value={form.prompt}
                onChange={(e) => update('prompt', e.target.value)}
                className="w-full min-h-[118px] p-2.5 resize-y bg-transparent text-[12px] leading-5 text-fg outline-none"
              />
              <div className="min-h-9 px-2.5 py-1.5 flex flex-wrap items-center gap-1.5 border-t border-border bg-surface">
                <select
                  aria-label="权限模式"
                  className="h-7 px-2 rounded-md bg-transparent text-[11px] text-fg border border-transparent hover:border-border outline-none"
                  value={form.permissionMode}
                  onChange={(e) => update('permissionMode', e.target.value)}
                >
                  <option value="default">默认</option>
                  <option value="yolo">Yolo</option>
                </select>
                <button type="button" title="刷新模型" onClick={onRefreshModels} className="w-7 h-7 rounded-md hover:bg-surface-hi flex items-center justify-center">
                  <VsIcon name="refresh" size={15} />
                </button>
                <select
                  aria-label="模型"
                  className="h-7 min-w-[200px] max-w-full px-2 rounded-md border border-border bg-surface text-[11px] text-fg outline-none"
                  value={form.modelName}
                  onChange={(e) => update('modelName', e.target.value)}
                >
                  {!models.length && <option value="">暂无可用模型</option>}
                  {models.map((model) => <option key={model.name} value={model.name}>{model.name}</option>)}
                </select>
              </div>
            </div>
          </Field>

          <div className="flex flex-nowrap items-center gap-2.5" role="group" aria-label="执行频率">
            <span className="shrink-0 text-[12px] font-medium text-fg-2">执行频率</span>
            <div className="inline-flex shrink-0 rounded-md border border-border overflow-hidden">
              {[['period', '周期'], ['interval', '按间隔'], ['once', '单次']].map(([value, label]) => (
                <button
                  key={value}
                  type="button"
                  onClick={() => update('scheduleKind', value)}
                  className={`h-7 px-3.5 text-[11px] ${form.scheduleKind === value ? 'bg-surface-hi text-fg font-medium' : 'text-fg-mute hover:text-fg'}`}
                >{label}</button>
              ))}
            </div>
            {form.scheduleKind === 'period' && (
              <>
                <select aria-label="周期" className={inputClass('!w-[88px] shrink-0')} value={form.period} onChange={(e) => update('period', e.target.value)}>
                  <option value="daily">每天</option>
                  <option value="workdays">工作日</option>
                  <option value="weekly">每周</option>
                </select>
                <input type="time" aria-label="执行时间" className={inputClass('!w-[104px] shrink-0')} value={form.time} onChange={(e) => update('time', e.target.value)} />
              </>
            )}
            {form.scheduleKind === 'interval' && (
              <div className="flex shrink-0 items-center gap-2.5">
                <span className="text-[12px] text-fg-mute">每</span>
                <input type="number" min="1" max="10000" aria-label="间隔数值" className={inputClass('!w-[72px]')} value={form.intervalValue} onChange={(e) => update('intervalValue', e.target.value)} />
                <select aria-label="间隔单位" className={inputClass('!w-[72px]')} value={form.intervalUnit} onChange={(e) => update('intervalUnit', e.target.value)}>
                  <option value="minutes">分钟</option><option value="hours">小时</option><option value="days">天</option>
                </select>
              </div>
            )}
            {form.scheduleKind === 'once' && <input type="datetime-local" aria-label="单次执行时间" className={inputClass('!w-[184px] shrink-0')} value={form.onceAt} onChange={(e) => update('onceAt', e.target.value)} />}
          </div>

          {form.scheduleKind === 'period' && form.period === 'weekly' && (
            <div className="flex items-center gap-1.5 pl-[58px]" aria-label="执行星期">
              {WEEKDAYS.map(([day, label]) => {
                const selected = form.weekdays.includes(day);
                return (
                  <button key={day} type="button" onClick={() => update('weekdays', selected ? form.weekdays.filter((item) => item !== day) : [...form.weekdays, day])} className={`w-7 h-7 rounded-md text-[11px] border ${selected ? 'border-accent bg-accent/10 text-accent' : 'border-border text-fg-mute'}`}>{label}</button>
                );
              })}
            </div>
          )}

          <Field label="生效日期区间" hint="（可选，留空表示始终生效）">
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-2">
              <input type="datetime-local" className={inputClass()} value={form.validFrom} onChange={(e) => update('validFrom', e.target.value)} aria-label="开始时间" />
              <input type="datetime-local" className={inputClass()} value={form.validUntil} onChange={(e) => update('validUntil', e.target.value)} aria-label="结束时间" />
            </div>
          </Field>

          {error && <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-[12px] text-danger">{error}</div>}
        </div>

        <div className="sticky bottom-0 h-[52px] px-5 flex items-center gap-3 border-t border-border bg-surface">
          <div className="mr-auto flex min-w-0 items-center gap-2">
            <span className="shrink-0 text-[11px] font-medium text-fg-2">连接器</span>
            <button type="button" disabled title="连接器暂不可用" className="h-8 min-w-[112px] px-2.5 rounded-md border border-dashed border-border text-left text-[11px] text-fg-mute disabled:cursor-not-allowed">暂无连接器</button>
          </div>
          <div className="flex shrink-0 items-center gap-2">
            <button type="button" onClick={onClose} className="h-8 px-4 rounded-md border border-border text-[12px] hover:bg-surface-hi">取消</button>
            <button type="button" onClick={submit} disabled={saving} className="h-8 px-5 rounded-md bg-fg text-bg text-[12px] font-medium disabled:opacity-50">{saving ? '保存中…' : loop ? '保存' : '添加循环'}</button>
          </div>
        </div>
      </div>
    </div>
  );
}

function RunList({ runs, onOpenSession }) {
  if (!runs.length) return <div className="py-4 text-[12px] text-fg-mute">暂无运行记录</div>;
  return (
    <div className="mt-3 border-t border-border divide-y divide-border">
      {runs.map((run) => {
        const present = loopRunPresentation(run);
        return (
          <button key={run.id} type="button" disabled={!run.session_id} onClick={() => onOpenSession(run)} className="w-full py-2 flex items-center gap-3 text-left disabled:cursor-default hover:bg-surface-hi">
            <span className={`text-[11px] min-w-[58px] ${runToneClass(present)}`}>{present.label}</span>
            <span className="text-[12px] text-fg-2">{formatDate(run.scheduled_at_ms)}</span>
            <span className="flex-1 truncate text-[11px] text-fg-mute">{present.reason}</span>
            {run.session_id && <VsIcon name="arrowRight" size={13} />}
          </button>
        );
      })}
    </div>
  );
}

export function LoopPage({ onOpenSession }) {
  const [loops, setLoops] = useState([]);
  const [models, setModels] = useState([]);
  const [defaultModelName, setDefaultModelName] = useState('');
  const [workspaces, setWorkspaces] = useState([]);
  const [runsByLoop, setRunsByLoop] = useState({});
  const [expanded, setExpanded] = useState('');
  const [dialog, setDialog] = useState(null);
  const [loading, setLoading] = useState(true);
  const [unsupported, setUnsupported] = useState(false);
  const pollInFlight = useRef(false);

  const refreshModels = useCallback(async () => {
    try { setModels(normalizeList(await api.listModels(), 'models')); }
    catch (e) { toast({ kind: 'err', text: '刷新模型失败：' + (e?.message || '') }); }
  }, []);
  const load = useCallback(async () => {
    setLoading(true);
    try {
      const [loopData, workspaceData, modelData, defaultModel] = await Promise.all([
        api.listLoops(), api.listWorkspaces(), api.listModels(), api.getDefaultModel().catch(() => ({ name: '' })),
      ]);
      setLoops(normalizeList(loopData, 'loops'));
      setWorkspaces(normalizeList(workspaceData, 'workspaces'));
      setModels(normalizeList(modelData, 'models'));
      setDefaultModelName(defaultModel?.name || '');
      setUnsupported(false);
    } catch (e) {
      if (e?.status === 404 || e?.status === 501 || e?.code === 'LOOP_UNAVAILABLE') setUnsupported(true);
      else toast({ kind: 'err', text: '读取循环失败：' + (e?.message || '') });
    } finally { setLoading(false); }
  }, []);
  useEffect(() => { load(); }, [load]);

  const refreshActivity = useCallback(async () => {
    if (pollInFlight.current) return;
    pollInFlight.current = true;
    try {
      const loopData = await api.listLoops();
      setLoops(normalizeList(loopData, 'loops'));
      if (expanded) {
        const result = await api.listLoopRuns(expanded, 20);
        setRunsByLoop((prev) => ({ ...prev, [expanded]: normalizeList(result, 'runs') }));
      }
      setUnsupported(false);
    } catch (e) {
      if (e?.status === 404 || e?.status === 501 || e?.code === 'LOOP_UNAVAILABLE') setUnsupported(true);
    } finally {
      pollInFlight.current = false;
    }
  }, [expanded]);

  useEffect(() => {
    let stopped = false;
    let timer = null;
    const tick = async () => {
      if (!stopped && document.visibilityState !== 'hidden') await refreshActivity();
      if (!stopped) timer = window.setTimeout(tick, LOOP_POLL_INTERVAL_MS);
    };
    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') refreshActivity();
    };
    timer = window.setTimeout(tick, LOOP_POLL_INTERVAL_MS);
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      stopped = true;
      if (timer) window.clearTimeout(timer);
      document.removeEventListener('visibilitychange', onVisibilityChange);
    };
  }, [refreshActivity]);

  const workspaceByHash = useMemo(() => new Map(workspaces.map((item) => [item.hash, item])), [workspaces]);
  const toggleRuns = async (id) => {
    if (expanded === id) { setExpanded(''); return; }
    setExpanded(id);
    try {
      const result = await api.listLoopRuns(id, 20);
      setRunsByLoop((prev) => ({ ...prev, [id]: normalizeList(result, 'runs') }));
    } catch (e) { toast({ kind: 'err', text: '读取运行记录失败：' + (e?.message || '') }); }
  };
  const toggleEnabled = async (loop) => {
    try {
      const updated = await api.setLoopEnabled(loop.id, !loop.enabled);
      setLoops((prev) => prev.map((item) => item.id === loop.id
        ? { ...updated, latest_run: item.latest_run || null }
        : item));
    } catch (e) {
      const conflict = e?.status === 409 ? '启用失败：该工作空间的执行时间与已有循环冲突' : `操作失败：${e?.message || ''}`;
      toast({ kind: 'err', text: conflict });
    }
  };
  const remove = async (loop) => {
    if (!window.confirm(`删除循环“${loop.name}”？运行记录也会一并删除。`)) return;
    try {
      await api.deleteLoop(loop.id);
      setLoops((prev) => prev.filter((item) => item.id !== loop.id));
    } catch (e) { toast({ kind: 'err', text: '删除失败：' + (e?.message || '') }); }
  };

  if (unsupported) {
    return <div className="flex-1 flex items-center justify-center text-center p-8"><div><VsIcon name="alarm" size={36} className="mx-auto mb-3 text-fg-mute" /><h1 className="text-lg font-semibold">当前 daemon 不支持 LOOP</h1><p className="mt-2 text-[13px] text-fg-mute">请升级并重启 ACECode daemon 后再试。</p></div></div>;
  }

  return (
    <div className="flex-1 overflow-y-auto bg-bg">
      <div className="max-w-[1040px] mx-auto px-6 py-6">
        <div className="flex items-start justify-between gap-4">
          <div><h1 className="text-[20px] font-semibold tracking-tight">循环</h1><p className="mt-0.5 text-[12px] text-fg-mute">管理计划执行的代码任务并查看最近运行记录。</p></div>
          <button type="button" onClick={() => setDialog({})} className="h-8 px-3 rounded-md border border-border bg-surface hover:bg-surface-hi text-[12px] flex items-center gap-1.5"><VsIcon name="add" size={13} />添加</button>
        </div>

        <section className="mt-6">
          <h2 className="text-[13px] font-medium text-fg-2 mb-2.5">从模板入手</h2>
          <div className="grid grid-cols-1 md:grid-cols-2 gap-2">
            {LOOP_TEMPLATES.map((template) => (
              <button key={template.id} type="button" onClick={() => setDialog({ template })} className="min-h-[68px] p-3 rounded-lg border border-border bg-surface hover:bg-surface-hi hover:border-fg-mute transition text-left flex gap-2.5">
                <span className="w-8 h-8 rounded-md bg-accent-bg text-accent flex items-center justify-center shrink-0"><VsIcon name="alarm" size={16} /></span>
                <span><strong className="block text-[13px] font-semibold">{template.name}</strong><span className="block mt-0.5 text-[11px] leading-4 text-fg-mute">{template.description}</span></span>
              </button>
            ))}
          </div>
        </section>

        <section className="mt-7">
          <div className="mb-2.5 flex items-center justify-between gap-3">
            <h2 className="text-[13px] font-medium text-fg-2">我的循环</h2>
            <span className="text-[10px] text-fg-mute">自动刷新运行状态</span>
          </div>
          {loading ? <div className="py-6 text-[12px] text-fg-mute">加载中…</div> : !loops.length ? <div className="py-6 rounded-lg border border-dashed border-border text-center text-[12px] text-fg-mute">还没有循环，选择模板或点击“添加”开始。</div> : (
            <div className="space-y-2">
              {loops.map((loop) => {
                const workspace = workspaceByHash.get(loop.workspace_hash);
                const latestRun = loop.latest_run || null;
                const latestPresent = latestRun ? loopRunPresentation(latestRun) : null;
                const active = latestRun?.status === 'running' || latestRun?.status === 'waiting_user';
                return (
                  <article key={loop.id} className="rounded-lg border border-border bg-surface px-3.5 py-2.5">
                    <div className="flex items-center gap-2.5">
                      <span className={`w-2 h-2 rounded-full ${active ? 'bg-accent animate-pulse' : loop.enabled ? 'bg-success' : 'bg-fg-mute'}`} />
                      <div className="min-w-0 flex-1">
                        <h3 className="text-[13px] font-semibold truncate">{loop.name}</h3>
                        <p className="mt-0.5 text-[11px] text-fg-mute truncate">{loopScheduleLabel(loop.schedule)} · {workspace?.name || '无工作空间'} · {loop.permission_mode === 'default' ? '默认' : 'Yolo'} · {loop.model_name}</p>
                        <p className="mt-0.5 text-[11px] text-fg-mute flex flex-wrap items-center gap-x-1" aria-live="polite">
                          <span>最近运行：</span>
                          {latestPresent ? <span className={runToneClass(latestPresent)}>{latestPresent.label}</span> : <span>—</span>}
                          {latestRun && <span>· {formatDate(latestRun.started_at_ms || latestRun.scheduled_at_ms)}</span>}
                          <span>· 下次运行：{loop.enabled && loop.next_run_at_ms ? formatDate(loop.next_run_at_ms) : '—'}</span>
                        </p>
                      </div>
                      <button type="button" onClick={() => toggleEnabled(loop)} className="h-7 px-2.5 rounded-md text-[11px] border border-border hover:bg-surface-hi">{loop.enabled ? '停用' : '启用'}</button>
                      <button type="button" onClick={() => setDialog({ loop })} className="w-7 h-7 rounded-md hover:bg-surface-hi flex items-center justify-center" title="编辑"><VsIcon name="edit" size={14} /></button>
                      <button type="button" onClick={() => remove(loop)} className="w-7 h-7 rounded-md hover:bg-surface-hi hover:text-danger flex items-center justify-center" title="删除"><VsIcon name="delete" size={14} /></button>
                      <button type="button" onClick={() => toggleRuns(loop.id)} className="w-7 h-7 rounded-md hover:bg-surface-hi flex items-center justify-center" title="运行记录"><VsIcon name={expanded === loop.id ? 'expandUp' : 'expandDown'} size={14} /></button>
                    </div>
                    {expanded === loop.id && (
                      <RunList
                        runs={runsByLoop[loop.id] || []}
                        onOpenSession={(run) => onOpenSession({
                          ...run,
                          workspace_hash: loop.workspace_hash,
                          workspace_cwd: loop.workspace_cwd,
                        })}
                      />
                    )}
                  </article>
                );
              })}
            </div>
          )}
        </section>
      </div>

      {dialog && (
        <AddLoopDialog
          loop={dialog.loop}
          template={dialog.template}
          models={models}
          defaultModelName={defaultModelName}
          workspaces={workspaces}
          onRefreshModels={refreshModels}
          onClose={() => setDialog(null)}
          onSaved={(saved) => {
            setLoops((prev) => dialog.loop
              ? prev.map((item) => item.id === saved.id ? { ...saved, latest_run: item.latest_run || null } : item)
              : [{ ...saved, latest_run: null }, ...prev]);
            setDialog(null);
          }}
        />
      )}
    </div>
  );
}
