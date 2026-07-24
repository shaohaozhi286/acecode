import { useCallback, useEffect, useMemo, useState } from 'react';
import { api } from '../lib/api.js';
import {
  EXPERT_FILTERS,
  emptyExpertForm,
  expertFormFromDetail,
  expertPayloadFromForm,
  filterExperts,
  normalizeExperts,
  selectedTeamExperts,
  singleExpertsForTeam,
  toggleTeamExpert,
  validateExpertForm,
} from '../lib/expertComponents.js';
import { clsx } from '../lib/format.js';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

const inputClass = 'w-full h-8 rounded-md border border-border bg-surface-alt px-2.5 text-[12px] text-fg outline-none focus:border-accent transition';
const textareaClass = 'w-full min-h-[104px] resize-y rounded-md border border-border bg-surface-alt px-2.5 py-2 text-[12px] leading-5 text-fg outline-none focus:border-accent transition';

function Field({ label, hint = '', children }) {
  return (
    <label className="block space-y-1">
      <span className="text-[12px] font-medium text-fg-2">{label}</span>
      {hint && <span className="ml-1 text-[11px] text-fg-mute">{hint}</span>}
      {children}
    </label>
  );
}

function TeamSelectionSummary({ form, experts, onChoose }) {
  const selected = selectedTeamExperts(form, experts);
  return (
    <div className="border-t border-border pt-4">
      <div className="mb-3 flex items-start justify-between gap-4">
        <div>
          <div className="text-[14px] font-semibold text-fg">团队成员</div>
          <p className="mt-1 text-[12px] text-fg-mute">从已有专家中选择，并指定一位主理人。</p>
        </div>
        <button
          type="button"
          onClick={onChoose}
          className="h-8 shrink-0 rounded-md border border-border bg-surface px-3 text-[12px] text-fg-2 hover:bg-surface-hi transition"
        >
          选择专家
        </button>
      </div>
      {selected.length > 0 ? (
        <div className="space-y-2">
          {selected.map((expert) => (
            <div
              key={expert.id}
              className="flex items-center gap-3 rounded-md border border-border bg-surface px-3.5 py-2.5"
            >
              <div className="flex h-8 w-8 shrink-0 items-center justify-center rounded-md border border-border bg-bg text-accent">
                <VsIcon name="brain" size={16} />
              </div>
              <div className="min-w-0 flex-1">
                <div className="truncate text-[13px] font-medium text-fg">{expert.display_name}</div>
                <div className="mt-0.5 truncate text-[11px] text-fg-mute">{expert.profession || '专家'}</div>
              </div>
              {form.leadExpertId === expert.id && (
                <span className="rounded bg-accent-bg px-2 py-0.5 text-[11px] text-accent">主理人</span>
              )}
            </div>
          ))}
        </div>
      ) : (
        <button
          type="button"
          onClick={onChoose}
          className="flex h-20 w-full items-center justify-center rounded-md border border-dashed border-border bg-surface text-[12px] text-fg-mute hover:bg-surface-hi transition"
        >
          选择要一起工作的专家
        </button>
      )}
    </div>
  );
}

function ExpertEditor({
  initial,
  editing,
  experts,
  workspaceHash,
  onClose,
  onChooseExperts,
  onSaved,
}) {
  const [form, setForm] = useState(initial || emptyExpertForm());
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const update = (key, value) => setForm((current) => ({ ...current, [key]: value }));
  const isTeam = form.type === 'team';

  const save = async () => {
    const validation = validateExpertForm(form);
    if (validation) {
      setError(validation);
      return;
    }
    setSaving(true);
    setError('');
    try {
      const payload = expertPayloadFromForm(form);
      const result = editing
        ? await api.updateExpert(form.id, payload, workspaceHash)
        : await api.createExpert(payload, workspaceHash);
      onSaved(result);
    } catch (saveError) {
      setError(saveError?.message || `保存${isTeam ? '专家团' : '专家'}失败`);
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal onClose={onClose} width={680} dismissOnBackdrop={!saving}>
      <div data-expert-editor="true" data-expert-editor-kind={form.type} className="flex max-h-[82vh] flex-col">
        <div className="flex items-center justify-between border-b border-border px-5 py-3.5">
          <div>
            <h2 className="text-[15px] font-semibold text-fg">
              {editing ? `编辑${isTeam ? '专家团' : '专家'}` : isTeam ? '组建专家团' : '新建专家'}
            </h2>
            <p className="mt-0.5 text-[11px] text-fg-mute">
              {isTeam ? '让几位已有专家一起完成复杂任务。' : '设置这位专家擅长什么、怎样工作。'}
            </p>
          </div>
          <button type="button" onClick={onClose} className="p-1 text-fg-mute hover:text-fg" aria-label="关闭">
            <VsIcon name="close" size={16} />
          </button>
        </div>
        <div className="flex-1 space-y-4 overflow-y-auto px-5 py-4">
          <div className="grid grid-cols-2 gap-3">
            <Field label={isTeam ? '团队名称' : '姓名或称呼'}>
              <input
                className={inputClass}
                value={form.displayName}
                onChange={(event) => update('displayName', event.target.value)}
                placeholder={isTeam ? '产品交付专家团' : '代码审查专家'}
              />
            </Field>
            <Field label={isTeam ? '擅长解决' : '擅长方向'}>
              <input
                className={inputClass}
                value={form.profession}
                onChange={(event) => update('profession', event.target.value)}
                placeholder={isTeam ? '从需求到交付' : '代码质量与安全'}
              />
            </Field>
          </div>
          <Field label="介绍">
            <input
              className={inputClass}
              value={form.description}
              onChange={(event) => update('description', event.target.value)}
              placeholder={isTeam ? '这个团队适合一起解决什么问题' : '告诉大家什么时候适合找这位专家'}
            />
          </Field>

          {!isTeam && (
            <Field label="工作方式" hint="描述它应该如何思考、处理问题和给出结果">
              <textarea
                className={textareaClass}
                value={form.instructions}
                onChange={(event) => update('instructions', event.target.value)}
                placeholder={'例如：先理解目标，再检查现有实现；发现问题时说明影响，并给出可以直接执行的建议。'}
              />
            </Field>
          )}
          <Field label="常用开场白" hint="每行一条">
            <textarea
              className={`${textareaClass} min-h-[72px]`}
              value={form.quickPromptsText}
              onChange={(event) => update('quickPromptsText', event.target.value)}
              placeholder={isTeam ? '帮我把这个需求推进到交付' : '帮我审查当前改动'}
            />
          </Field>

          {isTeam && (
            <TeamSelectionSummary
              form={form}
              experts={experts}
              onChoose={() => onChooseExperts(form)}
            />
          )}
          {error && (
            <div className="rounded-md border border-danger/40 bg-danger-bg px-3 py-2 text-[12px] text-danger">
              {error}
            </div>
          )}
        </div>
        <div className="flex justify-end gap-2 border-t border-border px-5 py-3">
          <button
            type="button"
            onClick={onClose}
            disabled={saving}
            className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi disabled:opacity-50"
          >
            取消
          </button>
          <button
            type="button"
            onClick={save}
            disabled={saving}
            className="h-8 rounded-md bg-accent px-3 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-50"
          >
            {saving ? '保存中…' : isTeam ? '保存专家团' : '保存专家'}
          </button>
        </div>
      </div>
    </Modal>
  );
}

function TeamMemberNames({ expert }) {
  if (expert.type !== 'team' || !Array.isArray(expert.agents) || expert.agents.length === 0) {
    return null;
  }
  const lead = expert.agents.find((agent) => agent.id === expert.lead_expert_id)
    || expert.agents[0];
  const collaborators = expert.agents.filter((agent) => agent.id !== lead?.id);
  return (
    <div className="mt-3 space-y-1 text-[11px] text-fg-mute">
      {lead && <div className="truncate">主理人：{lead.display_name}</div>}
      {collaborators.length > 0 && (
        <div className="truncate">协作：{collaborators.map((agent) => agent.display_name).join('、')}</div>
      )}
    </div>
  );
}

function ExpertCard({
  expert,
  onUse,
  onEdit,
  onDelete,
  onStartTeam,
}) {
  return (
    <article data-expert-card={expert.id} className="group flex min-h-[220px] flex-col rounded-xl border border-border bg-surface p-4 hover:border-border-hi hover:bg-surface-hi transition">
      <div className="flex items-start gap-3">
        <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg border border-border bg-bg text-accent">
          <VsIcon name={expert.type === 'team' ? 'extension' : 'brain'} size={20} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <h3 className="truncate text-[14px] font-semibold text-fg">{expert.display_name}</h3>
            <span className="shrink-0 rounded border border-border px-1.5 py-0.5 text-[10px] text-fg-mute">
              {expert.type === 'team' ? '专家团' : '专家'}
            </span>
          </div>
          <p className="mt-0.5 truncate text-[11px] text-fg-mute">{expert.profession || '随时可以开始'}</p>
        </div>
        <span className={clsx(
          'h-5 rounded px-1.5 text-[10px] leading-5',
          expert.source === 'workspace' ? 'bg-accent-bg text-accent' : 'bg-bg text-fg-mute',
        )}>
          {expert.source === 'workspace' ? '项目提供' : '我的'}
        </span>
      </div>
      <p className="mt-3 line-clamp-3 flex-1 text-[12px] leading-5 text-fg-2">
        {expert.description || '还没有填写介绍。'}
      </p>
      <TeamMemberNames expert={expert} />
      {expert.quick_prompts.length > 0 && expert.type !== 'team' && (
        <div className="mt-3 flex flex-wrap gap-1.5">
          {expert.quick_prompts.slice(0, 2).map((prompt) => (
            <span key={prompt} className="max-w-full truncate rounded-md bg-bg px-2 py-1 text-[10px] text-fg-mute">
              {prompt}
            </span>
          ))}
        </div>
      )}
      <div className="mt-4 flex items-center justify-end gap-1.5 border-t border-border pt-3">
        {expert.type === 'agent' && (
          <button
            type="button"
            onClick={() => onStartTeam(expert)}
            className="h-7 rounded-md px-2 text-[11px] text-fg-mute hover:bg-bg hover:text-fg"
          >
            和其他专家组团
          </button>
        )}
        {expert.managed_global && (
          <>
            <button type="button" onClick={() => onEdit(expert)} className="h-7 rounded-md px-2 text-[11px] text-fg-mute hover:bg-bg hover:text-fg">编辑</button>
            <button type="button" onClick={() => onDelete(expert)} className="h-7 rounded-md px-2 text-[11px] text-fg-mute hover:bg-danger-bg hover:text-danger">删除</button>
          </>
        )}
        <button type="button" onClick={() => onUse(expert)} className="h-7 rounded-md bg-accent px-3 text-[11px] font-medium text-white hover:opacity-90">使用</button>
      </div>
    </article>
  );
}

function ExpertPickerCard({ expert, selected, lead, onToggle, onLead }) {
  return (
    <article
      data-team-expert-option={expert.id}
      className={clsx(
        'rounded-xl border p-4 transition',
        selected ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
      )}
    >
      <button type="button" onClick={onToggle} className="flex w-full items-start gap-3 text-left">
        <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg border border-border bg-bg text-accent">
          <VsIcon name="brain" size={20} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <h3 className="truncate text-[14px] font-semibold text-fg">{expert.display_name}</h3>
            {selected && <VsIcon name="ok" size={12} mono={false} className="shrink-0 text-accent" />}
          </div>
          <p className="mt-0.5 truncate text-[11px] text-fg-mute">{expert.profession || '专家'}</p>
          <p className="mt-2 line-clamp-2 text-[12px] leading-5 text-fg-2">{expert.description || '还没有填写介绍。'}</p>
        </div>
      </button>
      {selected && (
        <div className="mt-3 flex justify-end border-t border-border pt-3">
          <button
            type="button"
            onClick={onLead}
            disabled={lead}
            className={clsx(
              'h-7 rounded-md px-2.5 text-[11px] transition',
              lead ? 'bg-accent text-white' : 'border border-border bg-surface text-fg-2 hover:bg-surface-hi',
            )}
          >
            {lead ? '主理人' : '设为主理人'}
          </button>
        </div>
      )}
    </article>
  );
}

function TeamExpertPicker({ form, experts, onChange, onReturn }) {
  const candidates = useMemo(
    () => singleExpertsForTeam(experts, form.id),
    [experts, form.id],
  );
  return (
    <main data-team-expert-picker="true" className="flex-1 min-w-0 overflow-y-auto bg-bg">
      <div className="mx-auto max-w-[1180px] px-7 py-7">
        <header className="flex items-start justify-between gap-5">
          <div>
            <button
              type="button"
              onClick={onReturn}
              className="mb-3 inline-flex h-7 items-center gap-1 rounded-md text-[12px] text-fg-mute hover:text-fg"
            >
              <VsIcon name="back" size={13} />
              返回专家团
            </button>
            <h1 className="text-[20px] font-semibold text-fg">选择一起工作的专家</h1>
            <p className="mt-1 text-[12px] text-fg-mute">点选专家，再指定一位主理人。每位专家仍保留自己的工作方式。</p>
          </div>
          <button
            type="button"
            onClick={onReturn}
            className="h-8 rounded-md bg-accent px-4 text-[12px] font-medium text-white hover:opacity-90"
          >
            选好了
          </button>
        </header>

        {candidates.length > 0 ? (
          <div className="mt-6 grid grid-cols-[repeat(auto-fill,minmax(300px,1fr))] gap-4">
            {candidates.map((expert) => {
              const selected = form.selectedExpertIds.includes(expert.id);
              const lead = form.leadExpertId === expert.id;
              return (
                <ExpertPickerCard
                  key={expert.id}
                  expert={expert}
                  selected={selected}
                  lead={lead}
                  onToggle={() => onChange(toggleTeamExpert(form, expert.id))}
                  onLead={() => onChange({ ...form, leadExpertId: expert.id })}
                />
              );
            })}
          </div>
        ) : (
          <div className="mt-6 flex min-h-[260px] flex-col items-center justify-center rounded-xl border border-dashed border-border bg-surface text-center">
            <VsIcon name="brain" size={28} className="text-fg-mute" />
            <p className="mt-3 text-[13px] font-medium text-fg-2">还没有可以加入团队的专家</p>
            <p className="mt-1 text-[11px] text-fg-mute">先返回并新建专家，再来组建专家团。</p>
          </div>
        )}
      </div>
    </main>
  );
}

export function ExpertComponentsPage({ workspaceHash = '', onUseExpert }) {
  const [experts, setExperts] = useState([]);
  const [diagnostics, setDiagnostics] = useState([]);
  const [loading, setLoading] = useState(true);
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState('all');
  const [editor, setEditor] = useState(null);
  const [picker, setPicker] = useState(null);
  const [deleteTarget, setDeleteTarget] = useState(null);
  const [deleting, setDeleting] = useState(false);

  const effectiveWorkspace = workspaceHash || '__local__';
  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const result = await api.listExperts(effectiveWorkspace);
      setExperts(normalizeExperts(result));
      setDiagnostics(Array.isArray(result?.diagnostics) ? result.diagnostics : []);
    } catch (error) {
      toast({ kind: 'err', text: `加载专家组件失败：${error?.message || ''}` });
    } finally {
      setLoading(false);
    }
  }, [effectiveWorkspace]);

  useEffect(() => { refresh(); }, [refresh]);
  const visible = useMemo(
    () => filterExperts(experts, { query, type: filter }),
    [experts, filter, query],
  );

  const editExpert = async (expert) => {
    try {
      const detail = await api.getExpert(expert.id, effectiveWorkspace);
      setEditor({ editing: true, form: expertFormFromDetail(detail) });
    } catch (error) {
      toast({ kind: 'err', text: `读取专家组件失败：${error?.message || ''}` });
    }
  };

  const startTeam = (initialExpert = null) => {
    setEditor({
      editing: false,
      form: emptyExpertForm('team', initialExpert?.id || ''),
    });
  };

  const confirmDelete = async () => {
    if (!deleteTarget || deleting) return;
    setDeleting(true);
    try {
      await api.deleteExpert(deleteTarget.id, effectiveWorkspace);
      setDeleteTarget(null);
      await refresh();
      toast({ kind: 'ok', text: `已删除“${deleteTarget.display_name}”` });
    } catch (error) {
      toast({ kind: 'err', text: `删除失败：${error?.message || ''}` });
    } finally {
      setDeleting(false);
    }
  };

  if (picker) {
    return (
      <TeamExpertPicker
        form={picker.form}
        experts={experts}
        onChange={(form) => setPicker((current) => ({ ...current, form }))}
        onReturn={() => {
          setEditor({ editing: picker.editing, form: picker.form });
          setPicker(null);
        }}
      />
    );
  }

  return (
    <main data-expert-components-page="true" className="flex-1 min-w-0 overflow-y-auto bg-bg">
      <div className="mx-auto max-w-[1180px] px-7 py-7">
        <header className="flex items-start justify-between gap-5">
          <div>
            <h1 className="text-[20px] font-semibold text-fg">专家组件</h1>
            <p className="mt-1 text-[12px] text-fg-mute">创建可以反复使用的专家，也可以让几位专家组成一个团队。</p>
          </div>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={() => startTeam()}
              className="flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 hover:bg-surface-hi transition"
            >
              <VsIcon name="extension" size={14} />
              组建专家团
            </button>
            <button
              type="button"
              onClick={() => setEditor({ editing: false, form: emptyExpertForm('agent') })}
              className="flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white hover:opacity-90"
            >
              <VsIcon name="add" size={14} />
              新建专家
            </button>
          </div>
        </header>

        <div className="mt-6 flex items-center gap-3 border-b border-border pb-4">
          <div className="relative w-[280px]">
            <VsIcon name="search" size={14} className="pointer-events-none absolute left-2.5 top-1/2 -translate-y-1/2 text-fg-mute" />
            <input
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              className="h-8 w-full rounded-md border border-border bg-surface pl-8 pr-3 text-[12px] text-fg outline-none focus:border-accent transition"
              placeholder="搜索名称、方向或介绍"
            />
          </div>
          <div className="flex items-center rounded-md border border-border bg-surface p-0.5">
            {EXPERT_FILTERS.map((item) => (
              <button
                key={item.id}
                type="button"
                onClick={() => setFilter(item.id)}
                className={clsx(
                  'h-7 rounded px-3 text-[11px]',
                  filter === item.id ? 'bg-surface-hi text-fg shadow-sm' : 'text-fg-mute hover:text-fg',
                )}
              >
                {item.label}
              </button>
            ))}
          </div>
        </div>

        {diagnostics.length > 0 && (
          <div className="mt-4 rounded-lg border border-warn/50 bg-surface px-3 py-2 text-[11px] text-warn">
            有些专家暂时不可用，请稍后重试或重新选择团队成员。
          </div>
        )}
        {loading ? (
          <div className="flex h-48 items-center justify-center text-[12px] text-fg-mute">
            <span className="ace-spinner mr-2 h-4 w-4" />
            正在加载专家组件…
          </div>
        ) : visible.length > 0 ? (
          <div className="mt-5 grid grid-cols-[repeat(auto-fill,minmax(300px,1fr))] gap-4">
            {visible.map((expert) => (
              <ExpertCard
                key={expert.id}
                expert={expert}
                onUse={onUseExpert}
                onEdit={editExpert}
                onDelete={setDeleteTarget}
                onStartTeam={startTeam}
              />
            ))}
          </div>
        ) : (
          <div className="mt-5 flex min-h-[260px] flex-col items-center justify-center rounded-xl border border-dashed border-border bg-surface text-center">
            <VsIcon name="brain" size={28} className="text-fg-mute" />
            <p className="mt-3 text-[13px] font-medium text-fg-2">
              {query || filter !== 'all' ? '没有符合条件的专家组件' : '还没有专家'}
            </p>
            <p className="mt-1 text-[11px] text-fg-mute">先新建一位专家，之后还可以邀请它加入专家团。</p>
          </div>
        )}
      </div>

      {editor && (
        <ExpertEditor
          initial={editor.form}
          editing={editor.editing}
          experts={experts}
          workspaceHash={effectiveWorkspace}
          onClose={() => setEditor(null)}
          onChooseExperts={(form) => {
            setPicker({ editing: editor.editing, form });
            setEditor(null);
          }}
          onSaved={async () => {
            setEditor(null);
            await refresh();
            toast({ kind: 'ok', text: editor.form.type === 'team' ? '专家团已保存' : '专家已保存' });
          }}
        />
      )}
      {deleteTarget && (
        <Modal onClose={() => !deleting && setDeleteTarget(null)} width={420} dismissOnBackdrop={!deleting}>
          <div data-expert-delete-dialog="true" className="p-5">
            <h2 className="text-[15px] font-semibold text-fg">删除{deleteTarget.type === 'team' ? '专家团' : '专家'}</h2>
            <p className="mt-2 text-[12px] leading-5 text-fg-2">
              确定删除“{deleteTarget.display_name}”吗？以前的对话仍会保留。
            </p>
            <div className="mt-5 flex justify-end gap-2">
              <button type="button" disabled={deleting} onClick={() => setDeleteTarget(null)} className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi">取消</button>
              <button type="button" disabled={deleting} onClick={confirmDelete} className="h-8 rounded-md bg-danger px-3 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-50">{deleting ? '删除中…' : '删除'}</button>
            </div>
          </div>
        </Modal>
      )}
    </main>
  );
}
