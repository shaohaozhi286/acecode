import { Modal } from './Modal.jsx';

export function DesktopCloseDialog({
  open,
  remember,
  busy = false,
  trayAvailable = true,
  onRememberChange,
  onMinimizeToTray,
  onExit,
  onClose,
}) {
  if (!open) return null;
  return (
    <Modal
      onClose={busy ? undefined : onClose}
      width={440}
      dismissOnBackdrop={!busy}
      layerClassName="z-[400]"
    >
      <div className="p-4">
        <div className="text-[14px] font-semibold mb-2">关闭窗口</div>
        <div className="text-[12.5px] text-fg-mute leading-relaxed mb-3">
          关闭窗口时您希望执行什么操作？退出应用将停止所有正在运行的任务和定时任务。
        </div>
        <label className="mb-4 inline-flex cursor-pointer items-center gap-2 text-[12.5px] text-fg-2">
          <input
            type="checkbox"
            checked={remember}
            disabled={busy}
            onChange={(event) => onRememberChange?.(event.target.checked)}
            className="h-3.5 w-3.5 shrink-0 accent-accent disabled:opacity-60"
          />
          <span>记住我的选择</span>
        </label>
        <div className="flex justify-end gap-2">
          <button
            type="button"
            disabled={busy || !trayAvailable}
            onClick={onMinimizeToTray}
            title={trayAvailable ? '最小化到托盘' : '系统托盘不可用'}
            className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi transition-colors disabled:cursor-not-allowed disabled:opacity-50"
          >
            最小化到托盘
          </button>
          <button
            type="button"
            disabled={busy}
            onClick={onExit}
            className="px-3 py-1.5 text-[12.5px] rounded-lg bg-accent text-white hover:opacity-90 transition-opacity disabled:cursor-wait disabled:opacity-60"
          >
            退出应用
          </button>
        </div>
      </div>
    </Modal>
  );
}

