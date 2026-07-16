function desktopBridgeOrNull(desktopBridge) {
  if (desktopBridge !== undefined) return desktopBridge;
  return typeof window === 'undefined' ? null : window;
}
export function parseWorkspacePickerResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

export async function pickExistingWorkspace({ api, desktopBridge } = {}) {
  if (!api) throw new Error('workspace picker api required');

  const bridge = desktopBridgeOrNull(desktopBridge);
  const usesDesktopBridge = typeof bridge?.aceDesktop_addWorkspace === 'function';
  const workspace = usesDesktopBridge
    ? parseWorkspacePickerResult(await bridge.aceDesktop_addWorkspace())
    : await api.pickWorkspaceFolder();

  if (workspace == null) return null;
  if (workspace?.error) throw new Error(String(workspace.error));
  if (!workspace?.hash) return null;

  if (usesDesktopBridge && workspace.cwd) {
    try {
      await api.registerWorkspace(workspace.cwd);
    } catch {
      // The desktop bridge may already have registered this directory.
    }
  }
  return workspace;
}
