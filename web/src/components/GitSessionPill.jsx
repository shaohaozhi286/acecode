// 新会话的分支 + worktree pill(openspec add-webui-git-session-pill)。
//
// 两个挂载点(variant):
//   hero — 首页欢迎视图,「项目」pill 右侧
//   bar  — 聊天布局 InputBar 上方(会话尚无消息时可交互,开始后只读)
//
// 交互语义(上游对话拍板):
//   - 未勾 worktree 时选非当前分支 = 立即 POST /api/git/checkout;409 dirty
//     → 弹 stash 确认框(列改动文件),确认带 stash:true 重发;409 busy →
//     提示稍后再试。
//   - 勾了 worktree 后分支下拉的含义变为「worktree 基线」,不动主仓;真正
//     创建发生在首条消息发送时(父组件经 onIntentChange 拿到意图)。
//   - 会话开始后退化为只读徽标。
// 纯状态逻辑在 lib/gitSessionPill.js(Node 单测),这里只做 DOM 映射。

import { useCallback, useEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  buildPillModel,
  classifyCheckoutError,
  GIT_STATE_CHANGED_EVENT,
} from '../lib/gitSessionPill.js';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';

export function GitSessionPill({
  api,
  cwd,
  variant = 'bar',
  sessionStarted = false,
  worktreeSession = null,
  busy = false,
  onIntentChange = null,
}) {
  const [gitInfo, setGitInfo] = useState(null);
  const [open, setOpen] = useState(false);
  const [worktreeChecked, setWorktreeChecked] = useState(false);
  const [selectedBase, setSelectedBase] = useState('');
  const [checkingOut, setCheckingOut] = useState(false);
  const [stashConfirm, setStashConfirm] = useState(null); // {branch, files}
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;

  const refreshInfo = useCallback(() => {
    const target = cwdRef.current;
    if (!target) { setGitInfo(null); return; }
    api.gitInfo(target)
      .then((info) => { if (cwdRef.current === target) setGitInfo(info); })
      .catch(() => { if (cwdRef.current === target) setGitInfo(null); });
  }, [api]);

  // cwd 变化(切 workspace)重拉;非仓库时 pill 整体不渲染(零占位)。
  useEffect(() => {
    setGitInfo(null);
    setWorktreeChecked(false);
    setSelectedBase('');
    refreshInfo();
  }, [cwd, refreshInfo]);

  // 把待生效意图同步给父组件(首条消息发送时读取)。
  useEffect(() => {
    onIntentChange?.({
      worktreeChecked,
      selectedBase: selectedBase || gitInfo?.branch || '',
    });
  }, [worktreeChecked, selectedBase, gitInfo?.branch, onIntentChange]);

  const model = buildPillModel({
    gitInfo, worktreeChecked, sessionStarted, worktreeSession, busy, checkingOut,
  });
  if (!model.visible) return null;

  const doCheckout = (branch, stash) => {
    setCheckingOut(true);
    api.gitCheckout(cwdRef.current, branch, stash)
      .then(() => {
        toast({ kind: 'ok', text: `已切换到分支 ${branch}` });
        refreshInfo();
        // 广播给变更面板等消费方失效 git 缓存。
        window.dispatchEvent(new CustomEvent(GIT_STATE_CHANGED_EVENT, {
          detail: { cwd: cwdRef.current },
        }));
      })
      .catch((e) => {
        const action = classifyCheckoutError(e.status, e.body);
        if (action.kind === 'dirty') {
          setStashConfirm({ branch, files: action.files });
        } else if (action.kind === 'busy') {
          toast({ kind: 'err', text: '有会话正在运行,暂时不能切换分支' });
        } else {
          toast({ kind: 'err', text: '切换分支失败:' + action.message });
        }
      })
      .finally(() => setCheckingOut(false));
  };

  const pickBranch = (branch) => {
    setOpen(false);
    if (worktreeChecked) {
      // worktree 基线选择:纯前端状态,不动主仓。
      setSelectedBase(branch);
      return;
    }
    if (branch === model.branch) return;
    doCheckout(branch, false);
  };

  const displayBranch = worktreeChecked && selectedBase ? selectedBase : model.branch;

  return (
    <div className={clsx(
      'relative flex items-center',
      variant === 'hero' ? '' : 'px-3 pb-1',
    )}>
      <div
        className={clsx(
          'ace-git-pill group inline-flex items-center gap-0 rounded-lg border border-border bg-surface text-[12px] overflow-hidden',
          !model.interactive && 'opacity-80',
        )}
        data-variant={variant}
      >
        <button
          type="button"
          className={clsx(
            'inline-flex items-center gap-1.5 px-2.5 py-1 transition-colors',
            model.interactive ? 'hover:bg-surface-hi cursor-pointer' : 'cursor-default',
          )}
          disabled={!model.interactive}
          onClick={() => {
            if (!model.interactive) return;
            // 打开下拉时重拉 info:外部新建/切换的分支即时可见。
            if (!open) refreshInfo();
            setOpen(!open);
          }}
          title={worktreeChecked
            ? 'worktree 基线分支(发送首条消息时创建 worktree)'
            : model.started ? '会话进行中,分支不可在此切换' : '切换分支(git checkout)'}
        >
          <VsIcon name="fork" size={13} className="opacity-70" />
          <span className="font-medium truncate max-w-[160px]">{displayBranch}</span>
          {model.checkingOut
            ? <span className="ace-spinner w-3 h-3" />
            : model.interactive && (
              <VsIcon name="expandDown" size={12} className="opacity-50 group-hover:opacity-100 transition-opacity" />
            )}
        </button>
        <div className="w-px self-stretch bg-border" aria-hidden="true" />
        <label
          className={clsx(
            'inline-flex items-center gap-1.5 px-2.5 py-1 select-none transition-colors',
            model.interactive ? 'hover:bg-surface-hi cursor-pointer' : 'cursor-default',
          )}
          title={model.worktreeBadge
            ? `会话运行在 worktree:${model.worktreeBadge}`
            : '在独立的 git worktree 中运行本会话(发送首条消息时创建)'}
        >
          <input
            type="checkbox"
            className="ace-git-pill-checkbox"
            checked={model.worktreeChecked}
            disabled={!model.interactive}
            onChange={(e) => setWorktreeChecked(e.target.checked)}
          />
          <span className={clsx('text-fg-mute', model.worktreeChecked && 'text-fg')}>
            {model.worktreeBadge ? `worktree:${model.worktreeBadge}` : 'worktree'}
          </span>
        </label>
      </div>

      {open && model.interactive && (
        <>
          <div className="fixed inset-0 z-40" onClick={() => setOpen(false)} />
          <div className="absolute bottom-full left-0 mb-1.5 min-w-[200px] max-w-[300px] max-h-[40vh] overflow-y-auto bg-surface border border-border ace-shadow rounded-xl z-50 py-1.5 ace-scrollbar"
               data-placement={variant === 'hero' ? 'below' : 'above'}
               style={variant === 'hero' ? { bottom: 'auto', top: '100%', marginBottom: 0, marginTop: 6 } : undefined}>
            <div className="px-3 pb-1 mb-1 text-[11px] font-semibold text-fg-mute border-b border-border/50 uppercase tracking-wider">
              {worktreeChecked ? 'worktree 基线分支' : '切换分支'}
            </div>
            {model.branches.map((b) => (
              <button
                key={b}
                type="button"
                className={clsx(
                  'w-full text-left px-3 py-1.5 text-[13px] flex items-center gap-2 transition-colors',
                  b === displayBranch ? 'bg-accent/10 text-accent font-medium' : 'text-fg hover:bg-surface-hi',
                )}
                onClick={() => pickBranch(b)}
              >
                <VsIcon name="fork" size={12} className="opacity-60" />
                <span className="truncate">{b}</span>
                {b === model.branch && (
                  <span className="ml-auto text-[10px] text-fg-mute">当前</span>
                )}
              </button>
            ))}
          </div>
        </>
      )}

      {stashConfirm && (
        <Modal onClose={() => setStashConfirm(null)} width={440}>
          {({ close }) => (
            <div className="p-4">
              <div className="text-[14px] font-semibold mb-2">工作区有未提交的改动</div>
              <div className="text-[12.5px] text-fg-mute leading-relaxed mb-2">
                切换到 <span className="text-fg font-medium">{stashConfirm.branch}</span> 前,
                以下改动将存入 git stash(切回后可用 <code>git stash pop</code> 恢复):
              </div>
              <div className="max-h-[30vh] overflow-y-auto ace-scrollbar border border-border rounded-lg px-2.5 py-1.5 mb-3 text-[12px] font-mono">
                {stashConfirm.files.map((f) => (
                  <div key={f} className="truncate py-0.5">{f}</div>
                ))}
              </div>
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors"
                  onClick={close}
                >
                  取消
                </button>
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity"
                  onClick={() => {
                    const branch = stashConfirm.branch;
                    close();
                    setStashConfirm(null);
                    doCheckout(branch, true);
                  }}
                >
                  stash 并切换
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
    </div>
  );
}
