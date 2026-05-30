const ACE_SOURCE = "ace-browser-bridge";
const DEFAULT_PORT = 52007;
const PROTOCOL_VERSION = "0.1";
const HEARTBEAT_ALARM = "ace-browser-host-heartbeat";
// 0.5 分钟 = 30 秒,是 Chrome 允许的最小 alarm 周期。service worker 一旦被回收,alarm 会在
// 约 30 秒内把它冷启唤醒并恢复轮询,作为保活 ping 的兜底。
const HEARTBEAT_PERIOD_MINUTES = 0.5;
const KEEPALIVE_PORT_NAME = "ace-keepalive";
const KEEPALIVE_PING_MS = 20000;
const SESSION_REGISTRY_STORAGE_KEY = "aceBrowserBridgeSessions";
const NAVIGATION_TIMEOUT_MS = 15000;
const PLUGIN_LOG_TIMEOUT_MS = 800;

let daemonPort = DEFAULT_PORT;
let connectionState = {
  connected: false,
  port: DEFAULT_PORT,
  lastHelloAt: null,
  lastError: null,
  extensionVersion: chrome.runtime.getManifest().version
};
let polling = false;
let keepAlivePort = null;
let keepAliveTimer = null;
const sessions = new Map();
const traceBySession = new Map();
const consoleBySession = new Map();
const networkBySession = new Map();
const emulationBySession = new Map();
const performanceBySession = new Map();
const heapBySession = new Map();
const debuggerTabs = new Set();
const tabSessionByTabId = new Map();
let registryRestorePromise = null;
let registryPersistTimer = null;

function daemonBaseUrl() {
  return `http://127.0.0.1:${daemonPort}`;
}

function capabilities() {
  return {
    cdp: true,
    devtools: true,
    raw_cdp: true,
    console: true,
    network: true,
    emulation: true,
    performance: true,
    heap_snapshot: true,
    pdf: true,
    upload: true,
    os_pointer: false,
    operation_overlay: true,
    input_block: true
  };
}

async function saveConnectionState() {
  await chrome.storage.local.set({ aceBrowserBridgeConnection: connectionState });
}

function registryStorage() {
  return chrome.storage.session || chrome.storage.local;
}

function shortSessionHash(value) {
  let hash = 2166136261;
  for (const ch of String(value || "")) {
    hash ^= ch.codePointAt(0);
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash.toString(36).padStart(6, "0").slice(-6);
}

function defaultGroupTitle(session) {
  return `ACE-${shortSessionHash(session)}`;
}

function isLegacyDefaultGroupTitle(session, title) {
  return title === `ACE: ${String(session || "").slice(0, 24)}`;
}

function normalizedSessionState(raw) {
  if (!raw || typeof raw.session !== "string" || !raw.session) return null;
  const tabId = Number(raw.tabId);
  const groupId = Number(raw.groupId);
  const storedTitle = typeof raw.groupTitle === "string" ? raw.groupTitle : "";
  const inputBlockExpiresAt = Number(raw.inputBlockExpiresAt || 0);
  const inputBlockActive = raw.inputBlocked === true &&
    (!Number.isFinite(inputBlockExpiresAt) || inputBlockExpiresAt <= 0 || Date.now() < inputBlockExpiresAt);
  return {
    session: raw.session,
    tabId: Number.isFinite(tabId) && tabId > 0 ? tabId : null,
    ownership: raw.ownership === "owned" ? "owned" : "adopted",
    groupId: Number.isFinite(groupId) ? groupId : null,
    status: typeof raw.status === "string" && raw.status ? raw.status : "idle",
    groupTitle: storedTitle && !isLegacyDefaultGroupTitle(raw.session, storedTitle)
      ? storedTitle
      : defaultGroupTitle(raw.session),
    lastPointer: raw.lastPointer && Number.isFinite(Number(raw.lastPointer.x)) && Number.isFinite(Number(raw.lastPointer.y))
      ? { x: Number(raw.lastPointer.x), y: Number(raw.lastPointer.y) }
      : null,
    inputBlocked: inputBlockActive,
    inputBlockWatchdogMs: Number.isFinite(Number(raw.inputBlockWatchdogMs))
      ? Number(raw.inputBlockWatchdogMs)
      : 300000,
    inputBlockExpiresAt: inputBlockActive && Number.isFinite(inputBlockExpiresAt) ? inputBlockExpiresAt : 0,
    inputBlockMessage: typeof raw.inputBlockMessage === "string" ? raw.inputBlockMessage : ""
  };
}

function serializedSessionRegistry() {
  return Array.from(sessions.values()).map((state) => ({
    session: state.session,
    tabId: state.tabId,
    ownership: state.ownership,
    groupId: state.groupId,
    status: state.status,
    groupTitle: state.groupTitle,
    lastPointer: state.lastPointer,
    inputBlocked: state.inputBlocked === true,
    inputBlockWatchdogMs: state.inputBlockWatchdogMs || 300000,
    inputBlockExpiresAt: state.inputBlockExpiresAt || 0,
    inputBlockMessage: state.inputBlockMessage || ""
  }));
}

async function persistSessionRegistryNow() {
  try {
    await registryStorage().set({
      [SESSION_REGISTRY_STORAGE_KEY]: serializedSessionRegistry()
    });
  } catch (error) {
    connectionState.lastError = error instanceof Error ? error.message : String(error);
  }
}

function scheduleSessionRegistryPersist() {
  if (registryPersistTimer !== null) clearTimeout(registryPersistTimer);
  registryPersistTimer = setTimeout(() => {
    registryPersistTimer = null;
    persistSessionRegistryNow();
  }, 50);
}

async function restoreSessionRegistry() {
  try {
    const stored = await registryStorage().get(SESSION_REGISTRY_STORAGE_KEY);
    const items = stored?.[SESSION_REGISTRY_STORAGE_KEY];
    if (!Array.isArray(items)) return;
    for (const item of items) {
      const state = normalizedSessionState(item);
      if (state) sessions.set(state.session, state);
    }
  } catch (error) {
    connectionState.lastError = error instanceof Error ? error.message : String(error);
  }
}

async function ensureSessionRegistryRestored() {
  if (!registryRestorePromise) registryRestorePromise = restoreSessionRegistry();
  await registryRestorePromise;
}

async function helloDaemon() {
  try {
    const response = await fetch(`${daemonBaseUrl()}/plugin/hello`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Ace-Browser-Bridge": "extension"
      },
      body: JSON.stringify({
        source: ACE_SOURCE,
        protocol_version: PROTOCOL_VERSION,
        extension_version: chrome.runtime.getManifest().version,
        browser: "chromium",
        capabilities: capabilities()
      })
    });
    const payload = await response.json();
    if (!payload?.ok) {
      throw new Error(payload?.error?.message || "daemon rejected plugin hello");
    }
    connectionState = {
      connected: true,
      port: daemonPort,
      lastHelloAt: new Date().toISOString(),
      lastError: null,
      extensionVersion: chrome.runtime.getManifest().version
    };
  } catch (error) {
    connectionState = {
      ...connectionState,
      connected: false,
      port: daemonPort,
      lastError: error instanceof Error ? error.message : String(error)
    };
  }
  await saveConnectionState();
  return connectionState;
}

async function postDaemon(path, body) {
  const response = await fetch(`${daemonBaseUrl()}${path}`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-Ace-Browser-Bridge": "extension"
    },
    body: JSON.stringify(body || {})
  });
  return response.json();
}

function postPluginLog(level, message, data = {}) {
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), PLUGIN_LOG_TIMEOUT_MS);
    fetch(`${daemonBaseUrl()}/plugin/log`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Ace-Browser-Bridge": "extension"
      },
      signal: controller.signal,
      body: JSON.stringify({
        source: ACE_SOURCE,
        level,
        message,
        at: new Date().toISOString(),
        data: {
          extension_version: chrome.runtime.getManifest().version,
          port: daemonPort,
          ...data
        }
      })
    }).catch(() => {
      // Best-effort diagnostics only.
    }).finally(() => {
      clearTimeout(timer);
    });
  } catch (_error) {
    // Best-effort diagnostics only; never let logging break browser actions.
  }
}

async function ensureContentScript(tabId) {
  await chrome.scripting.executeScript({
    target: { tabId },
    files: ["content/virtual-cursor.js"]
  });
}

async function activePageTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab?.id) throw new Error("No active tab found.");
  if (!tab.url || /^(chrome|edge|about):/i.test(tab.url)) {
    throw new Error("This page cannot run extension scripts.");
  }
  return tab;
}

function sessionName(action) {
  return action.session || action.args?.session || "acecode-default";
}

function actionLogData(action, extra = {}) {
  return {
    id: String(action?.id || ""),
    action: String(action?.action || ""),
    session: sessionName(action || {}),
    ...extra
  };
}

function sessionState(session) {
  if (!sessions.has(session)) {
    sessions.set(session, {
      session,
      tabId: null,
      ownership: "adopted",
      groupId: null,
      status: "idle",
      groupTitle: defaultGroupTitle(session),
      lastPointer: null,
      inputBlocked: false,
      inputBlockWatchdogMs: 300000,
      inputBlockExpiresAt: 0,
      inputBlockMessage: ""
    });
  }
  return sessions.get(session);
}

function trace(session, entry) {
  const list = traceBySession.get(session) || [];
  list.push({
    at: new Date().toISOString(),
    session,
    ...entry
  });
  while (list.length > 50) list.shift();
  traceBySession.set(session, list);
}

function clearInputBlockState(state) {
  state.inputBlocked = false;
  state.inputBlockWatchdogMs = 300000;
  state.inputBlockExpiresAt = 0;
  state.inputBlockMessage = "";
}

function inputBlockIsActive(state) {
  if (!state?.inputBlocked) return false;
  if (state.inputBlockExpiresAt && Date.now() >= state.inputBlockExpiresAt) {
    clearInputBlockState(state);
    scheduleSessionRegistryPersist();
    return false;
  }
  return true;
}

function inputBlockOverlayArgs(state) {
  return {
    watchdog_ms: state.inputBlockWatchdogMs || 300000,
    message: state.inputBlockMessage || "AI 正在操作浏览器，请暂时不要操作"
  };
}

async function applyInputBlockOverlay(session) {
  const state = sessionState(session);
  if (!inputBlockIsActive(state)) return { visible: false, pending: false };
  if (!(await tabExists(state.tabId))) return { visible: false, pending: true };
  try {
    await sendPageCommand("show_overlay", inputBlockOverlayArgs(state), state.tabId);
    return { visible: true, pending: false };
  } catch (error) {
    connectionState.lastError = error instanceof Error ? error.message : String(error);
    return { visible: false, pending: true, warning: connectionState.lastError };
  }
}

function statusAfterSuccessfulOperation(session) {
  const state = sessions.get(session);
  if (inputBlockIsActive(state)) return "operating";
  return networkBySession.get(session)?.active ? "network" : "idle";
}

function debuggerFeatureActive(session) {
  return Boolean(
    consoleBySession.get(session)?.active ||
    networkBySession.get(session)?.active ||
    emulationBySession.get(session)?.active ||
    performanceBySession.get(session)?.active ||
    heapBySession.get(session)?.active
  );
}

async function detachDebuggerIfIdle(session, tabId) {
  if (!tabId || debuggerFeatureActive(session)) return;
  await detachDebugger(tabId);
}

async function tabExists(tabId) {
  if (!tabId) return false;
  try {
    await chrome.tabs.get(tabId);
    return true;
  } catch {
    return false;
  }
}

async function waitForTabComplete(tabId, timeoutMs = NAVIGATION_TIMEOUT_MS) {
  const deadline = Date.now() + timeoutMs;

  await sleep(250);
  while (Date.now() < deadline) {
    try {
      const tab = await chrome.tabs.get(tabId);
      if (tab.status === "complete") return tab;
    } catch (error) {
      throw Object.assign(new Error("navigation tab was closed"), { code: "navigation_tab_closed" });
    }
    await sleep(100);
  }

  throw Object.assign(new Error("timed out waiting for page navigation to complete"), {
    code: "navigation_timeout"
  });
}

async function tabForSession(session) {
  const state = sessionState(session);
  if (await tabExists(state.tabId)) {
    return chrome.tabs.get(state.tabId);
  }
  const tab = await activePageTab();
  state.tabId = tab.id;
  state.ownership = "adopted";
  await persistSessionRegistryNow();
  return tab;
}

function colorForStatus(status) {
  if (status === "operating") return "cyan";
  if (status === "network") return "blue";
  if (status === "waiting") return "yellow";
  if (status === "error") return "red";
  return "green";
}

async function updateGroup(session, title, status = "idle") {
  const state = sessionState(session);
  state.status = status;
  if (title) state.groupTitle = title;
  if (!state.tabId || !chrome.tabs.group || !chrome.tabGroups) {
    await persistSessionRegistryNow();
    return;
  }
  try {
    const groupId = await chrome.tabs.group({ tabIds: [state.tabId] });
    state.groupId = groupId;
    await chrome.tabGroups.update(groupId, {
      title: state.groupTitle,
      color: colorForStatus(status)
    });
  } catch (error) {
    connectionState.lastError = error instanceof Error ? error.message : String(error);
  } finally {
    await persistSessionRegistryNow();
  }
}

async function ensureDebugger(tabId) {
  if (debuggerTabs.has(tabId)) return;
  try {
    await chrome.debugger.attach({ tabId }, "1.3");
    debuggerTabs.add(tabId);
  } catch (error) {
    const wrapped = new Error(error instanceof Error ? error.message : String(error));
    wrapped.code = "cdp_unavailable";
    throw wrapped;
  }
}

async function detachDebugger(tabId) {
  if (!debuggerTabs.has(tabId)) return;
  try {
    await chrome.debugger.detach({ tabId });
  } catch {
    // Ignore detach races.
  }
  debuggerTabs.delete(tabId);
  tabSessionByTabId.delete(tabId);
}

async function cdpCommand(tabId, method, params = {}) {
  await ensureDebugger(tabId);
  try {
    return await chrome.debugger.sendCommand({ tabId }, method, params);
  } catch (error) {
    const wrapped = new Error(error instanceof Error ? error.message : String(error));
    wrapped.code = "cdp_unavailable";
    throw wrapped;
  }
}

async function sendPageCommand(command, args = {}, tabId = null) {
  const tab = tabId ? await chrome.tabs.get(tabId) : await activePageTab();
  await ensureContentScript(tab.id);
  return chrome.tabs.sendMessage(tab.id, {
    source: ACE_SOURCE,
    command,
    args
  });
}

async function sendSessionPageCommand(session, command, args = {}) {
  const tab = await tabForSession(session);
  return sendPageCommand(command, args, tab.id);
}

async function withOperatingOverlay(session, actionName, args, fn) {
  await updateGroup(session, null, actionName === "wait" ? "waiting" : "operating");
  const state = sessionState(session);
  const manualBlock = inputBlockIsActive(state);
  const watchdogMs = manualBlock
    ? (state.inputBlockWatchdogMs || 300000)
    : (args?.operation_overlay_watchdog_ms || 10000);
  try {
    await sendSessionPageCommand(session, "show_overlay", {
      watchdog_ms: watchdogMs,
      message: manualBlock ? state.inputBlockMessage : undefined
    });
  } catch {
    // Some pages cannot receive content scripts; the action below will return the real error.
  }
  try {
    const result = await fn();
    trace(session, {
      action: actionName,
      ok: Boolean(result?.ok),
      error: result?.error?.code || null
    });
    await updateGroup(session, null, result?.ok ? statusAfterSuccessfulOperation(session) : "error");
    return result;
  } catch (error) {
    trace(session, {
      action: actionName,
      ok: false,
      error: error?.code || "extension_error"
    });
    await updateGroup(session, null, "error");
    return bridgeError(error);
  } finally {
    if (!inputBlockIsActive(sessionState(session))) {
      try {
        await sendSessionPageCommand(session, "hide_overlay");
      } catch {
        // Ignore cleanup failures; content script watchdog also removes the overlay.
      }
    }
  }
}

async function handleNavigate(session, args) {
  const state = sessionState(session);
  const operation = args.operation || (args.url ? "goto" : "reload");
  const timeoutMs = clampNumber(args.timeout_ms, 1000, 120000, NAVIGATION_TIMEOUT_MS);

  if (operation === "goto") {
    if (!args.url) {
      return { ok: false, error: { code: "invalid_request", message: "navigate goto requires url" } };
    }
    let tab;
    if (args.newTab === true || !(await tabExists(state.tabId))) {
      tab = await chrome.tabs.create({ url: args.url, active: true });
      state.tabId = tab.id;
      state.ownership = "owned";
    } else {
      tab = await chrome.tabs.update(state.tabId, { url: args.url, active: true });
    }
    await updateGroup(session, args.group_title, "waiting");
    tab = await waitForTabComplete(state.tabId, timeoutMs);
    const blockState = await applyInputBlockOverlay(session);
    await updateGroup(session, args.group_title, statusAfterSuccessfulOperation(session));
    trace(session, { action: "navigate", url: args.url, tabId: state.tabId });
    return { ok: true, data: { success: true, url: tab.url || args.url, tabId: state.tabId, ownership: state.ownership, loaded: true, input_block: blockState } };
  }

  const tab = await tabForSession(session);
  if (operation === "reload") await chrome.tabs.reload(tab.id);
  else if (operation === "back") await chrome.tabs.goBack(tab.id);
  else if (operation === "forward") await chrome.tabs.goForward(tab.id);
  else return { ok: false, error: { code: "invalid_request", message: `Unsupported navigate operation: ${operation}` } };
  await updateGroup(session, null, "waiting");
  const loadedTab = await waitForTabComplete(tab.id, timeoutMs);
  const blockState = await applyInputBlockOverlay(session);
  await updateGroup(session, null, statusAfterSuccessfulOperation(session));
  trace(session, { action: "navigate", operation, tabId: tab.id });
  return { ok: true, data: { success: true, operation, tabId: tab.id, url: loadedTab.url || tab.url, loaded: true, input_block: blockState } };
}

async function handleBlockInput(session, args = {}) {
  const state = sessionState(session);
  const watchdogMs = clampNumber(args.watchdog_ms ?? args.timeout_ms, 1000, 1800000, 300000);
  state.inputBlocked = true;
  state.inputBlockWatchdogMs = watchdogMs;
  state.inputBlockExpiresAt = Date.now() + watchdogMs;
  state.inputBlockMessage = typeof args.message === "string" ? args.message.slice(0, 96) : "";

  const blockState = await applyInputBlockOverlay(session);
  await updateGroup(session, null, "operating");
  trace(session, {
    action: "block_input",
    ok: true,
    visible: blockState.visible === true,
    pending: blockState.pending === true
  });
  return {
    ok: true,
    data: {
      success: true,
      blocked: true,
      visible: blockState.visible === true,
      pending: blockState.pending === true,
      warning: blockState.warning || null,
      watchdog_ms: watchdogMs,
      expires_at: new Date(state.inputBlockExpiresAt).toISOString()
    }
  };
}

async function handleUnblockInput(session) {
  const state = sessionState(session);
  clearInputBlockState(state);
  let warning = null;
  if (await tabExists(state.tabId)) {
    try {
      await sendPageCommand("hide_overlay", {}, state.tabId);
    } catch (error) {
      warning = error instanceof Error ? error.message : String(error);
      connectionState.lastError = warning;
    }
  }
  await updateGroup(session, null, statusAfterSuccessfulOperation(session));
  trace(session, { action: "unblock_input", ok: true, warning });
  return {
    ok: true,
    data: {
      success: true,
      blocked: false,
      visible: false,
      warning
    }
  };
}

async function handleFindTab(session, args) {
  const state = sessionState(session);
  let tab = null;
  const requestedTabId = args.tabId || args.tab_id;
  if (requestedTabId) {
    try {
      tab = await chrome.tabs.get(Number(requestedTabId));
    } catch {
      tab = null;
    }
  } else if (args.active) {
    tab = await activePageTab();
  } else {
    const tabs = await chrome.tabs.query({});
    const needle = String(args.url || "").toLowerCase();
    tab = tabs.find((item) => (item.url || "").toLowerCase().includes(needle));
  }
  if (!tab?.id) {
    return { ok: false, error: { code: "tab_not_found", message: "No matching browser tab found" } };
  }
  state.tabId = tab.id;
  state.ownership = "adopted";
  const blockState = await applyInputBlockOverlay(session);
  await updateGroup(session, args.group_title, statusAfterSuccessfulOperation(session));
  trace(session, { action: "find_tab", url: tab.url, tabId: tab.id });
  return { ok: true, data: { success: true, url: tab.url, title: tab.title, tabId: tab.id, ownership: "adopted", input_block: blockState } };
}

async function handleListTabs(session) {
  const tabs = [];
  for (const [name, state] of sessions.entries()) {
    let tab = null;
    if (await tabExists(state.tabId)) tab = await chrome.tabs.get(state.tabId);
    if (session && name !== session) continue;
    let group = null;
    if (state.groupId !== null && state.groupId !== undefined && chrome.tabGroups) {
      try {
        group = await chrome.tabGroups.get(state.groupId);
      } catch {
        group = null;
      }
    }
    tabs.push({
      session: name,
      tabId: state.tabId,
      ownership: state.ownership,
      groupId: state.groupId,
      groupTitle: state.groupTitle,
      groupColor: group?.color || null,
      group_color: group?.color || null,
      status: state.status,
      inputBlocked: inputBlockIsActive(state),
      input_blocked: inputBlockIsActive(state),
      active: Boolean(tab?.active),
      url: tab?.url || null,
      title: tab?.title || null
    });
  }
  return { ok: true, data: { success: true, tabs } };
}

async function handleCloseSession(session) {
  const state = sessionState(session);
  let closed = 0;
  let detached = 0;
  if (await tabExists(state.tabId)) {
    try {
      await sendPageCommand("hide_overlay", {}, state.tabId);
    } catch {
      // Ignore cleanup failures while closing or detaching the tab.
    }
    if (state.ownership === "owned") {
      await chrome.tabs.remove(state.tabId);
      closed = 1;
    } else {
      detached = 1;
    }
  }
  sessions.delete(session);
  traceBySession.delete(session);
  consoleBySession.delete(session);
  networkBySession.delete(session);
  emulationBySession.delete(session);
  performanceBySession.delete(session);
  heapBySession.delete(session);
  await persistSessionRegistryNow();
  return { ok: true, data: { success: true, closed, detached } };
}

function networkState(session) {
  const existing = networkBySession.get(session);
  if (existing) return existing;
  const state = { active: false, tabId: null, requests: [] };
  networkBySession.set(session, state);
  return state;
}

function networkRequestSummary(request) {
  const { body, responseBody, ...summary } = request;
  if (body !== undefined) {
    summary.body = typeof body === "string"
      ? { inline: true, sizeBytes: body.length }
      : body;
  }
  if (responseBody !== undefined) {
    summary.responseBody = typeof responseBody === "string"
      ? { inline: true, sizeBytes: responseBody.length }
      : responseBody;
    summary.response_body = summary.responseBody;
  }
  return summary;
}

async function handleNetwork(session, args) {
  const state = networkState(session);
  const cmd = args.cmd || "list";
  if (cmd === "start") {
    const tab = await tabForSession(session);
    await ensureDebugger(tab.id);
    tabSessionByTabId.set(tab.id, session);
    await cdpCommand(tab.id, "Network.enable");
    state.active = true;
    state.tabId = tab.id;
    state.requests = [];
    await updateGroup(session, null, "network");
    return { ok: true, data: { success: true, message: "network capture started", tabId: tab.id } };
  }
  if (cmd === "stop") {
    state.active = false;
    if (state.tabId) await detachDebuggerIfIdle(session, state.tabId);
    await updateGroup(session, null, "idle");
    return { ok: true, data: { success: true, message: "network capture stopped", count: state.requests.length } };
  }
  if (cmd === "list") {
    const filter = String(args.filter || "");
    const typeFilter = String(args.resource_type || args.resourceType || "").toLowerCase();
    const requests = state.requests
      .filter((req) => !filter || req.url.includes(filter))
      .filter((req) => !typeFilter || String(req.resourceType || "").toLowerCase() === typeFilter)
      .map(networkRequestSummary);
    return { ok: true, data: { count: requests.length, requests } };
  }
  if (cmd === "detail") {
    const requestId = args.requestId || args.request_id;
    const request = state.requests.find((item) => item.requestId === requestId || item.request_id === requestId);
    if (!request) {
      return { ok: false, error: { code: "request_not_found", message: `request not found: ${requestId}` } };
    }
    if (state.tabId && debuggerTabs.has(state.tabId)) {
      try {
        const body = await chrome.debugger.sendCommand({ tabId: state.tabId }, "Network.getResponseBody", { requestId: request.requestId });
        if (body?.base64Encoded) {
          request.responseBody = {
            base64Encoded: true,
            body: body.body || "",
            sizeBytes: body.body?.length || 0
          };
          request.response_body = request.responseBody;
        } else if (typeof body?.body === "string") {
          request.responseBody = body.body;
          request.response_body = body.body;
        }
      } catch (error) {
        request.bodyError = error instanceof Error ? error.message : String(error);
        request.body_error = request.bodyError;
      }
    }
    return { ok: true, data: request };
  }
  return { ok: false, error: { code: "invalid_request", message: `unknown network cmd: ${cmd}` } };
}

function consoleState(session) {
  const existing = consoleBySession.get(session);
  if (existing) return existing;
  const state = { active: false, tabId: null, nextId: 1, messages: [] };
  consoleBySession.set(session, state);
  return state;
}

function remoteObjectValue(obj) {
  if (!obj) return null;
  if (Object.prototype.hasOwnProperty.call(obj, "value")) return obj.value;
  return obj.description ?? obj.unserializableValue ?? `[${obj.type || "object"}]`;
}

function appendConsoleMessage(session, message) {
  const state = consoleState(session);
  if (!state.active) return;
  const item = {
    id: state.nextId++,
    timestamp: new Date(message.timestamp || Date.now()).toISOString(),
    ...message
  };
  state.messages.push(item);
  while (state.messages.length > 500) state.messages.shift();
}

async function handleConsole(session, cmd, args) {
  const state = consoleState(session);
  if (cmd === "console-start") {
    const tab = await tabForSession(session);
    await ensureDebugger(tab.id);
    tabSessionByTabId.set(tab.id, session);
    await cdpCommand(tab.id, "Runtime.enable");
    try {
      await cdpCommand(tab.id, "Log.enable");
    } catch {
      // Some Chromium targets do not expose Log; Runtime.consoleAPICalled is enough.
    }
    if (args.preserve !== true) {
      state.messages = [];
      state.nextId = 1;
    }
    state.active = true;
    state.tabId = tab.id;
    return { ok: true, data: { success: true, message: "console capture started", tabId: tab.id } };
  }
  if (cmd === "console-stop") {
    state.active = false;
    if (state.tabId) await detachDebuggerIfIdle(session, state.tabId);
    return { ok: true, data: { success: true, message: "console capture stopped", count: state.messages.length } };
  }
  if (cmd === "console-list") {
    const types = Array.isArray(args.types) ? args.types.map((item) => String(item)) : [];
    const pageSize = clampNumber(args.page_size ?? args.pageSize, 1, 1000, state.messages.length || 100);
    const pageIdx = clampNumber(args.page_idx ?? args.pageIdx, 0, 100000, 0);
    const filtered = state.messages.filter((item) => types.length === 0 || types.includes(item.type));
    const messages = filtered.slice(pageIdx * pageSize, pageIdx * pageSize + pageSize);
    return { ok: true, data: { count: filtered.length, page_size: pageSize, page_idx: pageIdx, messages } };
  }
  if (cmd === "console-get") {
    const id = Number(args.id ?? args.msgid);
    const message = state.messages.find((item) => item.id === id);
    if (!message) {
      return { ok: false, error: { code: "console_message_not_found", message: `console message not found: ${id}` } };
    }
    return { ok: true, data: message };
  }
  if (cmd === "console-clear") {
    state.messages = [];
    state.nextId = 1;
    if (state.tabId && debuggerTabs.has(state.tabId)) {
      try {
        await cdpCommand(state.tabId, "Runtime.discardConsoleEntries");
      } catch {
        // Ignore clear failures; the bridge buffer is already cleared.
      }
    }
    return { ok: true, data: { success: true, message: "console buffer cleared" } };
  }
  return { ok: false, error: { code: "invalid_request", message: `unknown console cmd: ${cmd}` } };
}

function parseViewport(value, args = {}) {
  const raw = value || args.viewport;
  if (!raw && !args.width && !args.height) return null;
  if (String(raw || "").toLowerCase() === "clear") return { clear: true };
  const text = String(raw || "");
  const [sizePart, ...flags] = text.split(",");
  const parts = sizePart ? sizePart.split("x") : [];
  const width = clampNumber(args.width ?? parts[0], 1, 10000, 1280);
  const height = clampNumber(args.height ?? parts[1], 1, 10000, 720);
  const deviceScaleFactor = clampNumber(
    args.device_scale_factor ?? args.deviceScaleFactor ?? parts[2],
    0.1,
    10,
    1
  );
  const flagSet = new Set(flags.map((item) => item.trim().toLowerCase()).filter(Boolean));
  return {
    width,
    height,
    deviceScaleFactor,
    mobile: Boolean(args.mobile) || flagSet.has("mobile"),
    touch: Boolean(args.touch) || flagSet.has("touch"),
    screenOrientation: flagSet.has("landscape")
      ? { type: "landscapePrimary", angle: 90 }
      : { type: "portraitPrimary", angle: 0 }
  };
}

function networkConditionsPreset(name) {
  const normalized = String(name || "").trim().toLowerCase();
  if (!normalized || normalized === "clear" || normalized === "none") return null;
  if (normalized === "offline") return { offline: true, latency: 0, downloadThroughput: 0, uploadThroughput: 0 };
  if (normalized === "slow 3g") return { offline: false, latency: 400, downloadThroughput: 50 * 1024, uploadThroughput: 50 * 1024 };
  if (normalized === "fast 3g") return { offline: false, latency: 150, downloadThroughput: 180 * 1024, uploadThroughput: 90 * 1024 };
  if (normalized === "slow 4g") return { offline: false, latency: 80, downloadThroughput: 512 * 1024, uploadThroughput: 256 * 1024 };
  if (normalized === "fast 4g") return { offline: false, latency: 20, downloadThroughput: 4 * 1024 * 1024, uploadThroughput: 2 * 1024 * 1024 };
  return null;
}

function jsonObjectFromArg(value, label) {
  if (value === undefined || value === null || value === "") return null;
  if (typeof value === "object" && !Array.isArray(value)) return value;
  try {
    const parsed = JSON.parse(String(value));
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) return parsed;
  } catch {
    // Report a stable invalid_request below.
  }
  const error = new Error(`${label} must be a JSON object`);
  error.code = "invalid_request";
  throw error;
}

async function handleEmulation(session, args) {
  const tab = await tabForSession(session);
  await ensureDebugger(tab.id);
  tabSessionByTabId.set(tab.id, session);
  const state = emulationBySession.get(session) || { active: false, tabId: tab.id, applied: [] };
  state.tabId = tab.id;
  const applied = [];

  if (args.reset === true || args.clear === true) {
    await cdpCommand(tab.id, "Emulation.clearDeviceMetricsOverride");
    await cdpCommand(tab.id, "Emulation.clearGeolocationOverride");
    await cdpCommand(tab.id, "Emulation.setCPUThrottlingRate", { rate: 1 });
    await cdpCommand(tab.id, "Network.enable");
    await cdpCommand(tab.id, "Network.emulateNetworkConditions", {
      offline: false,
      latency: 0,
      downloadThroughput: -1,
      uploadThroughput: -1
    });
    await cdpCommand(tab.id, "Network.setExtraHTTPHeaders", { headers: {} });
    await cdpCommand(tab.id, "Emulation.setEmulatedMedia", { features: [] });
    state.active = false;
    state.applied = [];
    emulationBySession.set(session, state);
    await detachDebuggerIfIdle(session, tab.id);
    return { ok: true, data: { success: true, reset: true } };
  }

  const viewport = parseViewport(args.viewport, args);
  if (viewport?.clear) {
    await cdpCommand(tab.id, "Emulation.clearDeviceMetricsOverride");
    applied.push("viewport-clear");
  } else if (viewport) {
    await cdpCommand(tab.id, "Emulation.setDeviceMetricsOverride", viewport);
    await cdpCommand(tab.id, "Emulation.setTouchEmulationEnabled", {
      enabled: Boolean(viewport.touch),
      maxTouchPoints: viewport.touch ? 5 : 0
    });
    applied.push("viewport");
  }

  const networkName = args.network_conditions || args.networkConditions;
  if (networkName !== undefined) {
    const preset = networkConditionsPreset(networkName);
    await cdpCommand(tab.id, "Network.enable");
    await cdpCommand(tab.id, "Network.emulateNetworkConditions", preset || {
      offline: false,
      latency: 0,
      downloadThroughput: -1,
      uploadThroughput: -1
    });
    applied.push("network_conditions");
  }

  const cpuRate = args.cpu_throttling_rate ?? args.cpuThrottlingRate;
  if (cpuRate !== undefined) {
    await cdpCommand(tab.id, "Emulation.setCPUThrottlingRate", {
      rate: clampNumber(cpuRate, 1, 20, 1)
    });
    applied.push("cpu");
  }

  const geolocation = args.geolocation;
  if (typeof geolocation === "string") {
    if (geolocation.toLowerCase() === "clear") {
      await cdpCommand(tab.id, "Emulation.clearGeolocationOverride");
      applied.push("geolocation-clear");
    } else {
      const [lat, lon] = geolocation.split(",").map((part) => Number(part.trim()));
      if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
        return { ok: false, error: { code: "invalid_request", message: "geolocation must be '<latitude>,<longitude>'" } };
      }
      await cdpCommand(tab.id, "Emulation.setGeolocationOverride", {
        latitude: lat,
        longitude: lon,
        accuracy: clampNumber(args.accuracy, 1, 100000, 100)
      });
      applied.push("geolocation");
    }
  }

  const userAgent = args.user_agent ?? args.userAgent;
  if (typeof userAgent === "string") {
    await cdpCommand(tab.id, "Network.setUserAgentOverride", { userAgent });
    applied.push("user_agent");
  }

  const colorScheme = args.color_scheme ?? args.colorScheme;
  if (typeof colorScheme === "string") {
    const normalized = colorScheme.toLowerCase();
    await cdpCommand(tab.id, "Emulation.setEmulatedMedia", {
      features: normalized === "auto" || normalized === "clear"
        ? []
        : [{ name: "prefers-color-scheme", value: normalized }]
    });
    applied.push("color_scheme");
  }

  const extraHeaders = jsonObjectFromArg(args.extra_http_headers ?? args.extraHttpHeaders, "extra_http_headers");
  if (extraHeaders) {
    await cdpCommand(tab.id, "Network.enable");
    await cdpCommand(tab.id, "Network.setExtraHTTPHeaders", { headers: extraHeaders });
    applied.push("extra_http_headers");
  }

  if (applied.length === 0) {
    emulationBySession.set(session, state);
    await detachDebuggerIfIdle(session, tab.id);
    return { ok: true, data: { success: true, applied, active: state.active, tabId: tab.id } };
  }

  state.active = true;
  state.applied = Array.from(new Set([...(state.applied || []), ...applied]));
  emulationBySession.set(session, state);
  return { ok: true, data: { success: true, applied, active: state.active, tabId: tab.id } };
}

function performanceState(session) {
  const existing = performanceBySession.get(session);
  if (existing) return existing;
  const state = { active: false, tabId: null, events: [], complete: false };
  performanceBySession.set(session, state);
  return state;
}

async function waitForPredicate(predicate, timeoutMs, intervalMs = 100) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return true;
    await sleep(intervalMs);
  }
  return predicate();
}

async function handlePerformance(session, cmd, args) {
  const state = performanceState(session);
  if (cmd === "performance-start") {
    const tab = await tabForSession(session);
    await ensureDebugger(tab.id);
    tabSessionByTabId.set(tab.id, session);
    state.active = true;
    state.tabId = tab.id;
    state.events = [];
    state.complete = false;
    await cdpCommand(tab.id, "Tracing.start", {
      categories: args.categories || "devtools.timeline,v8.execute,blink.user_timing,loading,disabled-by-default-devtools.timeline",
      transferMode: "ReportEvents"
    });
    if (args.reload === true) {
      await chrome.tabs.reload(tab.id);
      await waitForTabComplete(tab.id, clampNumber(args.timeout_ms, 1000, 120000, NAVIGATION_TIMEOUT_MS));
    }
    return { ok: true, data: { success: true, message: "performance trace started", tabId: tab.id } };
  }
  if (cmd === "performance-stop") {
    if (!state.active || !state.tabId) {
      return { ok: false, error: { code: "trace_not_active", message: "performance trace is not active" } };
    }
    await cdpCommand(state.tabId, "Tracing.end");
    const complete = await waitForPredicate(
      () => state.complete === true,
      clampNumber(args.timeout_ms, 1000, 120000, 30000)
    );
    state.active = false;
    await detachDebuggerIfIdle(session, state.tabId);
    return {
      ok: true,
      data: {
        success: complete,
        complete,
        event_count: state.events.length,
        trace: { traceEvents: state.events }
      }
    };
  }
  return { ok: false, error: { code: "invalid_request", message: `unknown performance cmd: ${cmd}` } };
}

async function handleHeapSnapshot(session, args) {
  const tab = await tabForSession(session);
  await ensureDebugger(tab.id);
  tabSessionByTabId.set(tab.id, session);
  const state = { active: true, tabId: tab.id, chunks: [] };
  heapBySession.set(session, state);
  try {
    await cdpCommand(tab.id, "HeapProfiler.enable");
    await cdpCommand(tab.id, "HeapProfiler.takeHeapSnapshot", { reportProgress: false });
  } finally {
    state.active = false;
  }
  const snapshot = state.chunks.join("");
  await detachDebuggerIfIdle(session, tab.id);
  return {
    ok: true,
    data: {
      success: true,
      tabId: tab.id,
      chunk_count: state.chunks.length,
      sizeBytes: snapshot.length,
      snapshot
    }
  };
}

async function handleRawCdp(session, args) {
  if (typeof args.method !== "string" || !args.method.trim()) {
    return { ok: false, error: { code: "invalid_request", message: "raw CDP requires method" } };
  }
  const tab = await tabForSession(session);
  const wasAttached = debuggerTabs.has(tab.id);
  const params = args.params && typeof args.params === "object" && !Array.isArray(args.params) ? args.params : {};
  const result = await cdpCommand(tab.id, args.method, params);
  if (!wasAttached) await detachDebuggerIfIdle(session, tab.id);
  return { ok: true, data: { success: true, method: args.method, result } };
}

async function handleDevtools(session, args) {
  const cmd = args.cmd || args.command;
  if (!cmd) {
    return { ok: false, error: { code: "invalid_request", message: "devtools requires cmd" } };
  }
  if (String(cmd).startsWith("console-")) return handleConsole(session, String(cmd), args);
  if (String(cmd).startsWith("network-")) {
    const networkCmd = String(cmd).slice("network-".length);
    return handleNetwork(session, { ...args, cmd: networkCmd });
  }
  if (cmd === "emulate") return handleEmulation(session, args);
  if (cmd === "performance-start" || cmd === "performance-stop") return handlePerformance(session, String(cmd), args);
  if (cmd === "heap-snapshot") return handleHeapSnapshot(session, args);
  return { ok: false, error: { code: "invalid_request", message: `unknown devtools cmd: ${cmd}` } };
}

function pointerProfile(speed, args = {}) {
  const profiles = {
    fast: { moveMin: 90, moveMax: 240, holdMin: 25, holdMax: 70, jitter: 1.0, spacing: 8, typingMin: 8, typingMax: 25 },
    normal: { moveMin: 180, moveMax: 520, holdMin: 45, holdMax: 120, jitter: 2.0, spacing: 14, typingMin: 20, typingMax: 70 },
    slow: { moveMin: 450, moveMax: 1100, holdMin: 90, holdMax: 220, jitter: 3.0, spacing: 24, typingMin: 50, typingMax: 150 },
    custom: { moveMin: 180, moveMax: 650, holdMin: 45, holdMax: 120, jitter: 2.0, spacing: 14, typingMin: 20, typingMax: 90 }
  };
  const base = { ...(profiles[speed] || profiles.normal) };
  if (speed === "custom" && args.pointer_custom) {
    base.moveMin = clampNumber(args.pointer_custom.move_duration_ms_min, 0, 300000, base.moveMin);
    base.moveMax = clampNumber(args.pointer_custom.move_duration_ms_max, base.moveMin, 300000, base.moveMax);
    base.holdMin = clampNumber(args.pointer_custom.click_hold_ms_min, 0, 300000, base.holdMin);
    base.holdMax = clampNumber(args.pointer_custom.click_hold_ms_max, base.holdMin, 300000, base.holdMax);
    base.typingMin = clampNumber(args.pointer_custom.typing_delay_ms_min, 0, 300000, base.typingMin);
    base.typingMax = clampNumber(args.pointer_custom.typing_delay_ms_max, base.typingMin, 300000, base.typingMax);
    base.jitter = clampNumber(args.pointer_custom.jitter_px, 0, 50, base.jitter);
    base.maxPathPoints = clampNumber(args.pointer_custom.max_path_points, 6, 300, 80);
  }
  if (args.jitter !== undefined) base.jitter = clampNumber(args.jitter, 0, 50, base.jitter);
  return base;
}

function clampNumber(value, min, max, fallback) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, n));
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function typingDelayRange(args = {}, profile = {}) {
  const configured = Array.isArray(args.delay_ms) ? args.delay_ms : null;
  const min = configured ? configured[0] : profile.typingMin;
  const max = configured ? configured[1] : profile.typingMax;
  const low = clampNumber(min, 0, 300000, profile.typingMin || 0);
  const high = clampNumber(max, low, 300000, profile.typingMax || low);
  return { min: low, max: high };
}

function randomDelay({ min, max }) {
  return Math.round(min + Math.random() * Math.max(0, max - min));
}

function pointerDebugEnabled(args = {}) {
  return args.debug_visualization === true || args.debug_path === true;
}

async function showPointerPath(session, points, args = {}) {
  if (!pointerDebugEnabled(args) || !Array.isArray(points) || points.length === 0) return;
  const durationMs = clampNumber(args.debug_duration_ms, 500, 15000, 2500);
  await sendSessionPageCommand(session, "show_pointer_path", {
    duration_ms: durationMs,
    points: points.map((point) => ({
      x: Number(point.x),
      y: Number(point.y)
    }))
  });
}

async function allowBridgeEvents(session, durationMs) {
  try {
    await sendSessionPageCommand(session, "allow_bridge_events", {
      duration_ms: Math.max(0, Math.min(300000, Math.round(durationMs)))
    });
  } catch {
    // The action below returns the real page/script error if the page cannot receive commands.
  }
}

function pointInRect(rect) {
  const width = Math.max(1, Number(rect.width || 1));
  const height = Math.max(1, Number(rect.height || 1));
  const marginX = Math.min(Math.max(3, width * 0.1), width / 2);
  const marginY = Math.min(Math.max(3, height * 0.1), height / 2);
  const left = Number(rect.x ?? rect.left ?? 0);
  const top = Number(rect.y ?? rect.top ?? 0);
  return {
    x: left + marginX + Math.random() * Math.max(1, width - marginX * 2),
    y: top + marginY + Math.random() * Math.max(1, height - marginY * 2)
  };
}

function generatePointerPath(from, to, profile, durationMs) {
  const distance = Math.hypot(to.x - from.x, to.y - from.y);
  const count = Math.max(6, Math.min(profile.maxPathPoints || 80, Math.round(distance / 18)));
  const c1 = {
    x: from.x + (to.x - from.x) * 0.35 + (Math.random() - 0.5) * profile.jitter * 8,
    y: from.y + (to.y - from.y) * 0.15 + (Math.random() - 0.5) * profile.jitter * 8
  };
  const c2 = {
    x: from.x + (to.x - from.x) * 0.7 + (Math.random() - 0.5) * profile.jitter * 8,
    y: from.y + (to.y - from.y) * 0.85 + (Math.random() - 0.5) * profile.jitter * 8
  };
  const points = [];
  for (let i = 1; i <= count; i += 1) {
    const t0 = i / count;
    const t = t0 < 0.5 ? 2 * t0 * t0 : 1 - Math.pow(-2 * t0 + 2, 2) / 2;
    const inv = 1 - t;
    points.push({
      x: inv ** 3 * from.x + 3 * inv ** 2 * t * c1.x + 3 * inv * t ** 2 * c2.x + t ** 3 * to.x,
      y: inv ** 3 * from.y + 3 * inv ** 2 * t * c1.y + 3 * inv * t ** 2 * c2.y + t ** 3 * to.y,
      delay: Math.max(1, Math.round(durationMs / count))
    });
  }
  return points;
}

async function resolvePoint(session, args, role = "target") {
  if (Number.isFinite(Number(args.x)) && Number.isFinite(Number(args.y))) {
    return { x: Number(args.x), y: Number(args.y), rect: null };
  }
  const textTarget = role === "target" && (args.target_text || args.targetText)
    ? { text: args.target_text || args.targetText }
    : null;
  const locatorTarget = role === "target"
    ? args.locator
    : (args[`${role}_locator`] || args[`${role}Locator`]);
  const target = args[role] || locatorTarget || args.target || args.selector || textTarget;
  if (!target) {
    const error = new Error(`${role} target is required`);
    error.code = "invalid_request";
    throw error;
  }
  const response = await sendSessionPageCommand(session, "resolve_target", { target });
  if (!response?.ok) {
    const error = new Error(response?.error?.message || "target resolution failed");
    error.code = response?.error?.code || "element_not_found";
    error.diagnostics = response?.error?.diagnostics || null;
    throw error;
  }
  const rect = response.data?.rect || {};
  const targetPoint = rect.width && rect.height ? pointInRect(rect) : { x: response.data.x, y: response.data.y };
  return { ...targetPoint, rect, element: response.data?.element || null, viewport: response.data?.viewport || null };
}

function diagnosticsForBridgeError(error, fallbackCode = "extension_error", extra = {}) {
  if (error?.diagnostics && typeof error.diagnostics === "object") {
    return { ...error.diagnostics, ...extra };
  }
  return {
    action: extra.action || null,
    session: extra.session || null,
    timestamp_ms: Date.now(),
    source: "service_worker",
    code: error?.code || fallbackCode,
    suggested_next: extra.suggested_next || "Inspect the returned error, read_page, and retry with a narrower locator or assertion."
  };
}

function bridgeError(error, fallbackCode = "extension_error", extra = {}) {
  return {
    ok: false,
    error: {
      code: error?.code || fallbackCode,
      message: error instanceof Error ? error.message : String(error),
      diagnostics: diagnosticsForBridgeError(error, fallbackCode, extra)
    }
  };
}

async function dispatchMousePath(tabId, points) {
  for (const point of points) {
    await cdpCommand(tabId, "Input.dispatchMouseEvent", {
      type: "mouseMoved",
      x: point.x,
      y: point.y,
      button: "none",
      buttons: point.buttons || 0
    });
    await sleep(point.delay);
  }
}

async function cdpPointerAction(session, args, kind) {
  const tab = await tabForSession(session);
  const state = sessionState(session);
  const speed = args.speed || "normal";
  const profile = pointerProfile(speed, args);
  const target = await resolvePoint(session, args);
  const from = state.lastPointer || { x: Math.max(8, target.x - 80), y: Math.max(8, target.y - 40) };
  const durationMs = clampNumber(args.duration_ms, profile.moveMin, profile.moveMax, Math.round((profile.moveMin + profile.moveMax) / 2));
  const holdMs = clampNumber(args.hold_ms, profile.holdMin, profile.holdMax, Math.round((profile.holdMin + profile.holdMax) / 2));
  const path = generatePointerPath(from, target, profile, durationMs);
  await showPointerPath(session, path, args);
  await allowBridgeEvents(session, durationMs + holdMs + 500);
  await dispatchMousePath(tab.id, path);
  state.lastPointer = { x: target.x, y: target.y };
  scheduleSessionRegistryPersist();

  if (kind === "hover") {
    return {
      ok: true,
      data: { success: true, mode: "cdp", speed, target, path_points: path.length, duration_ms: durationMs }
    };
  }

  await cdpCommand(tab.id, "Input.dispatchMouseEvent", {
    type: "mousePressed",
    x: target.x,
    y: target.y,
    button: args.button || "left",
    buttons: 1,
    clickCount: 1
  });
  await sleep(holdMs);
  await cdpCommand(tab.id, "Input.dispatchMouseEvent", {
    type: "mouseReleased",
    x: target.x,
    y: target.y,
    button: args.button || "left",
    buttons: 0,
    clickCount: 1
  });
  return {
    ok: true,
    data: { success: true, mode: "cdp", speed, target, path_points: path.length, duration_ms: durationMs, hold_ms: holdMs }
  };
}

async function cdpDrag(session, args) {
  const tab = await tabForSession(session);
  const state = sessionState(session);
  const speed = args.speed || "normal";
  const profile = pointerProfile(speed, args);
  const from = await resolvePoint(session, { target: args.from });
  let to;
  if (args.to) {
    to = await resolvePoint(session, { target: args.to });
  } else if (Array.isArray(args.offset) && args.offset.length >= 2) {
    to = { x: from.x + Number(args.offset[0]), y: from.y + Number(args.offset[1]) };
  } else {
    const error = new Error("drag requires to or offset");
    error.code = "invalid_request";
    throw error;
  }

  const start = state.lastPointer || { x: Math.max(8, from.x - 80), y: Math.max(8, from.y - 40) };
  const approachMs = clampNumber(args.duration_ms, profile.moveMin, profile.moveMax, Math.round(profile.moveMin * 1.2));
  const dragMs = clampNumber(args.duration_ms, profile.moveMin, profile.moveMax, Math.round(profile.moveMax * 0.9));
  const approach = generatePointerPath(start, from, profile, approachMs);
  const dragPath = generatePointerPath(from, to, profile, dragMs);
  await showPointerPath(session, approach.concat(dragPath), args);
  await allowBridgeEvents(session, approachMs + dragMs + profile.holdMax + 500);
  await dispatchMousePath(tab.id, approach);
  await cdpCommand(tab.id, "Input.dispatchMouseEvent", { type: "mousePressed", x: from.x, y: from.y, button: "left", buttons: 1 });
  await sleep(clampNumber(args.hold_ms, profile.holdMin, profile.holdMax, profile.holdMax));
  await dispatchMousePath(tab.id, dragPath.map((point) => ({ ...point, buttons: 1 })));
  await cdpCommand(tab.id, "Input.dispatchMouseEvent", { type: "mouseReleased", x: to.x, y: to.y, button: "left", buttons: 0 });
  state.lastPointer = { x: to.x, y: to.y };
  scheduleSessionRegistryPersist();
  return {
    ok: true,
    data: { success: true, mode: "cdp", speed, from, to, path_points: approach.length + dragPath.length, duration_ms: approachMs + dragMs }
  };
}

async function cdpScroll(session, args) {
  const tab = await tabForSession(session);
  const state = sessionState(session);
  let target = state.lastPointer || { x: 200, y: 200 };
  if (args.target || args.selector || args.target_text || args.targetText || args.locator) {
    target = await resolvePoint(session, args);
  }
  await allowBridgeEvents(session, 500);
  await cdpCommand(tab.id, "Input.dispatchMouseEvent", {
    type: "mouseWheel",
    x: target.x,
    y: target.y,
    deltaX: Number(args.delta_x || 0),
    deltaY: Number(args.delta_y || 0)
  });
  state.lastPointer = { x: target.x, y: target.y };
  scheduleSessionRegistryPersist();
  return { ok: true, data: { success: true, mode: "cdp", target, delta_x: Number(args.delta_x || 0), delta_y: Number(args.delta_y || 0) } };
}

async function cdpEvaluate(session, args) {
  if (typeof args.code !== "string" || !args.code.trim()) {
    return { ok: false, error: { code: "invalid_request", message: "evaluate requires code" } };
  }
  const tab = await tabForSession(session);
  const wasAttached = debuggerTabs.has(tab.id);
  const result = await cdpCommand(tab.id, "Runtime.evaluate", {
    expression: args.code,
    awaitPromise: true,
    returnByValue: true
  });
  if (!wasAttached) await detachDebuggerIfIdle(session, tab.id);
  if (result?.exceptionDetails) {
    return {
      ok: false,
      error: {
        code: "evaluation_error",
        message: result.exceptionDetails.text || result.exceptionDetails.exception?.description || "evaluation failed"
      }
    };
  }
  const remote = result?.result || {};
  const value = Object.prototype.hasOwnProperty.call(remote, "value") ? remote.value : remote.description;
  return {
    ok: true,
    data: {
      type: remote.subtype || remote.type || (value === null ? "null" : typeof value),
      value: typeof value === "string" ? value : JSON.stringify(value)
    }
  };
}

async function cdpType(session, args) {
  const speed = args.speed || "normal";
  const profile = pointerProfile(speed, args);
  if (args.target || args.selector || args.target_text || args.targetText || args.locator) {
    const clickResult = await cdpPointerAction(session, args, "click");
    if (!clickResult.ok) return clickResult;
  }
  if (args.clear) {
    await sendSessionPageCommand(session, "type", { ...args, text: "", clear: true, keys: [] });
  }
  const text = String(args.text || "");
  const delay = typingDelayRange(args, profile);
  const keys = Array.isArray(args.keys) ? args.keys.slice() : [];
  if (args.submit) keys.push("Enter");
  await allowBridgeEvents(session, (Array.from(text).length + keys.length + 1) * delay.max + 500);
  if (text) {
    const tab = await tabForSession(session);
    const chunks = Array.from(text);
    for (let index = 0; index < chunks.length; index += 1) {
      await cdpCommand(tab.id, "Input.insertText", { text: chunks[index] });
      if (index + 1 < chunks.length) await sleep(randomDelay(delay));
    }
  }
  const tab = await tabForSession(session);
  for (const key of keys) {
    await cdpCommand(tab.id, "Input.dispatchKeyEvent", { type: "keyDown", key });
    await cdpCommand(tab.id, "Input.dispatchKeyEvent", { type: "keyUp", key });
    await sleep(randomDelay(delay));
  }
  return { ok: true, data: { success: true, mode: "cdp", speed, typed_chars: Array.from(text).length, keys, typing_delay_ms: delay } };
}

async function pointerAction(session, actionName, args) {
  const mode = args.mode || "auto";
  if (mode === "dom") {
    return sendSessionPageCommand(session, actionName, args);
  }
  if (mode === "os") {
    return { ok: false, error: { code: "os_pointer_disabled", message: "OS pointer mode is disabled" } };
  }
  try {
    if (args.force_cdp_unavailable === true) {
      const forced = new Error("forced CDP unavailable for diagnostics");
      forced.code = "cdp_unavailable";
      throw forced;
    }
    if (actionName === "click") return cdpPointerAction(session, args, "click");
    if (actionName === "hover") return cdpPointerAction(session, args, "hover");
    if (actionName === "drag") return cdpDrag(session, args);
    if (actionName === "scroll") return cdpScroll(session, args);
    if (actionName === "type") return cdpType(session, args);
  } catch (error) {
    if (mode === "auto") {
      const fallback = await sendSessionPageCommand(session, actionName, args);
      if (fallback?.ok) {
        fallback.data = { ...(fallback.data || {}), mode: fallback.data?.mode || "dom", fallback_reason: error?.code || "cdp_unavailable" };
      }
      return fallback;
    }
    return bridgeError(error, "cdp_unavailable");
  }
  return sendSessionPageCommand(session, actionName, args);
}

function base64ToBytes(data) {
  const binary = atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

async function blobToBase64(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  return bytesToBase64(bytes);
}

function hasTargetedScreenshotArgs(args = {}) {
  return Boolean(args.target || args.selector || args.target_text || args.targetText || args.locator);
}

async function cropScreenshotBase64(data, rect, viewport) {
  if (!rect || !viewport || typeof createImageBitmap !== "function" || typeof OffscreenCanvas === "undefined") {
    return { data, crop_applied: false, sizeBytes: Math.floor(data.length * 3 / 4), source_width: null, source_height: null };
  }
  const blob = new Blob([base64ToBytes(data)], { type: "image/png" });
  const bitmap = await createImageBitmap(blob);
  const ratioX = bitmap.width / Math.max(1, Number(viewport.width || bitmap.width));
  const ratioY = bitmap.height / Math.max(1, Number(viewport.height || bitmap.height));
  const sx = Math.max(0, Math.round(Number(rect.x || 0) * ratioX));
  const sy = Math.max(0, Math.round(Number(rect.y || 0) * ratioY));
  const sw = Math.max(1, Math.min(bitmap.width - sx, Math.round(Number(rect.width || 1) * ratioX)));
  const sh = Math.max(1, Math.min(bitmap.height - sy, Math.round(Number(rect.height || 1) * ratioY)));
  const canvas = new OffscreenCanvas(sw, sh);
  const context = canvas.getContext("2d");
  context.drawImage(bitmap, sx, sy, sw, sh, 0, 0, sw, sh);
  const cropped = await canvas.convertToBlob({ type: "image/png" });
  const croppedData = await blobToBase64(cropped);
  return {
    data: croppedData,
    crop_applied: true,
    sizeBytes: cropped.size,
    source_width: bitmap.width,
    source_height: bitmap.height
  };
}

function mimeFromUrl(url, fallback = "application/octet-stream") {
  const lower = String(url || "").split("?")[0].toLowerCase();
  if (/\.(png)$/.test(lower)) return "image/png";
  if (/\.(jpe?g)$/.test(lower)) return "image/jpeg";
  if (/\.(gif)$/.test(lower)) return "image/gif";
  if (/\.(webp)$/.test(lower)) return "image/webp";
  if (/\.(svg)$/.test(lower)) return "image/svg+xml";
  if (/\.pdf$/.test(lower)) return "application/pdf";
  if (/\.json$/.test(lower)) return "application/json";
  if (/\.(log|txt)$/.test(lower)) return "text/plain";
  return fallback;
}

async function attachmentUrlForArgs(session, args = {}) {
  const direct = args.attachment_url || args.attachmentUrl || args.url;
  if (direct) return { url: direct, ref: null, info: null };
  const ref = args.attachment_ref || args.attachmentRef;
  if (!ref) return null;
  const response = await sendSessionPageCommand(session, "attachment_info", { attachment_ref: ref });
  if (!response?.ok) {
    const error = new Error(response?.error?.message || "attachment resolution failed");
    error.code = response?.error?.code || "element_ref_stale";
    error.diagnostics = response?.error?.diagnostics || null;
    throw error;
  }
  return { url: response.data?.url, ref, info: response.data };
}

async function handleAttachmentExport(session, args) {
  const attachment = await attachmentUrlForArgs(session, args);
  if (!attachment?.url) return null;
  const response = await fetch(attachment.url, { credentials: "include" });
  if (!response.ok) {
    const error = new Error(`attachment fetch failed: HTTP ${response.status}`);
    error.code = "attachment_fetch_failed";
    throw error;
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  const data = bytesToBase64(bytes);
  const mimeType = response.headers.get("content-type") || attachment.info?.mime_hint || mimeFromUrl(attachment.url);
  return {
    ok: true,
    data: {
      path: args.output || `/tmp/ace-browser-bridge-attachments/${Date.now()}`,
      mimeType,
      sizeBytes: bytes.length,
      data,
      attachment: {
        ref: attachment.ref,
        url: attachment.url,
        kind: attachment.info?.kind || null,
        name: attachment.info?.name || null
      }
    }
  };
}

async function handleScreenshot(session, args) {
  const started = Date.now();
  let tab = null;
  try {
    const attachment = await handleAttachmentExport(session, args);
    if (attachment) return attachment;
    tab = await tabForSession(session);
    let target = null;
    if (hasTargetedScreenshotArgs(args)) {
      target = await resolvePoint(session, args);
    }
    await postPluginLog("info", "screenshot_start", {
      session,
      tab_id: tab.id,
      window_id: tab.windowId,
      has_output_path: Boolean(args.output),
      targeted: Boolean(target)
    });
    const dataUrl = await chrome.tabs.captureVisibleTab(tab.windowId, { format: "png" });
    const comma = dataUrl.indexOf(",");
    let data = comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl;
    let sizeBytes = Math.floor(data.length * 3 / 4);
    let crop = null;
    if (target?.rect) {
      const viewport = target.viewport || {};
      const cropped = await cropScreenshotBase64(data, target.rect, viewport);
      data = cropped.data;
      sizeBytes = cropped.sizeBytes;
      crop = {
        applied: cropped.crop_applied,
        rect: target.rect,
        viewport,
        device_pixel_ratio: viewport.device_pixel_ratio || null,
        source_width: cropped.source_width,
        source_height: cropped.source_height
      };
    }
    const path = args.output || `/tmp/ace-browser-bridge-screenshots/${Date.now()}.png`;
    await postPluginLog("info", "screenshot_finish", {
      session,
      tab_id: tab.id,
      window_id: tab.windowId,
      size_bytes: sizeBytes,
      data_length: data.length,
      duration_ms: Date.now() - started,
      output_path: path,
      crop_applied: crop?.applied || false
    });
    return {
      ok: true,
      data: {
        path,
        mimeType: "image/png",
        sizeBytes,
        data,
        crop,
        element: target?.element || null
      }
    };
  } catch (error) {
    await postPluginLog("error", "screenshot_error", {
      session,
      tab_id: tab?.id || null,
      window_id: tab?.windowId || null,
      error_code: error?.code || "extension_error",
      message: error instanceof Error ? error.message : String(error),
      duration_ms: Date.now() - started
    });
    throw error;
  }
}

async function handleSavePdf(session, args) {
  const tab = await tabForSession(session);
  const wasAttached = debuggerTabs.has(tab.id);
  const printOptions = {
    printBackground: args.print_background !== false,
    landscape: Boolean(args.landscape),
    scale: Number(args.scale || 1.0)
  };
  if (args.paper_format) printOptions.paperWidth = args.paper_format === "letter" ? 8.5 : 8.27;
  if (args.paper_format) printOptions.paperHeight = args.paper_format === "letter" ? 11 : 11.69;
  const result = await cdpCommand(tab.id, "Page.printToPDF", printOptions);
  if (!wasAttached) await detachDebuggerIfIdle(session, tab.id);
  const fileName = args.file_name || `ace-browser-bridge-${Date.now()}.pdf`;
  return {
    ok: true,
    data: {
      path: `/tmp/ace-browser-bridge-pdfs/${fileName}`,
      mimeType: "application/pdf",
      sizeBytes: Math.floor((result.data || "").length * 3 / 4),
      pageTitle: tab.title || "",
      data: result.data || ""
    }
  };
}

chrome.debugger.onEvent.addListener((source, method, params) => {
  const tabId = source.tabId;
  const session = tabSessionByTabId.get(tabId);
  if (!session) return;
  const consoleCapture = consoleBySession.get(session);
  if (consoleCapture?.active && method === "Runtime.consoleAPICalled") {
    appendConsoleMessage(session, {
      type: params.type || "log",
      text: (params.args || []).map(remoteObjectValue).join(" "),
      args: (params.args || []).map((arg) => ({
        type: arg.type,
        subtype: arg.subtype || null,
        value: remoteObjectValue(arg),
        description: arg.description || null
      })),
      stackTrace: params.stackTrace || null,
      stack_trace: params.stackTrace || null,
      executionContextId: params.executionContextId || null,
      execution_context_id: params.executionContextId || null
    });
  } else if (consoleCapture?.active && method === "Log.entryAdded") {
    const entry = params.entry || {};
    appendConsoleMessage(session, {
      type: entry.level || "log",
      text: entry.text || "",
      source: entry.source || null,
      url: entry.url || null,
      lineNumber: entry.lineNumber || null,
      line_number: entry.lineNumber || null,
      stackTrace: entry.stackTrace || null,
      stack_trace: entry.stackTrace || null,
      networkRequestId: entry.networkRequestId || null,
      network_request_id: entry.networkRequestId || null
    });
  }

  const state = networkBySession.get(session);
  if (state?.active && method === "Network.requestWillBeSent") {
    const request = {
      requestId: params.requestId,
      request_id: params.requestId,
      loaderId: params.loaderId || null,
      loader_id: params.loaderId || null,
      documentURL: params.documentURL || null,
      document_url: params.documentURL || null,
      url: params.request?.url || "",
      method: params.request?.method || "",
      resourceType: params.type || null,
      resource_type: params.type || null,
      requestHeaders: params.request?.headers || {},
      request_headers: params.request?.headers || {},
      postData: params.request?.postData || null,
      post_data: params.request?.postData || null,
      initiator: params.initiator || null,
      seenAtMs: Date.now(),
      seen_at_ms: Date.now(),
      wallTime: params.wallTime || null,
      wall_time: params.wallTime || null,
      timestamp: params.timestamp || null,
      status: null,
      mimeType: null,
      mime_type: null,
      completed: false
    };
    state.requests.push(request);
    while (state.requests.length > 200) state.requests.shift();
  } else if (state?.active && method === "Network.responseReceived") {
    const request = state.requests.find((item) => item.requestId === params.requestId);
    if (request) {
      request.status = params.response?.status || null;
      request.statusText = params.response?.statusText || null;
      request.status_text = request.statusText;
      request.mimeType = params.response?.mimeType || null;
      request.mime_type = request.mimeType;
      request.url = params.response?.url || request.url;
      request.responseHeaders = params.response?.headers || {};
      request.response_headers = request.responseHeaders;
      request.remoteIPAddress = params.response?.remoteIPAddress || null;
      request.remote_ip_address = request.remoteIPAddress;
      request.fromDiskCache = params.response?.fromDiskCache || false;
      request.from_disk_cache = request.fromDiskCache;
      request.fromServiceWorker = params.response?.fromServiceWorker || false;
      request.from_service_worker = request.fromServiceWorker;
      request.timing = params.response?.timing || null;
      request.securityState = params.response?.securityState || null;
      request.security_state = request.securityState;
      request.resourceType = params.type || request.resourceType || null;
      request.resource_type = request.resourceType;
    }
  } else if (state?.active && method === "Network.loadingFinished") {
    const request = state.requests.find((item) => item.requestId === params.requestId);
    if (request) {
      request.completed = true;
      request.completedAtMs = Date.now();
      request.completed_at_ms = request.completedAtMs;
      request.encodedDataLength = params.encodedDataLength || 0;
      request.encoded_data_length = request.encodedDataLength;
    }
  } else if (state?.active && method === "Network.loadingFailed") {
    const request = state.requests.find((item) => item.requestId === params.requestId);
    if (request) {
      request.completed = false;
      request.failed = true;
      request.completedAtMs = Date.now();
      request.completed_at_ms = request.completedAtMs;
      request.errorText = params.errorText || "";
      request.error_text = request.errorText;
      request.canceled = params.canceled === true;
    }
  }

  const perf = performanceBySession.get(session);
  if (perf?.active && method === "Tracing.dataCollected") {
    const values = Array.isArray(params.value) ? params.value : [];
    perf.events.push(...values);
  } else if (perf?.active && method === "Tracing.tracingComplete") {
    perf.complete = true;
  }

  const heap = heapBySession.get(session);
  if (heap?.active && method === "HeapProfiler.addHeapSnapshotChunk") {
    heap.chunks.push(params.chunk || "");
  }
});

chrome.debugger.onDetach.addListener((source) => {
  if (source.tabId) {
    const session = tabSessionByTabId.get(source.tabId);
    debuggerTabs.delete(source.tabId);
    tabSessionByTabId.delete(source.tabId);
    if (session) {
      for (const map of [consoleBySession, networkBySession, emulationBySession, performanceBySession, heapBySession]) {
        const state = map.get(session);
        if (state?.tabId === source.tabId) state.active = false;
      }
    }
  }
});

chrome.tabs.onRemoved.addListener((tabId) => {
  let changed = false;
  for (const state of sessions.values()) {
    if (state.tabId === tabId) {
      state.tabId = null;
      state.groupId = null;
      changed = true;
    }
  }
  debuggerTabs.delete(tabId);
  tabSessionByTabId.delete(tabId);
  for (const map of [consoleBySession, networkBySession, emulationBySession, performanceBySession, heapBySession]) {
    for (const state of map.values()) {
      if (state?.tabId === tabId) state.active = false;
    }
  }
  if (changed) scheduleSessionRegistryPersist();
});

// ---- 验证条件:网络类在 service worker 侧求值(CDP 抓包状态只在这里) ----
function isNetworkCondition(condition) {
  return condition === "network_idle" || condition === "request_finished" || condition === "request_completed";
}

function networkInFlightCount(session) {
  const state = networkBySession.get(session);
  if (!state) return null;
  return state.requests.filter((r) => !r.completed && !r.failed).length;
}

function statusMatchesClass(status, cls) {
  const normalized = String(cls || "").toLowerCase();
  if (!normalized || normalized === "any" || normalized === "*") return true;
  const s = Number(status);
  if (!Number.isFinite(s)) return false;
  if (normalized === "2xx") return s >= 200 && s < 300;
  if (normalized === "3xx") return s >= 300 && s < 400;
  if (normalized === "4xx") return s >= 400 && s < 500;
  if (normalized === "5xx") return s >= 500 && s < 600;
  const exact = Number(normalized);
  return Number.isFinite(exact) && s === exact;
}

function requestWindowStart(args = {}) {
  const raw = args.after_ms ?? args.afterMs ?? args.since_ms ?? args.sinceMs ??
    args.action_started_at_ms ?? args.actionStartedAtMs;
  const value = Number(raw || 0);
  return Number.isFinite(value) && value > 0 ? value : 0;
}

function requestMatchesStatic(request, args = {}) {
  const needle = String(args.url || "");
  if (needle && !String(request.url || "").includes(needle)) return false;
  const requestId = args.request_id || args.requestId;
  if (requestId && String(request.request_id || request.requestId || "") !== String(requestId)) return false;
  if (args.url_regex || args.urlRegex) {
    try {
      const regex = new RegExp(String(args.url_regex || args.urlRegex));
      if (!regex.test(String(request.url || ""))) return false;
    } catch {
      return false;
    }
  }
  const method = args.method ? String(args.method).toUpperCase() : "";
  if (method && String(request.method || "").toUpperCase() !== method) return false;
  const start = requestWindowStart(args);
  if (start && Number(request.seen_at_ms || request.seenAtMs || 0) < start) return false;
  if (args.status != null && Number(request.status) !== Number(args.status)) return false;
  if (!statusMatchesClass(request.status, args.status_class ?? args.statusClass)) return false;
  const requestBodyNeedle = args.request_body_contains ?? args.requestBodyContains;
  if (requestBodyNeedle != null && !String(request.post_data || request.postData || "").includes(String(requestBodyNeedle))) {
    return false;
  }
  return true;
}

async function responseBodyContains(session, request, needle) {
  if (needle == null || needle === "") return true;
  if (request.response_body_unavailable) return false;
  if (request.response_body_text == null) {
    const state = networkBySession.get(session);
    try {
      if (!state?.tabId) return false;
      const body = await cdpCommand(state.tabId, "Network.getResponseBody", { requestId: request.requestId || request.request_id });
      const text = body?.base64Encoded ? atob(body.body || "") : String(body?.body || "");
      request.response_body_text = text;
      request.response_body_base64_encoded = body?.base64Encoded === true;
    } catch (error) {
      request.response_body_unavailable = true;
      request.response_body_error = error instanceof Error ? error.message : String(error);
      return false;
    }
  }
  return String(request.response_body_text || "").includes(String(needle));
}

async function requestMatches(session, request, args = {}) {
  if (!request.completed || !requestMatchesStatic(request, args)) return false;
  return responseBodyContains(session, request, args.response_body_contains ?? args.responseBodyContains);
}

async function latestMatchingCompletedRequest(session, requests, args = {}) {
  for (let i = requests.length - 1; i >= 0; i -= 1) {
    if (await requestMatches(session, requests[i], args)) return requests[i];
  }
  return null;
}

function observedNetworkFailure(state, args = {}) {
  const candidates = state.requests.filter((r) => {
    const needle = String(args.url || "");
    if (needle && !String(r.url || "").includes(needle)) return false;
    const start = requestWindowStart(args);
    if (start && Number(r.seen_at_ms || r.seenAtMs || 0) < start) return false;
    return r.completed === true || r.failed === true;
  });
  const latest = candidates[candidates.length - 1] || null;
  return latest
    ? {
        matched: false,
        url: latest.url,
        method: latest.method,
        status: latest.status,
        failed: latest.failed === true,
        seen_at_ms: latest.seen_at_ms || latest.seenAtMs || null,
        completed_at_ms: latest.completed_at_ms || latest.completedAtMs || null
      }
    : { matched: false };
}

// 网络类条件的 SW 侧轮询器。抓包未 active 不伪造成功,返回 network_capture_required。
async function waitForNetworkCondition(session, args, failCode = "wait_timeout") {
  const timeoutMs = Math.max(1, Number(args.timeout_ms || 5000));
  const quietMs = Math.max(100, Number(args.quiet_ms || 500));
  const started = Date.now();
  const state = networkBySession.get(session);
  if (!state || !state.active) {
    return {
      ok: false,
      error: {
        code: "network_capture_required",
        message: "network condition requires active network capture; run a network start first",
        diagnostics: diagnosticsForBridgeError(new Error("network capture required"), "network_capture_required", {
          action: args.condition,
          session,
          suggested_next: "Run network --cmd start for this session before network assertions."
        })
      }
    };
  }
  const condition = args.condition;
  let idleSince = null;
  for (;;) {
    if (condition === "request_completed") {
      const match = await latestMatchingCompletedRequest(session, state.requests, args);
      if (match) {
        return {
          ok: true,
          data: {
            success: true,
            matched: condition,
            observed: {
              url: match.url,
              method: match.method,
              status: match.status,
              request_id: match.request_id || match.requestId,
              seen_at_ms: match.seen_at_ms || match.seenAtMs || null,
              completed_at_ms: match.completed_at_ms || match.completedAtMs || null
            },
            elapsed_ms: Date.now() - started
          }
        };
      }
    } else {
      const inFlight = networkInFlightCount(session);
      if (inFlight === 0) {
        if (idleSince === null) idleSince = Date.now();
        if (Date.now() - idleSince >= quietMs) {
          return { ok: true, data: { success: true, matched: condition, observed: { in_flight: 0 }, elapsed_ms: Date.now() - started } };
        }
      } else {
        idleSince = null;
      }
    }
    if (Date.now() - started >= timeoutMs) {
      let observed;
      if (condition === "request_completed") {
        observed = observedNetworkFailure(state, args);
      } else {
        observed = { in_flight: networkInFlightCount(session) };
      }
      return {
        ok: false,
        error: {
          code: failCode,
          message: `Timed out waiting for ${condition}`,
          observed,
          diagnostics: diagnosticsForBridgeError(new Error(`Timed out waiting for ${condition}`), failCode, {
            action: condition,
            session,
            suggested_next: "Check network capture state, method/status/body filters, and retry with a narrower request window."
          })
        }
      };
    }
    await sleep(120);
  }
}

// wait / assert 入口:网络类走 SW,DOM 类转 content script(共用 content 的求值器)。
async function handleWait(session, args) {
  if (isNetworkCondition(args.condition)) return waitForNetworkCondition(session, args, "wait_timeout");
  return sendSessionPageCommand(session, "wait", args);
}

async function handleAssert(session, args) {
  if (isNetworkCondition(args.condition)) return waitForNetworkCondition(session, args, "assertion_failed");
  return sendSessionPageCommand(session, "assert", args);
}

// 动作内联 expect:动作主体成功后就地校验期望,成功折进 verified+observed,失败把整体翻成 expectation_failed。
async function applyExpectation(session, args, result, actionStartedAtMs = null) {
  if (!result?.ok || !args?.expect || typeof args.expect !== "object") return result;
  const expectArgs = { ...args.expect, timeout_ms: args.expect.timeout_ms || 5000 };
  if (isNetworkCondition(expectArgs.condition) &&
      expectArgs.after_ms == null &&
      expectArgs.afterMs == null &&
      expectArgs.since_ms == null &&
      expectArgs.sinceMs == null &&
      expectArgs.action_started_at_ms == null &&
      expectArgs.actionStartedAtMs == null &&
      actionStartedAtMs) {
    expectArgs.action_started_at_ms = actionStartedAtMs;
  }
  const verdict = isNetworkCondition(expectArgs.condition)
    ? await waitForNetworkCondition(session, expectArgs, "expectation_failed")
    : await sendSessionPageCommand(session, "assert", expectArgs);
  if (verdict?.ok) {
    if (result.data && typeof result.data === "object") {
      result.data.verified = true;
      result.data.observed = verdict.data?.observed;
    }
    return result;
  }
  return {
    ok: false,
    error: {
      code: "expectation_failed",
      message: verdict?.error?.message || "post-action expectation not met",
      observed: verdict?.error?.observed,
      diagnostics: verdict?.error?.diagnostics || diagnosticsForBridgeError(new Error("post-action expectation not met"), "expectation_failed", {
        action: expectArgs.condition,
        session,
        suggested_next: "Use read_page/network detail to inspect the actual post-action state and refine the expectation."
      })
    }
  };
}

function cloneJson(value) {
  if (value == null) return value;
  return JSON.parse(JSON.stringify(value));
}

function valueAtPath(root, path) {
  const parts = String(path || "").split(".").filter(Boolean);
  let current = root;
  for (const part of parts) {
    if (current == null) return undefined;
    if (Array.isArray(current) && /^\d+$/.test(part)) current = current[Number(part)];
    else current = current[part];
  }
  return current;
}

function resolveBatchPath(context, expression) {
  const path = String(expression || "").trim();
  if (!path) return "";
  if (path === "last") return context.last;
  if (path.startsWith("last.")) return valueAtPath(context.last, path.slice(5));
  if (path === "vars") return context.vars;
  if (path.startsWith("vars.")) return valueAtPath(context.vars, path.slice(5));
  const firstDot = path.indexOf(".");
  const name = firstDot >= 0 ? path.slice(0, firstDot) : path;
  const rest = firstDot >= 0 ? path.slice(firstDot + 1) : "";
  if (Object.prototype.hasOwnProperty.call(context.vars, name)) {
    return rest ? valueAtPath(context.vars[name], rest) : context.vars[name];
  }
  return undefined;
}

function interpolateBatchValue(value, context) {
  if (typeof value === "string") {
    const exact = value.match(/^\$\{([^}]+)\}$/);
    if (exact) {
      const resolved = resolveBatchPath(context, exact[1]);
      return resolved === undefined ? "" : resolved;
    }
    return value.replace(/\$\{([^}]+)\}/g, (_match, expression) => {
      const resolved = resolveBatchPath(context, expression);
      if (resolved == null) return "";
      return typeof resolved === "string" ? resolved : JSON.stringify(resolved);
    });
  }
  if (Array.isArray(value)) return value.map((item) => interpolateBatchValue(item, context));
  if (value && typeof value === "object") {
    const out = {};
    for (const [key, child] of Object.entries(value)) out[key] = interpolateBatchValue(child, context);
    return out;
  }
  return value;
}

function retryOptions(step) {
  const retry = step.retry && typeof step.retry === "object" ? step.retry : {};
  const attempts = Math.max(1, Math.min(10, Number(retry.attempts || step.attempts || 1) || 1));
  const delayMs = Math.max(0, Math.min(30000, Number(retry.delay_ms ?? retry.delayMs ?? 0) || 0));
  return { attempts, delayMs };
}

async function shouldRunBatchStep(session, step, context) {
  if (!step.when || typeof step.when !== "object") return { run: true, verdict: null };
  const whenArgs = interpolateBatchValue(step.when, context);
  if (!whenArgs.timeout_ms && !whenArgs.timeoutMs) whenArgs.timeout_ms = 250;
  const verdict = await handleAssert(session, whenArgs);
  return { run: Boolean(verdict?.ok), verdict };
}

async function executeBatchStep(session, rawStep, index, context, phase) {
  const step = interpolateBatchValue(rawStep || {}, context);
  const actionName = step.action;
  const base = {
    index,
    phase,
    name: step.name || null,
    action: actionName || null
  };
  const when = await shouldRunBatchStep(session, step, context);
  if (!when.run) {
    return {
      result: {
        ...base,
        ok: true,
        skipped: true,
        when: step.when || null,
        observed: when.verdict?.error?.observed
      },
      response: { ok: true, data: { skipped: true } },
      stop: false
    };
  }

  const { attempts, delayMs } = retryOptions(step);
  let response = null;
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      response = await dispatchDaemonAction({ action: actionName, args: step.args || {}, session });
    } catch (error) {
      response = bridgeError(error, "extension_error", { action: actionName, session });
    }
    if (response?.ok || attempt >= attempts) {
      const item = {
        ...base,
        ok: Boolean(response?.ok),
        attempts: attempt,
        data: response?.ok ? response.data : undefined,
        error: response?.ok ? undefined : response?.error
      };
      if (response?.ok && step.set) {
        context.vars[String(step.set)] = cloneJson(response.data);
        item.set = String(step.set);
      }
      context.last = {
        ok: Boolean(response?.ok),
        action: actionName,
        data: response?.data,
        error: response?.error
      };
      return {
        result: item,
        response,
        stop: !response?.ok && step.continue_on_error !== true
      };
    }
    if (delayMs > 0) await sleep(delayMs);
  }
  return {
    result: { ...base, ok: false, error: { code: "batch_internal_error", message: "retry loop did not produce a result" } },
    response: { ok: false, error: { code: "batch_internal_error", message: "retry loop did not produce a result" } },
    stop: true
  };
}

async function executeBatchSteps(session, steps, context, phase) {
  const results = [];
  let stoppedAt = null;
  for (let i = 0; i < steps.length; i += 1) {
    const outcome = await executeBatchStep(session, steps[i] || {}, i, context, phase);
    results.push(outcome.result);
    if (outcome.stop) {
      stoppedAt = i;
      break;
    }
  }
  return { results, stoppedAt };
}

// batch:在同一受管 tab 上顺序执行多步,一次 dispatch 返回逐步结果。默认遇错即停,自带 block/unblock 生命周期。
async function handleBatch(session, args) {
  const steps = Array.isArray(args.steps) ? args.steps : [];
  const finallySteps = Array.isArray(args.finally) ? args.finally : [];
  const wrapLifecycle = args.lifecycle !== false;
  const context = {
    vars: cloneJson(args.vars && typeof args.vars === "object" ? args.vars : {}),
    last: null
  };
  let main = { results: [], stoppedAt: null };
  let cleanup = { results: [], stoppedAt: null };
  if (wrapLifecycle) {
    try { await handleBlockInput(session, { watchdog_ms: args.watchdog_ms }); } catch { /* 遮罩失败不致命 */ }
  }
  try {
    main = await executeBatchSteps(session, steps, context, "main");
    if (finallySteps.length) cleanup = await executeBatchSteps(session, finallySteps, context, "finally");
  } finally {
    if (wrapLifecycle) {
      try { await handleUnblockInput(session); } catch { /* 释放失败不致命 */ }
    }
  }
  const mainOk = main.stoppedAt === null && main.results.every((r) => r.ok);
  const finallyOk = cleanup.stoppedAt === null && cleanup.results.every((r) => r.ok);
  const overallOk = mainOk && finallyOk;
  return {
    ok: overallOk,
    data: {
      success: overallOk,
      steps: main.results,
      finally_steps: cleanup.results,
      stopped_at: main.stoppedAt,
      finally_stopped_at: cleanup.stoppedAt,
      ran: main.results.length,
      total: steps.length,
      vars: context.vars
    }
  };
}

async function dispatchDaemonAction(action) {
  await ensureSessionRegistryRestored();
  let actionName = action.action;
  if (actionName === "read_page" || actionName === "read-page") actionName = "snapshot";
  const args = action.args || {};
  const session = sessionName(action);
  if (actionName === "navigate") {
    const actionStartedAtMs = Date.now();
    return applyExpectation(session, args, await handleNavigate(session, args), actionStartedAtMs);
  }
  if (actionName === "find_tab") {
    return handleFindTab(session, args);
  }
  if (actionName === "list_tabs") {
    return handleListTabs(args.session || session);
  }
  if (actionName === "close_session") {
    return handleCloseSession(session);
  }
  if (actionName === "snapshot" || actionName === "read_page") {
    return sendSessionPageCommand(session, "snapshot", args);
  }
  if (actionName === "wait") {
    return withOperatingOverlay(session, actionName, args, () => handleWait(session, args));
  }
  if (actionName === "assert") {
    return handleAssert(session, args);
  }
  if (actionName === "batch") {
    return handleBatch(session, args);
  }
  if (["click", "type", "hover", "drag", "scroll"].includes(actionName)) {
    const actionStartedAtMs = Date.now();
    const result = await withOperatingOverlay(session, actionName, args, () => pointerAction(session, actionName, args));
    return applyExpectation(session, args, result, actionStartedAtMs);
  }
  if (actionName === "fill") {
    const actionStartedAtMs = Date.now();
    const result = await withOperatingOverlay(session, actionName, args, () => sendSessionPageCommand(session, actionName, args));
    return applyExpectation(session, args, result, actionStartedAtMs);
  }
  if (actionName === "evaluate") {
    return withOperatingOverlay(session, actionName, args, () => cdpEvaluate(session, args));
  }
  if (actionName === "network") {
    return handleNetwork(session, args);
  }
  if (actionName === "devtools") {
    return handleDevtools(session, args);
  }
  if (actionName === "raw_cdp" || actionName === "cdp") {
    return handleRawCdp(session, args);
  }
  if (actionName === "trace") {
    const list = traceBySession.get(session) || [];
    const limit = Number(args.limit || 20);
    return { ok: true, data: { success: true, traces: list.slice(-limit) } };
  }
  if (actionName === "screenshot") {
    return handleScreenshot(session, args);
  }
  if (actionName === "save_as_pdf") {
    return handleSavePdf(session, args);
  }
  if (actionName === "upload") {
    return {
      ok: false,
      error: {
        code: "upload_unavailable",
        message: "file upload dispatch is not available in the extension route yet"
      }
    };
  }
  if (actionName === "block_input") {
    return handleBlockInput(session, args);
  }
  if (actionName === "unblock_input") {
    return handleUnblockInput(session);
  }
  if (actionName === "show_overlay") {
    return sendSessionPageCommand(session, "show_overlay", args);
  }
  if (actionName === "hide_overlay") {
    return sendSessionPageCommand(session, "hide_overlay", args);
  }
  if (actionName === "hide_pointer_path") {
    return sendSessionPageCommand(session, "hide_pointer_path", args);
  }
  if (actionName === "resolve_target") {
    return sendSessionPageCommand(session, "resolve_target", args);
  }
  return {
    ok: false,
    error: {
      code: "unsupported_action",
      message: `Unsupported browser plugin action: ${actionName}`
    }
  };
}

async function pollDaemonOnce() {
  const poll = await postDaemon("/plugin/poll", {
    source: ACE_SOURCE,
    extension_version: chrome.runtime.getManifest().version
  });
  if (!poll?.ok) {
    throw new Error(poll?.error?.message || "plugin poll failed");
  }
  const action = poll.data?.action;
  if (!action) return;

  // 立刻向 daemon 确认已收到本指令。daemon 据此判定"投递成功";若收不到 ack,它会在重投窗口后
  // 把指令重新入队再投一次,从而避免 service worker 在派发瞬间被回收导致指令永久丢失。
  // ack 在执行前发出,因此一旦发出就代表 worker 真的拿到了指令、即将执行,不会误触发重投。
  try {
    await postDaemon("/plugin/ack", {
      source: ACE_SOURCE,
      id: action.id,
      extension_version: chrome.runtime.getManifest().version
    });
  } catch {
    // ack 失败不致命:最坏情况是 daemon 触发一次重投(仅在确实没收到 ack 时才会发生)。
  }

  let result;
  const started = Date.now();
  await postPluginLog("info", "action_start", actionLogData(action));
  try {
    result = await dispatchDaemonAction(action);
  } catch (error) {
    result = {
      ok: false,
      error: {
        code: error?.code || "extension_error",
        message: error instanceof Error ? error.message : String(error)
      }
    };
  }
  const finishData = actionLogData(action, {
    ok: Boolean(result?.ok),
    error_code: result?.ok ? null : (result?.error?.code || "extension_error"),
    duration_ms: Date.now() - started
  });
  await postPluginLog(result?.ok ? "info" : "warn", "action_finish", finishData);
  try {
    await postDaemon("/plugin/result", {
      source: ACE_SOURCE,
      id: action.id,
      result
    });
    await postPluginLog("info", "result_posted", finishData);
  } catch (error) {
    await postPluginLog("error", "result_post_failed", actionLogData(action, {
      error_code: error?.code || "post_failed",
      message: error instanceof Error ? error.message : String(error),
      duration_ms: Date.now() - started
    }));
    throw error;
  }
}

async function startPolling() {
  if (polling) return;
  polling = true;
  while (polling) {
    try {
      await pollDaemonOnce();
      connectionState = {
        ...connectionState,
        connected: true,
        port: daemonPort,
        lastError: null
      };
    } catch (error) {
      connectionState = {
        ...connectionState,
        connected: false,
        port: daemonPort,
        lastError: error instanceof Error ? error.message : String(error)
      };
      await new Promise((resolve) => setTimeout(resolve, 1500));
    }
    await saveConnectionState();
  }
}

// ---- Service worker 保活 ----
// MV3 后台 service worker 空闲约 30 秒就被浏览器回收。长轮询一旦在"派发指令的瞬间"被回收,
// 那条指令在旧设计里会永久丢失(现在 daemon 侧有未确认重投兜底)。这里用"自连端口 + 周期 ping"
// 在存活期间不断刷新空闲计时,尽量不让 worker 在两次操作之间被回收;真被回收后由 alarm 唤醒。
function connectKeepAlivePort() {
  if (keepAlivePort) return;
  try {
    keepAlivePort = chrome.runtime.connect({ name: KEEPALIVE_PORT_NAME });
    keepAlivePort.onDisconnect.addListener(() => {
      keepAlivePort = null;
    });
  } catch {
    keepAlivePort = null;
  }
}

function keepAliveTick() {
  connectKeepAlivePort();
  try {
    keepAlivePort?.postMessage({ type: "ping", at: Date.now() });
  } catch {
    keepAlivePort = null;
  }
}

function ensureKeepAlive() {
  if (keepAliveTimer === null) {
    keepAliveTimer = setInterval(keepAliveTick, KEEPALIVE_PING_MS);
  }
  keepAliveTick();
}

// 自连端口的接收端:收到 ping 即视为一次事件,刷新 service worker 空闲计时(内容无所谓)。
chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== KEEPALIVE_PORT_NAME) return;
  port.onMessage.addListener(() => {
    // no-op:消息送达本身就足以续命。
  });
});

async function connectAndPoll() {
  ensureKeepAlive();
  await ensureSessionRegistryRestored();
  await helloDaemon();
  startPolling();
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.alarms.create(HEARTBEAT_ALARM, { periodInMinutes: HEARTBEAT_PERIOD_MINUTES });
  connectAndPoll();
});

chrome.runtime.onStartup.addListener(() => {
  chrome.alarms.create(HEARTBEAT_ALARM, { periodInMinutes: HEARTBEAT_PERIOD_MINUTES });
  connectAndPoll();
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === HEARTBEAT_ALARM) {
    // 被回收后由 alarm 冷启唤醒:不能只 hello,要把保活和轮询都重新拉起来,
    // 否则 worker 虽然活了却没人继续 poll,指令照样堆在 daemon 里等超时。
    connectAndPoll();
  }
});

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.source !== ACE_SOURCE) return false;

  (async () => {
    if (message.command === "get_status") {
      sendResponse({ ok: true, data: connectionState });
      return;
    }
    if (message.command === "reconnect") {
      const data = await helloDaemon();
      startPolling();
      sendResponse({ ok: true, data });
      return;
    }
    if (message.command === "content_ready") {
      const data = await helloDaemon();
      startPolling();
      sendResponse({ ok: true, data });
      return;
    }
    if (message.command === "release") {
      const response = await sendPageCommand("hide_overlay");
      sendResponse(response);
      return;
    }
    if (message.command === "page_command") {
      const response = await sendPageCommand(message.pageCommand, message.args || {});
      sendResponse(response);
      return;
    }
    sendResponse({ ok: false, error: `Unknown service worker command: ${message.command}` });
  })().catch((error) => {
    sendResponse({ ok: false, error: error instanceof Error ? error.message : String(error) });
  });
  return true;
});

connectAndPoll();
