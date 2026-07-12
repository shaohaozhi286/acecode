import { formatBytes } from '../lib/format.js';
import {
  updateDialogMode,
  updateJobIsActive,
  updateJobPhaseLabel,
  updateJobProgress,
  updateRestartMessage,
} from '../lib/updateJob.js';
import { Modal } from './Modal.jsx';

function versionText(value) {
  const text = String(value || '').trim();
  if (!text) return '未知';
  return text.startsWith('v') ? text : `v${text}`;
}

export function UpdateDialog({
  open,
  updateStatus,
  job,
  starting = false,
  onConfirm,
  onRetry,
  onClose,
}) {
  if (!open) return null;

  const mode = updateDialogMode(job);
  const active = updateJobIsActive(job) || starting;
  const currentVersion = job?.current_version || updateStatus?.current_version;
  const targetVersion = job?.target_version || updateStatus?.latest_version;
  const packageSize = job?.bytes_total ?? updateStatus?.package_size;
  const progress = updateJobProgress(job);
  const phaseLabel = starting && !job ? '正在启动升级任务' : updateJobPhaseLabel(job);
  const restartMessage = updateRestartMessage(job);

  return (
    <Modal onClose={onClose} width={500} dismissOnBackdrop={!active}>
      <div className="px-5 py-4 border-b border-border">
        <div className="text-[15px] font-semibold text-fg">
          {mode === 'success' ? '升级安装完成' : mode === 'failure' ? '升级失败' : 'ACECode 升级'}
        </div>
        <div className="mt-1 text-[12px] text-fg-mute">
          {mode === 'confirm'
            ? '升级期间可以继续查看当前页面，请勿重复启动升级。'
            : phaseLabel}
        </div>
      </div>

      <div className="px-5 py-5">
        <div className="grid grid-cols-[88px_1fr] gap-y-2 text-[12px]">
          <span className="text-fg-mute">当前版本</span>
          <span className="text-fg font-medium tabular-nums">{versionText(currentVersion)}</span>
          <span className="text-fg-mute">目标版本</span>
          <span className="text-fg font-medium tabular-nums">{versionText(targetVersion)}</span>
          {Number(packageSize) > 0 && (
            <>
              <span className="text-fg-mute">安装包</span>
              <span className="text-fg tabular-nums">{formatBytes(Number(packageSize))}</span>
            </>
          )}
        </div>

        {(mode === 'running' || starting) && (
          <div className="mt-5">
            <div className="flex items-center justify-between text-[12px]">
              <span className="text-fg">{phaseLabel}</span>
              <span className="text-fg-mute tabular-nums">{progress}%</span>
            </div>
            <div className="mt-2 h-2 overflow-hidden rounded-full bg-surface-hi">
              <div
                className="h-full rounded-full bg-accent transition-[width] duration-300"
                style={{ width: `${progress}%` }}
              />
            </div>
            {job?.phase === 'downloading' && Number(job?.bytes_total) > 0 && (
              <div className="mt-2 text-[11px] text-fg-mute tabular-nums">
                {formatBytes(Number(job.bytes_downloaded || 0))} / {formatBytes(Number(job.bytes_total))}
              </div>
            )}
            <div className="mt-3 text-[11px] leading-5 text-fg-mute">
              安装完成前不要退出 ACECode。关闭此窗口只会隐藏进度，不会取消升级。
            </div>
          </div>
        )}

        {mode === 'success' && (
          <div className="mt-5 rounded-lg border border-ok bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg">
            {restartMessage || '升级已安装，请重新启动 ACECode。'}
          </div>
        )}

        {mode === 'failure' && (
          <div className="mt-5 rounded-lg border border-danger bg-surface-alt px-4 py-3">
            <div className="text-[12px] font-medium text-danger">升级没有完成</div>
            <div className="mt-1 break-words text-[11px] leading-5 text-fg-mute">
              {job?.error || '未知升级错误'}
            </div>
          </div>
        )}
      </div>

      <div className="flex items-center justify-end gap-2 border-t border-border bg-surface-alt px-5 py-3">
        {mode === 'confirm' && (
          <>
            <button
              type="button"
              onClick={onClose}
              disabled={starting}
              className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
            >
              取消
            </button>
            <button
              type="button"
              onClick={onConfirm}
              disabled={starting}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
            >
              {starting ? '正在启动…' : '立即升级'}
            </button>
          </>
        )}
        {mode === 'running' && (
          <button
            type="button"
            onClick={onClose}
            className="rounded-md px-4 py-1.5 text-[12px] text-fg hover:bg-surface-hi"
          >
            后台运行
          </button>
        )}
        {mode === 'success' && (
          <button
            type="button"
            onClick={onClose}
            className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
          >
            我知道了
          </button>
        )}
        {mode === 'failure' && (
          <>
            <button
              type="button"
              onClick={onClose}
              className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg"
            >
              关闭
            </button>
            <button
              type="button"
              onClick={onRetry}
              disabled={starting}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
            >
              {starting ? '正在重试…' : '重试'}
            </button>
          </>
        )}
      </div>
    </Modal>
  );
}
