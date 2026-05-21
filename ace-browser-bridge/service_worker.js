const ACE_SOURCE = "ace-browser-bridge";
const DEFAULT_PORT = 52007;
const PROTOCOL_VERSION = "0.1";
const HEARTBEAT_ALARM = "ace-browser-host-heartbeat";

let daemonPort = DEFAULT_PORT;
let connectionState = {
  connected: false,
  port: DEFAULT_PORT,
  lastHelloAt: null,
  lastError: null,
  extensionVersion: chrome.runtime.getManifest().version
};
let polling = false;
const sessions = new Map();
const traceBySession = new Map();
const networkBySession = new Map();
const debuggerTabs = new Set();
const tabSessionByTabId = new Map();

function daemonBaseUrl() {
  return `http://127.0.0.1:${daemonPort}`;
}

function capabilities() {
  return {
    cdp: true,
    network: true,
    pdf: true,
    upload: true,
    os_pointer: false,
    operation_overlay: true
  };
}

async function saveConnectionState() {
  await chrome.storage.local.set({ aceBrowserBridgeConnection: connectionState });
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

function sessionState(session) {
  if (!sessions.has(session)) {
    sessions.set(session, {
      session,
      tabId: null,
      ownership: "adopted",
      groupId: null,
      status: "idle",
      groupTitle: `ACE: ${session.slice(0, 24)}`,
      lastPointer: null
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

function statusAfterSuccessfulOperation(session) {
  return networkBySession.get(session)?.active ? "network" : "idle";
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

async function tabForSession(session) {
  const state = sessionState(session);
  if (await tabExists(state.tabId)) {
    return chrome.tabs.get(state.tabId);
  }
  const tab = await activePageTab();
  state.tabId = tab.id;
  state.ownership = "adopted";
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
  if (!state.tabId || !chrome.tabs.group || !chrome.tabGroups) return;
  try {
    const groupId = await chrome.tabs.group({ tabIds: [state.tabId] });
    state.groupId = groupId;
    await chrome.tabGroups.update(groupId, {
      title: state.groupTitle,
      color: colorForStatus(status)
    });
  } catch (error) {
    connectionState.lastError = error instanceof Error ? error.message : String(error);
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
  const watchdogMs = args?.operation_overlay_watchdog_ms || 10000;
  try {
    await sendSessionPageCommand(session, "show_overlay", { watchdog_ms: watchdogMs });
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
    try {
      await sendSessionPageCommand(session, "hide_overlay");
    } catch {
      // Ignore cleanup failures; content script watchdog also removes the overlay.
    }
  }
}

async function handleNavigate(session, args) {
  const state = sessionState(session);
  const operation = args.operation || (args.url ? "goto" : "reload");

  if (operation === "goto") {
    if (!args.url) {
      return { ok: false, error: { code: "invalid_request", message: "navigate goto requires url" } };
    }
    let tab;
    if (args.newTab !== false || !(await tabExists(state.tabId))) {
      tab = await chrome.tabs.create({ url: args.url, active: true });
      state.tabId = tab.id;
      state.ownership = "owned";
    } else {
      tab = await chrome.tabs.update(state.tabId, { url: args.url, active: true });
    }
    await updateGroup(session, args.group_title, "idle");
    trace(session, { action: "navigate", url: args.url, tabId: state.tabId });
    return { ok: true, data: { success: true, url: tab.url || args.url, tabId: state.tabId, ownership: state.ownership } };
  }

  const tab = await tabForSession(session);
  if (operation === "reload") await chrome.tabs.reload(tab.id);
  else if (operation === "back") await chrome.tabs.goBack(tab.id);
  else if (operation === "forward") await chrome.tabs.goForward(tab.id);
  else return { ok: false, error: { code: "invalid_request", message: `Unsupported navigate operation: ${operation}` } };
  trace(session, { action: "navigate", operation, tabId: tab.id });
  return { ok: true, data: { success: true, operation, tabId: tab.id } };
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
  await updateGroup(session, args.group_title, "idle");
  trace(session, { action: "find_tab", url: tab.url, tabId: tab.id });
  return { ok: true, data: { success: true, url: tab.url, title: tab.title, tabId: tab.id, ownership: "adopted" } };
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
    if (state.ownership === "owned") {
      await chrome.tabs.remove(state.tabId);
      closed = 1;
    } else {
      detached = 1;
    }
  }
  sessions.delete(session);
  traceBySession.delete(session);
  networkBySession.delete(session);
  return { ok: true, data: { success: true, closed, detached } };
}

async function handleNetwork(session, args) {
  const state = networkBySession.get(session) || { active: false, requests: [] };
  if (args.cmd === "start") {
    const tab = await tabForSession(session);
    await ensureDebugger(tab.id);
    tabSessionByTabId.set(tab.id, session);
    await cdpCommand(tab.id, "Network.enable");
    state.active = true;
    state.tabId = tab.id;
    state.requests = [];
    networkBySession.set(session, state);
    await updateGroup(session, null, "network");
    return { ok: true, data: { success: true, message: "network capture started" } };
  }
  if (args.cmd === "stop") {
    state.active = false;
    networkBySession.set(session, state);
    if (state.tabId) await detachDebugger(state.tabId);
    await updateGroup(session, null, "idle");
    return { ok: true, data: { success: true, message: "network capture stopped" } };
  }
  if (args.cmd === "list") {
    const filter = String(args.filter || "");
    const requests = state.requests.filter((req) => !filter || req.url.includes(filter));
    return { ok: true, data: { count: requests.length, requests } };
  }
  if (args.cmd === "detail") {
    const requestId = args.requestId || args.request_id;
    const request = state.requests.find((item) => item.requestId === requestId);
    if (!request) {
      return { ok: false, error: { code: "request_not_found", message: `request not found: ${requestId}` } };
    }
    if (state.tabId && debuggerTabs.has(state.tabId)) {
      try {
        const body = await chrome.debugger.sendCommand({ tabId: state.tabId }, "Network.getResponseBody", { requestId });
        request.body = body?.base64Encoded ? { base64Encoded: true, sizeBytes: body.body?.length || 0 } : body?.body;
      } catch {
        // Response bodies may be unavailable for cached, preflight, or streaming requests.
      }
    }
    return { ok: true, data: request };
  }
  return { ok: false, error: { code: "invalid_request", message: `unknown network cmd: ${args.cmd}` } };
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
  const target = args[role] || args.target || args.selector;
  if (!target) {
    const error = new Error(`${role} target is required`);
    error.code = "invalid_request";
    throw error;
  }
  const response = await sendSessionPageCommand(session, "resolve_target", { target });
  if (!response?.ok) {
    const error = new Error(response?.error?.message || "target resolution failed");
    error.code = response?.error?.code || "element_not_found";
    throw error;
  }
  const rect = response.data?.rect || {};
  const targetPoint = rect.width && rect.height ? pointInRect(rect) : { x: response.data.x, y: response.data.y };
  return { ...targetPoint, rect };
}

function bridgeError(error, fallbackCode = "extension_error") {
  return {
    ok: false,
    error: {
      code: error?.code || fallbackCode,
      message: error instanceof Error ? error.message : String(error)
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
  return {
    ok: true,
    data: { success: true, mode: "cdp", speed, from, to, path_points: approach.length + dragPath.length, duration_ms: approachMs + dragMs }
  };
}

async function cdpScroll(session, args) {
  const tab = await tabForSession(session);
  const state = sessionState(session);
  let target = state.lastPointer || { x: 200, y: 200 };
  if (args.target || args.selector) {
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
  if (!wasAttached && !networkBySession.get(session)?.active) {
    await detachDebugger(tab.id);
  }
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
  if (args.target || args.selector) {
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

async function handleScreenshot(session, args) {
  const tab = await tabForSession(session);
  const dataUrl = await chrome.tabs.captureVisibleTab(tab.windowId, { format: "png" });
  const comma = dataUrl.indexOf(",");
  const data = comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl;
  return {
    ok: true,
    data: {
      path: args.output || `/tmp/ace-browser-bridge-screenshots/${Date.now()}.png`,
      mimeType: "image/png",
      sizeBytes: Math.floor(data.length * 3 / 4),
      data
    }
  };
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
  if (!wasAttached) await detachDebugger(tab.id);
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
  const state = networkBySession.get(session);
  if (!state?.active) return;
  if (method === "Network.requestWillBeSent") {
    const request = {
      requestId: params.requestId,
      url: params.request?.url || "",
      method: params.request?.method || "",
      status: null,
      mimeType: null,
      completed: false
    };
    state.requests.push(request);
    while (state.requests.length > 200) state.requests.shift();
  } else if (method === "Network.responseReceived") {
    const request = state.requests.find((item) => item.requestId === params.requestId);
    if (request) {
      request.status = params.response?.status || null;
      request.mimeType = params.response?.mimeType || null;
      request.url = params.response?.url || request.url;
    }
  } else if (method === "Network.loadingFinished") {
    const request = state.requests.find((item) => item.requestId === params.requestId);
    if (request) request.completed = true;
  }
});

chrome.debugger.onDetach.addListener((source) => {
  if (source.tabId) {
    debuggerTabs.delete(source.tabId);
    tabSessionByTabId.delete(source.tabId);
  }
});

async function dispatchDaemonAction(action) {
  const actionName = action.action;
  const args = action.args || {};
  const session = sessionName(action);
  if (actionName === "navigate") {
    return handleNavigate(session, args);
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
    return withOperatingOverlay(session, actionName, args, () => sendSessionPageCommand(session, "wait", args));
  }
  if (["click", "type", "hover", "drag", "scroll"].includes(actionName)) {
    return withOperatingOverlay(session, actionName, args, () => pointerAction(session, actionName, args));
  }
  if (actionName === "fill") {
    return withOperatingOverlay(session, actionName, args, () => sendSessionPageCommand(session, actionName, args));
  }
  if (actionName === "evaluate") {
    return withOperatingOverlay(session, actionName, args, () => cdpEvaluate(session, args));
  }
  if (actionName === "network") {
    return handleNetwork(session, args);
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

  let result;
  try {
    result = await dispatchDaemonAction(action);
  } catch (error) {
    result = {
      ok: false,
      error: {
        code: "extension_error",
        message: error instanceof Error ? error.message : String(error)
      }
    };
  }
  await postDaemon("/plugin/result", {
    source: ACE_SOURCE,
    id: action.id,
    result
  });
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

chrome.runtime.onInstalled.addListener(() => {
  chrome.alarms.create(HEARTBEAT_ALARM, { periodInMinutes: 1 });
  helloDaemon().then(startPolling);
});

chrome.runtime.onStartup.addListener(() => {
  chrome.alarms.create(HEARTBEAT_ALARM, { periodInMinutes: 1 });
  helloDaemon().then(startPolling);
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === HEARTBEAT_ALARM) {
    helloDaemon();
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

helloDaemon().then(startPolling);
