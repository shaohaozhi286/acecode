// 右侧滑出 Skills 面板:搜索 + 列表(toggle 启停 + 查看正文)
//
// 启停 → PUT /api/skills/:name {enabled};查看 → GET body 弹独立窗口(简单方案)。

import { useEffect, useMemo, useState } from 'react';
import { api } from '../lib/api.js';
import { SlideOver, Toggle } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { clsx } from '../lib/format.js';

export function SkillsPanel({ onClose }) {
  const [skills, setSkills] = useState([]);
  const [search, setSearch] = useState('');
  const [busy,   setBusy]   = useState(true);

  useEffect(() => {
    let off = false;
    api.listSkills()
      .then((list) => { if (!off) setSkills(Array.isArray(list) ? list : []); })
      .catch((e) => toast({ kind: 'err', text: '加载 Skills 失败:' + (e.message || '') }))
      .finally(() => { if (!off) setBusy(false); });
    return () => { off = true; };
  }, []);

  const filtered = useMemo(() => {
    if (!search.trim()) return skills;
    const q = search.trim().toLowerCase();
    return skills.filter((s) =>
      (s.name || '').toLowerCase().includes(q) ||
      (s.description || '').toLowerCase().includes(q));
  }, [skills, search]);

  const toggle = async (name, next) => {
    const orig = skills;
    setSkills((prev) => prev.map((s) => s.name === name ? { ...s, enabled: next } : s));
    try { await api.setSkillEnabled(name, next); }
    catch (e) {
      setSkills(orig);
      toast({ kind: 'err', text: '切换失败:' + (e.message || '') });
    }
  };

  const view = async (name) => {
    try {
      const body = await api.getSkillBody(name);
      const w = window.open('', '_blank', 'width=720,height=600');
      if (w) {
        w.document.title = name;
        w.document.body.style.cssText = 'font-family:monospace;padding:20px;white-space:pre-wrap;line-height:1.5;';
        w.document.body.textContent = body;
      }
    } catch (e) {
      toast({ kind: 'err', text: '查看失败:' + (e.message || '') });
    }
  };

  return (
    <SlideOver width={400} onClose={onClose}>
      {({ close }) => (
        <>
          <div className="h-11 px-4 flex items-center bg-surface-alt border-b border-border shrink-0">
            <span className="flex-1 text-[14px] font-semibold">🧩 Skills</span>
            <button
              type="button"
              onClick={close}
              className="w-7 h-7 rounded text-fg-mute hover:text-fg hover:bg-surface-hi text-[16px] transition"
            >✕</button>
          </div>
          <input
            type="text"
            placeholder="搜索 Skills..."
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            className="m-3 px-3 h-8 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
          />
          <div className="flex-1 overflow-y-auto px-3 pb-3 flex flex-col gap-1.5">
            {busy && <div className="text-fg-mute text-[12px] py-4 text-center">加载中…</div>}
            {!busy && filtered.length === 0 && (
              <div className="text-fg-mute text-[12px] py-6 text-center">{search ? '无匹配' : '暂无 Skill'}</div>
            )}
            {filtered.map((s) => (
              <div
                key={s.name}
                className={clsx(
                  'flex items-center gap-2.5 px-3.5 py-2.5 rounded-lg border transition',
                  s.enabled
                    ? 'bg-ok-bg border-ok-border'
                    : 'bg-surface border-border',
                )}
              >
                <div className="flex-1 min-w-0">
                  <div className="text-[13px] font-semibold text-fg truncate">{s.name}</div>
                  <div className="text-[11px] text-fg-mute mt-0.5 truncate">{s.description || '—'}</div>
                </div>
                <button
                  type="button"
                  onClick={() => view(s.name)}
                  className="text-[11px] text-accent hover:underline"
                >
                  查看
                </button>
                <Toggle on={!!s.enabled} onChange={(v) => toggle(s.name, v)} />
              </div>
            ))}
          </div>
        </>
      )}
    </SlideOver>
  );
}
