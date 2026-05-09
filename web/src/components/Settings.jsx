// web/src/components/Settings.jsx
// 设置抽屉容器(右侧滑出),与 SettingsPage(全屏)区分:
// - SettingsPage:TopBar 齿轮触发,Codex 风格全屏设置页;
// - Settings 抽屉:Sidebar 齿轮触发,聚焦"模型管理"等运维型操作。
// 本期只挂 ModelManager;后续可扩 MCP / Skills / 高级配置。

import { ModelManager } from './ModelManager.jsx';

export function Settings({ open, onClose }) {
  if (!open) return null;
  return (
    <div className="fixed inset-0 z-[200] bg-black/40 flex justify-end" onClick={onClose}>
      <div
        className="w-[640px] h-full bg-surface text-fg shadow-xl overflow-auto"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-4 py-3 border-b border-border">
          <div className="text-sm font-semibold">设置</div>
          <button
            type="button"
            className="px-2 py-1 hover:bg-surface-hi rounded text-[12px]"
            onClick={onClose}
          >
            关闭
          </button>
        </div>
        <div className="p-4 space-y-6">
          <section>
            <h2 className="text-sm font-semibold mb-2">模型</h2>
            <ModelManager />
          </section>
        </div>
      </div>
    </div>
  );
}
