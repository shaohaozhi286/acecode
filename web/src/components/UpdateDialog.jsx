import { formatBytes } from '../lib/format.js';
import {
  formatUpdatePublishedDate,
  normalizeUpdateReleases,
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
  restarting = false,
  restartAvailable = false,
  onConfirm,
  onRetry,
  onRestart,
  onClose,
}) {
  if (!open) return null;

  const mode = updateDialogMode(job, updateStatus);
  const active = updateJobIsActive(job) || starting || restarting;
  const currentVersion = job?.current_version || updateStatus?.current_version;
  const targetVersion = mode === 'up_to_date'
    ? currentVersion
    : job?.target_version || updateStatus?.latest_version;
  const packageSize = job?.bytes_total ?? updateStatus?.package_size;
  const releaseHistory = normalizeUpdateReleases(updateStatus?.releases);
  const progress = updateJobProgress(job);
  const phaseLabel = starting && !job ? '正在启动升级任务' : updateJobPhaseLabel(job);
  const restartMessage = updateRestartMessage(job);
  const title = mode === 'success'
    ? '升级安装完成'
    : mode === 'failure'
      ? '升级失败'
      : mode === 'up_to_date'
        ? '已是最新版本'
        : 'ACECode 升级';
  const subtitle = mode === 'confirm'
    ? '升级期间可以继续查看当前页面，请勿重复启动升级。'
    : mode === 'up_to_date'
      ? '当前安装的 ACECode 已是最新版本。'
      : phaseLabel;

  return (
    <Modal onClose={onClose} width={560} dismissOnBackdrop={!active}>
      <div className="px-5 py-4 border-b border-border">
        <div className="text-[15px] font-semibold text-fg">{title}</div>
        <div className="mt-1 text-[12px] text-fg-mute">
          {subtitle}
        </div>
      </div>

      <div className="max-h-[calc(100vh-10rem)] overflow-y-auto px-5 py-5">
        <div className="grid grid-cols-[88px_1fr] gap-y-2 text-[12px]">
          <span className="text-fg-mute">当前版本</span>
          <span className="text-fg font-medium tabular-nums">{versionText(currentVersion)}</span>
          <span className="text-fg-mute">
            {mode === 'up_to_date' ? '最新版本' : '目标版本'}
          </span>
          <span className="text-fg font-medium tabular-nums">{versionText(targetVersion)}</span>
          {Number(packageSize) > 0 && (
            <>
              <span className="text-fg-mute">安装包</span>
              <span className="text-fg tabular-nums">{formatBytes(Number(packageSize))}</span>
            </>
          )}
        </div>

        {mode === 'up_to_date' && (
          <div className="mt-5 rounded-lg border border-ok bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg">
            当前已是最新版本，无需升级。
          </div>
        )}

        {releaseHistory.length > 0 && (
          <section className="mt-5" aria-label="版本更新记录">
            <div className="text-[14px] font-semibold text-fg">版本更新记录</div>
            <div className="mt-1 text-[11px] text-fg-mute">
              来自升级服务的历次版本说明
            </div>
            <div className="mt-3 max-h-64 overflow-y-auto rounded-lg border border-border bg-surface-alt">
              {releaseHistory.map((release, index) => {
                const publishedDate = formatUpdatePublishedDate(release.published_at);
                const latest = release.version === updateStatus?.latest_version;
                return (
                  <article
                    key={`${release.version}-${index}`}
                    className={[
                      'px-3.5 py-3',
                      index > 0 ? 'border-t border-border' : '',
                    ].join(' ')}
                  >
                    <div className="flex items-center gap-2">
                      <span className="text-[12px] font-semibold text-fg tabular-nums">
                        {versionText(release.version)}
                      </span>
                      {latest && (
                        <span className="rounded bg-accent-bg px-1.5 py-0.5 text-[10px] font-medium text-accent">
                          最新
                        </span>
                      )}
                      {publishedDate && (
                        <span className="ml-auto text-[10px] text-fg-mute tabular-nums">
                          {publishedDate}
                        </span>
                      )}
                    </div>
                    <div className="mt-1.5 whitespace-pre-wrap break-words text-[12px] leading-5 text-fg-2">
                      {release.notes}
                    </div>
                  </article>
                );
              })}
            </div>
          </section>
        )}

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
            {restartAvailable
              ? '升级已安装，是否立即重启 ACECode 以使用新版本？'
              : (restartMessage || '升级已安装，请重新启动 ACECode。')}
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
        {mode === 'up_to_date' && (
          <button
            type="button"
            onClick={onClose}
            className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
          >
            关闭
          </button>
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
          restartAvailable ? (
            <>
              <button
                type="button"
                onClick={onClose}
                disabled={restarting}
                className="rounded-md px-3 py-1.5 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
              >
                稍后重启
              </button>
              <button
                type="button"
                onClick={onRestart}
                disabled={restarting}
                className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-60"
              >
                {restarting ? '正在重启…' : '立即重启'}
              </button>
            </>
          ) : (
            <button
              type="button"
              onClick={onClose}
              className="rounded-md bg-accent px-4 py-1.5 text-[12px] font-medium text-white hover:opacity-90"
            >
              我知道了
            </button>
          )
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
