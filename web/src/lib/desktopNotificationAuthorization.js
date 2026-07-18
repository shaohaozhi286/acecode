export const NOTIFICATION_AUTHORIZATION_EVENT =
  'acecode:notification-authorization-changed';

const AUTHORIZATION_STATUSES = new Set([
  'unknown',
  'not_determined',
  'requesting',
  'denied',
  'authorized',
  'provisional',
  'unavailable',
]);

function parseBridgeResult(value) {
  if (value && typeof value === 'object') return value;
  if (typeof value !== 'string') return null;
  try {
    const parsed = JSON.parse(value);
    return parsed && typeof parsed === 'object' ? parsed : null;
  } catch {
    return null;
  }
}

export function normalizeNotificationAuthorization(value) {
  const parsed = parseBridgeResult(value) || {};
  const status = AUTHORIZATION_STATUSES.has(parsed.status)
    ? parsed.status
    : 'unavailable';
  return {
    ok: parsed.ok !== false && status !== 'unavailable',
    status,
    canRequest: parsed.can_request === true,
    canOpenSettings: parsed.can_open_settings === true,
  };
}

export function macNotificationAuthorizationAvailable(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return !!win
    && win.__ACECODE_DESKTOP_SHELL__ === true
    && win.__ACECODE_OS__ === 'macos'
    && typeof win.aceDesktop_getNotificationAuthorization === 'function';
}

async function invokeAuthorizationBridge(win, name) {
  if (!macNotificationAuthorizationAvailable(win)
      || typeof win[name] !== 'function') {
    return normalizeNotificationAuthorization(null);
  }
  try {
    return normalizeNotificationAuthorization(await win[name]());
  } catch {
    return normalizeNotificationAuthorization(null);
  }
}

export function getMacNotificationAuthorization(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return invokeAuthorizationBridge(
    win,
    'aceDesktop_getNotificationAuthorization',
  );
}

export function requestMacNotificationAuthorization(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return invokeAuthorizationBridge(
    win,
    'aceDesktop_requestNotificationAuthorization',
  );
}

export async function openMacNotificationSettings(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!macNotificationAuthorizationAvailable(win)
      || typeof win.aceDesktop_openNotificationSettings !== 'function') {
    return false;
  }
  try {
    const result = parseBridgeResult(
      await win.aceDesktop_openNotificationSettings(),
    );
    return result?.ok === true;
  } catch {
    return false;
  }
}

export function subscribeMacNotificationAuthorization(
  handler,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!win || typeof win.addEventListener !== 'function'
      || typeof handler !== 'function') {
    return () => {};
  }
  const listener = (event) => {
    handler(normalizeNotificationAuthorization(event?.detail));
  };
  win.addEventListener(NOTIFICATION_AUTHORIZATION_EVENT, listener);
  return () => {
    win.removeEventListener?.(NOTIFICATION_AUTHORIZATION_EVENT, listener);
  };
}

export function notificationAuthorizationPresentation(value) {
  const state = normalizeNotificationAuthorization(value);
  switch (state.status) {
    case 'authorized':
      return {
        ...state,
        label: '已允许',
        description: 'macOS 已允许 ACECode 显示系统通知',
        tone: 'ok',
      };
    case 'provisional':
      return {
        ...state,
        label: '静默允许',
        description: '通知会静默进入通知中心',
        tone: 'ok',
      };
    case 'denied':
      return {
        ...state,
        label: '已拒绝',
        description: '请在 macOS 系统设置中允许 ACECode 通知',
        tone: 'danger',
      };
    case 'requesting':
      return {
        ...state,
        label: '等待确认',
        description: '请在 macOS 授权提示中选择是否允许',
        tone: 'warn',
      };
    case 'not_determined':
      return {
        ...state,
        label: '尚未授权',
        description: '允许后才能收到会话完成和待处理提醒',
        tone: 'warn',
      };
    case 'unknown':
      return {
        ...state,
        label: '正在检查',
        description: '正在读取 macOS 系统通知权限',
        tone: 'muted',
      };
    default:
      return {
        ...state,
        label: '不可用',
        description: '当前运行方式不支持 macOS 系统通知',
        tone: 'muted',
      };
  }
}
