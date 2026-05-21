(function initAceBrowserBridgeContent() {
  if (window.__aceBrowserBridgeContent) {
    return;
  }

  const SOURCE = "ace-browser-bridge";
  const state = {
    cursor: null,
    badge: null,
    overlay: null,
    overlayTimer: 0,
    pointerDebug: null,
    pointerDebugTimer: 0,
    inputBypassTimer: 0,
    inputBypassUntil: 0,
    timer: 0,
    x: Math.max(24, Math.round(window.innerWidth * 0.5)),
    y: Math.max(24, Math.round(window.innerHeight * 0.5)),
    running: false,
    snapshotCounter: 0,
    refs: new Map()
  };

  function createCursor() {
    if (state.cursor) return;

    const cursor = document.createElement("div");
    cursor.id = "ace-browser-bridge-cursor";
    cursor.setAttribute("aria-hidden", "true");
    cursor.innerHTML = [
      '<svg width="28" height="32" viewBox="0 0 28 32" fill="none" xmlns="http://www.w3.org/2000/svg">',
      '<path d="M3 3L3 26L9.5 20.5L14 30L19.5 27.5L15.5 18.5L25 18.5L3 3Z" fill="#111827" stroke="white" stroke-width="3" stroke-linejoin="round"/>',
      '<path d="M3 3L3 26L9.5 20.5L14 30L19.5 27.5L15.5 18.5L25 18.5L3 3Z" fill="#1F6FEB"/>',
      "</svg>"
    ].join("");
    cursor.style.cssText = [
      "position: fixed",
      "left: 0",
      "top: 0",
      "width: 28px",
      "height: 32px",
      "z-index: 2147483647",
      "pointer-events: none",
      "transform: translate3d(0, 0, 0)",
      "transition: transform 520ms cubic-bezier(.22, .61, .36, 1)",
      "filter: drop-shadow(0 8px 18px rgba(31, 111, 235, .35))",
      "will-change: transform"
    ].join(";");

    const badge = document.createElement("div");
    badge.id = "ace-browser-bridge-badge";
    badge.textContent = "ACE cursor";
    badge.style.cssText = [
      "position: fixed",
      "right: 14px",
      "top: 14px",
      "z-index: 2147483647",
      "padding: 6px 9px",
      "border-radius: 6px",
      "background: rgba(17, 24, 39, .9)",
      "color: white",
      "font: 12px/1.2 Arial, sans-serif",
      "box-shadow: 0 8px 20px rgba(0, 0, 0, .18)",
      "pointer-events: none"
    ].join(";");

    document.documentElement.appendChild(cursor);
    document.documentElement.appendChild(badge);
    state.cursor = cursor;
    state.badge = badge;
    moveTo(state.x, state.y);
  }

  function moveTo(x, y) {
    state.x = Math.round(Math.max(12, Math.min(window.innerWidth - 36, x)));
    state.y = Math.round(Math.max(12, Math.min(window.innerHeight - 40, y)));
    if (state.cursor) {
      state.cursor.style.transform = `translate3d(${state.x}px, ${state.y}px, 0)`;
    }
  }

  function randomMove() {
    const edgePadding = 48;
    const width = Math.max(1, window.innerWidth - edgePadding * 2);
    const height = Math.max(1, window.innerHeight - edgePadding * 2);
    moveTo(edgePadding + Math.random() * width, edgePadding + Math.random() * height);
  }

  function start() {
    createCursor();
    if (state.running) return;
    state.running = true;
    if (state.badge) state.badge.textContent = "ACE cursor moving";
    randomMove();
    state.timer = window.setInterval(randomMove, 900);
  }

  function stop() {
    state.running = false;
    if (state.timer) {
      window.clearInterval(state.timer);
      state.timer = 0;
    }
    if (state.badge) state.badge.textContent = "ACE cursor stopped";
  }

  function interceptEvent(event) {
    if (event.isTrusted === false || Date.now() <= state.inputBypassUntil) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
  }

  function refreshOverlayInputMode() {
    if (!state.overlay) return;
    state.overlay.style.pointerEvents = Date.now() <= state.inputBypassUntil ? "none" : "auto";
  }

  function allowBridgeEvents(args = {}) {
    const durationMs = Math.max(0, Math.min(300000, Number(args.duration_ms || 0)));
    state.inputBypassUntil = Math.max(state.inputBypassUntil, Date.now() + durationMs);
    if (state.inputBypassTimer) window.clearTimeout(state.inputBypassTimer);
    refreshOverlayInputMode();
    state.inputBypassTimer = window.setTimeout(() => {
      state.inputBypassTimer = 0;
      refreshOverlayInputMode();
    }, durationMs + 25);
    return { ok: true, data: { success: true, duration_ms: durationMs } };
  }

  function showOverlay(args = {}) {
    if (state.overlay) {
      if (state.overlayTimer) window.clearTimeout(state.overlayTimer);
      state.overlayTimer = window.setTimeout(hideOverlay, Math.max(1000, Number(args.watchdog_ms || 10000)));
      refreshOverlayInputMode();
      return;
    }
    const overlay = document.createElement("div");
    overlay.id = "ace-browser-bridge-operation-overlay";
    overlay.innerHTML = '<div class="ace-browser-bridge-operation-label">AI 正在操作浏览器</div>';
    overlay.style.cssText = [
      "position: fixed",
      "inset: 0",
      "z-index: 2147483646",
      "pointer-events: auto",
      "box-sizing: border-box",
      "border: 4px solid transparent",
      "border-image: linear-gradient(135deg, #06b6d4, #14b8a6, #2563eb) 1",
      "background: rgba(2, 132, 199, .03)"
    ].join(";");

    const style = document.createElement("style");
    style.textContent = [
      "#ace-browser-bridge-operation-overlay .ace-browser-bridge-operation-label{",
      "position:absolute;top:12px;left:50%;transform:translateX(-50%);",
      "padding:6px 10px;border-radius:6px;background:rgba(15,23,42,.92);",
      "color:#fff;font:13px/1.2 Arial,sans-serif;box-shadow:0 8px 24px rgba(15,23,42,.2);",
      "}"
    ].join("");
    overlay.appendChild(style);

    ["pointerdown", "pointerup", "pointermove", "click", "dblclick", "wheel", "keydown", "keyup", "touchstart", "touchmove", "touchend"].forEach((type) => {
      overlay.addEventListener(type, interceptEvent, { capture: true, passive: false });
      document.addEventListener(type, interceptEvent, { capture: true, passive: false });
    });
    overlay.__aceCleanup = () => {
      ["pointerdown", "pointerup", "pointermove", "click", "dblclick", "wheel", "keydown", "keyup", "touchstart", "touchmove", "touchend"].forEach((type) => {
        document.removeEventListener(type, interceptEvent, { capture: true });
      });
    };

    document.documentElement.appendChild(overlay);
    state.overlay = overlay;
    state.overlayTimer = window.setTimeout(hideOverlay, Math.max(1000, Number(args.watchdog_ms || 10000)));
    refreshOverlayInputMode();
  }

  function hideOverlay() {
    if (state.overlayTimer) {
      window.clearTimeout(state.overlayTimer);
      state.overlayTimer = 0;
    }
    if (state.inputBypassTimer) {
      window.clearTimeout(state.inputBypassTimer);
      state.inputBypassTimer = 0;
    }
    state.inputBypassUntil = 0;
    if (!state.overlay) return;
    if (typeof state.overlay.__aceCleanup === "function") {
      state.overlay.__aceCleanup();
    }
    state.overlay.remove();
    state.overlay = null;
  }

  function hidePointerPath() {
    if (state.pointerDebugTimer) {
      window.clearTimeout(state.pointerDebugTimer);
      state.pointerDebugTimer = 0;
    }
    if (state.pointerDebug) {
      state.pointerDebug.remove();
      state.pointerDebug = null;
    }
  }

  function showPointerPath(args = {}) {
    hidePointerPath();
    const points = Array.isArray(args.points) ? args.points : [];
    const clean = points
      .map((point) => ({ x: Number(point.x), y: Number(point.y) }))
      .filter((point) => Number.isFinite(point.x) && Number.isFinite(point.y))
      .slice(0, 300);
    if (clean.length < 2) return;

    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("id", "ace-browser-bridge-pointer-debug");
    svg.setAttribute("width", "100%");
    svg.setAttribute("height", "100%");
    svg.setAttribute("viewBox", `0 0 ${Math.max(1, window.innerWidth)} ${Math.max(1, window.innerHeight)}`);
    svg.style.cssText = [
      "position: fixed",
      "inset: 0",
      "z-index: 2147483646",
      "pointer-events: none"
    ].join(";");

    const polyline = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
    polyline.setAttribute("points", clean.map((point) => `${point.x},${point.y}`).join(" "));
    polyline.setAttribute("fill", "none");
    polyline.setAttribute("stroke", "#16d3c5");
    polyline.setAttribute("stroke-width", "3");
    polyline.setAttribute("stroke-linecap", "round");
    polyline.setAttribute("stroke-linejoin", "round");
    polyline.setAttribute("opacity", "0.88");
    svg.appendChild(polyline);

    const start = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    start.setAttribute("cx", String(clean[0].x));
    start.setAttribute("cy", String(clean[0].y));
    start.setAttribute("r", "4");
    start.setAttribute("fill", "#60a5fa");
    svg.appendChild(start);

    const last = clean[clean.length - 1];
    const end = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    end.setAttribute("cx", String(last.x));
    end.setAttribute("cy", String(last.y));
    end.setAttribute("r", "5");
    end.setAttribute("fill", "#22c55e");
    svg.appendChild(end);

    document.documentElement.appendChild(svg);
    state.pointerDebug = svg;
    state.pointerDebugTimer = window.setTimeout(hidePointerPath, Math.max(500, Number(args.duration_ms || 2500)));
  }

  function visibleText() {
    return (document.body?.innerText || "")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 12000);
  }

  function elementName(el) {
    return (
      el.getAttribute("aria-label") ||
      el.getAttribute("placeholder") ||
      el.getAttribute("alt") ||
      el.getAttribute("title") ||
      el.innerText ||
      el.value ||
      ""
    ).toString().replace(/\s+/g, " ").trim().slice(0, 160);
  }

  function elementRole(el) {
    if (el.getAttribute("role")) return el.getAttribute("role");
    const tag = el.tagName.toLowerCase();
    if (tag === "a") return "link";
    if (tag === "button") return "button";
    if (tag === "textarea") return "textbox";
    if (tag === "select") return "combobox";
    if (tag === "input") {
      const type = (el.getAttribute("type") || "text").toLowerCase();
      if (type === "checkbox") return "checkbox";
      if (type === "radio") return "radio";
      if (type === "submit" || type === "button") return "button";
      return "textbox";
    }
    if (el.isContentEditable) return "textbox";
    return tag;
  }

  function isVisible(el) {
    const style = window.getComputedStyle(el);
    const rect = el.getBoundingClientRect();
    return style.visibility !== "hidden" &&
      style.display !== "none" &&
      rect.width > 0 &&
      rect.height > 0 &&
      rect.bottom >= 0 &&
      rect.right >= 0 &&
      rect.top <= window.innerHeight &&
      rect.left <= window.innerWidth;
  }

  function snapshot() {
    const snapshotId = `snap_${Date.now()}_${++state.snapshotCounter}`;
    state.refs.clear();
    const selector = [
      "a[href]",
      "button",
      "input",
      "textarea",
      "select",
      "[role='button']",
      "[contenteditable='true']",
      "[tabindex]"
    ].join(",");
    const elements = [];
    let index = 1;
    for (const el of Array.from(document.querySelectorAll(selector))) {
      if (!isVisible(el)) continue;
      const ref = `@e${index++}`;
      state.refs.set(ref, el);
      const rect = el.getBoundingClientRect();
      elements.push({
        ref,
        snapshot_id: snapshotId,
        role: elementRole(el),
        name: elementName(el),
        tag: el.tagName,
        text: elementName(el),
        rect: {
          x: Math.round(rect.x),
          y: Math.round(rect.y),
          width: Math.round(rect.width),
          height: Math.round(rect.height)
        }
      });
      if (elements.length >= 200) break;
    }
    return {
      ok: true,
      data: {
        snapshot_id: snapshotId,
        url: location.href,
        title: document.title,
        text: visibleText(),
        elements
      }
    };
  }

  function resolveTarget(target) {
    if (typeof target !== "string" || target.trim() === "") {
      throw new Error("target is required");
    }
    if (target.startsWith("@e")) {
      const el = state.refs.get(target);
      if (!el || !document.documentElement.contains(el)) {
        const error = new Error(`stale element ref: ${target}`);
        error.code = "element_ref_stale";
        throw error;
      }
      return el;
    }
    const el = document.querySelector(target);
    if (!el) {
      const error = new Error(`element not found: ${target}`);
      error.code = "element_not_found";
      throw error;
    }
    return el;
  }

  function assertFrameBoundary(el) {
    if (!el || !/^(IFRAME|FRAME)$/i.test(el.tagName)) return;
    let sameOrigin = false;
    try {
      sameOrigin = Boolean(el.contentWindow && el.contentWindow.document);
    } catch {
      sameOrigin = false;
    }
    if (!sameOrigin) {
      const error = new Error("target is inside a cross-origin iframe; open the iframe URL directly and read that page");
      error.code = "cross_origin_iframe";
      throw error;
    }
  }

  function rectForTarget(target) {
    const el = resolveTarget(target);
    assertFrameBoundary(el);
    el.scrollIntoView({ block: "center", inline: "center", behavior: "instant" });
    const rect = el.getBoundingClientRect();
    return {
      ok: true,
      data: {
        x: rect.left + rect.width / 2,
        y: rect.top + rect.height / 2,
        rect: {
          x: rect.x,
          y: rect.y,
          width: rect.width,
          height: rect.height
        }
      }
    };
  }

  function targetFromArgs(args) {
    return args?.target || args?.selector;
  }

  function pointForElement(el) {
    assertFrameBoundary(el);
    el.scrollIntoView({ block: "center", inline: "center", behavior: "instant" });
    const rect = el.getBoundingClientRect();
    return {
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2,
      rect
    };
  }

  function dispatchPointer(el, type, point, extra = {}) {
    const event = new PointerEvent(type, {
      bubbles: true,
      cancelable: true,
      pointerId: 1,
      pointerType: "mouse",
      isPrimary: true,
      clientX: point.x,
      clientY: point.y,
      button: extra.button ?? 0,
      buttons: extra.buttons ?? 0
    });
    el.dispatchEvent(event);
  }

  function hoverTarget(target) {
    const el = resolveTarget(target);
    const point = pointForElement(el);
    dispatchPointer(el, "pointerover", point);
    dispatchPointer(el, "pointermove", point);
    el.dispatchEvent(new MouseEvent("mouseover", { bubbles: true, clientX: point.x, clientY: point.y }));
    el.dispatchEvent(new MouseEvent("mousemove", { bubbles: true, clientX: point.x, clientY: point.y }));
    moveTo(point.x, point.y);
    return { ok: true, data: { success: true, mode: "dom", target: { x: point.x, y: point.y }, path_points: 1 } };
  }

  function clickTarget(target, args = {}) {
    let el;
    let point;
    if (!target && Number.isFinite(Number(args.x)) && Number.isFinite(Number(args.y))) {
      point = { x: Number(args.x), y: Number(args.y) };
      el = document.elementFromPoint(point.x, point.y);
      if (!el) {
        const error = new Error(`element not found at coordinates: ${point.x},${point.y}`);
        error.code = "element_not_found";
        throw error;
      }
    } else {
      el = resolveTarget(target);
      point = pointForElement(el);
    }
    moveTo(point.x, point.y);
    dispatchPointer(el, "pointerdown", point, { button: 0, buttons: 1 });
    dispatchPointer(el, "pointerup", point, { button: 0, buttons: 0 });
    el.click();
    return { ok: true, data: { success: true, tag: el.tagName, mode: "dom", target: { x: point.x, y: point.y }, path_points: 1 } };
  }

  function fillTarget(target, value) {
    const el = resolveTarget(target);
    el.focus();
    if (el.isContentEditable) {
      document.execCommand("selectAll", false, null);
      document.execCommand("insertText", false, String(value ?? ""));
      return { ok: true, data: { success: true, tag: el.tagName, mode: "contenteditable" } };
    }
    el.value = String(value ?? "");
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
    return { ok: true, data: { success: true, tag: el.tagName, mode: "value" } };
  }

  function sleep(ms) {
    return new Promise((resolve) => window.setTimeout(resolve, ms));
  }

  function typingDelayRange(args = {}) {
    const raw = Array.isArray(args.delay_ms) ? args.delay_ms : null;
    const low = Math.max(0, Number(raw ? raw[0] : 0) || 0);
    const high = Math.max(low, Number(raw ? raw[1] : low) || low);
    return { min: Math.min(low, 300000), max: Math.min(high, 300000) };
  }

  function randomDelay(range) {
    return Math.round(range.min + Math.random() * Math.max(0, range.max - range.min));
  }

  function insertText(el, text) {
    if (el.isContentEditable) {
      document.execCommand("insertText", false, text);
    } else if ("value" in el) {
      const start = typeof el.selectionStart === "number" ? el.selectionStart : el.value.length;
      const end = typeof el.selectionEnd === "number" ? el.selectionEnd : el.value.length;
      el.value = el.value.slice(0, start) + text + el.value.slice(end);
      const cursor = start + text.length;
      if (typeof el.setSelectionRange === "function") el.setSelectionRange(cursor, cursor);
      el.dispatchEvent(new Event("input", { bubbles: true }));
    }
  }

  async function typeTarget(target, args = {}) {
    const el = resolveTarget(target);
    el.focus();
    if (args.clear) {
      if (el.isContentEditable) {
        document.execCommand("selectAll", false, null);
        document.execCommand("delete", false, null);
      } else if ("value" in el) {
        el.value = "";
        el.dispatchEvent(new Event("input", { bubbles: true }));
      }
    }
    const text = String(args.text ?? "");
    if (text) {
      const chunks = Array.from(text);
      const delay = typingDelayRange(args);
      for (let index = 0; index < chunks.length; index += 1) {
        insertText(el, chunks[index]);
        if (index + 1 < chunks.length) await sleep(randomDelay(delay));
      }
    }
    const keys = Array.isArray(args.keys) ? args.keys.slice() : [];
    if (args.submit) keys.push("Enter");
    for (const key of keys) {
      el.dispatchEvent(new KeyboardEvent("keydown", { bubbles: true, key }));
      el.dispatchEvent(new KeyboardEvent("keyup", { bubbles: true, key }));
    }
    return { ok: true, data: { success: true, tag: el.tagName, mode: "dom", typed_chars: Array.from(text).length, keys } };
  }

  function dragTarget(args = {}) {
    const from = resolveTarget(args.from);
    const fromPoint = pointForElement(from);
    let toPoint;
    if (args.to) {
      const to = resolveTarget(args.to);
      toPoint = pointForElement(to);
    } else if (Array.isArray(args.offset) && args.offset.length >= 2) {
      toPoint = { x: fromPoint.x + Number(args.offset[0]), y: fromPoint.y + Number(args.offset[1]) };
    } else {
      const error = new Error("drag requires to or offset");
      error.code = "invalid_request";
      throw error;
    }
    moveTo(fromPoint.x, fromPoint.y);
    dispatchPointer(from, "pointerdown", fromPoint, { button: 0, buttons: 1 });
    dispatchPointer(document.elementFromPoint(toPoint.x, toPoint.y) || from, "pointermove", toPoint, { buttons: 1 });
    dispatchPointer(document.elementFromPoint(toPoint.x, toPoint.y) || from, "pointerup", toPoint, { button: 0, buttons: 0 });
    moveTo(toPoint.x, toPoint.y);
    return { ok: true, data: { success: true, mode: "dom", from: fromPoint, to: toPoint, path_points: 2 } };
  }

  function scrollPage(args = {}) {
    const deltaX = Number(args.delta_x || 0);
    const deltaY = Number(args.delta_y || 0);
    let target = window;
    if (targetFromArgs(args)) {
      target = resolveTarget(targetFromArgs(args));
      target.scrollIntoView({ block: "center", inline: "center", behavior: "instant" });
    }
    target.dispatchEvent?.(new WheelEvent("wheel", { bubbles: true, cancelable: true, deltaX, deltaY }));
    window.scrollBy(deltaX, deltaY);
    return { ok: true, data: { success: true, mode: "dom", delta_x: deltaX, delta_y: deltaY, scroll_x: window.scrollX, scroll_y: window.scrollY } };
  }

  function evaluatePage(code) {
    if (typeof code !== "string" || !code.trim()) {
      const error = new Error("evaluate requires code");
      error.code = "invalid_request";
      throw error;
    }
    const value = (0, eval)(code);
    return Promise.resolve(value).then((resolved) => ({
      ok: true,
      data: {
        type: resolved === null ? "null" : Array.isArray(resolved) ? "array" : typeof resolved,
        value: typeof resolved === "string" ? resolved : JSON.stringify(resolved)
      }
    }));
  }

  function conditionMet(args = {}) {
    const condition = args.condition;
    if (condition === "url_contains") return location.href.includes(String(args.url || ""));
    if (condition === "url_matches") return new RegExp(String(args.url || "")).test(location.href);
    if (condition === "text_present") return visibleText().includes(String(args.text || ""));
    if (condition === "element_present") {
      try { resolveTarget(targetFromArgs(args)); return true; } catch { return false; }
    }
    if (condition === "element_visible" || condition === "element_clickable") {
      try { return isVisible(resolveTarget(targetFromArgs(args))); } catch { return false; }
    }
    if (condition === "network_idle" || condition === "request_finished") return true;
    return false;
  }

  function waitForCondition(args = {}) {
    const started = Date.now();
    const timeoutMs = Math.max(1, Number(args.timeout_ms || 5000));
    return new Promise((resolve) => {
      const tick = () => {
        if (conditionMet(args)) {
          resolve({ ok: true, data: { success: true, matched: args.condition, elapsed_ms: Date.now() - started, url: location.href } });
          return;
        }
        if (Date.now() - started >= timeoutMs) {
          resolve({ ok: false, error: { code: "wait_timeout", message: `Timed out waiting for ${args.condition}` } });
          return;
        }
        window.setTimeout(tick, 100);
      };
      tick();
    });
  }

  function errorResponse(error) {
    return {
      ok: false,
      error: {
        code: error?.code || "extension_error",
        message: error instanceof Error ? error.message : String(error)
      }
    };
  }

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (message?.source !== SOURCE) return false;

    try {
      if (message.command === "start") {
        start();
        sendResponse({ ok: true, data: { running: true } });
      } else if (message.command === "stop") {
        stop();
        sendResponse({ ok: true, data: { running: false } });
      } else if (message.command === "snapshot") {
        sendResponse(snapshot());
      } else if (message.command === "show_overlay") {
        showOverlay(message.args || {});
        sendResponse({ ok: true, data: { visible: true } });
      } else if (message.command === "allow_bridge_events") {
        sendResponse(allowBridgeEvents(message.args || {}));
      } else if (message.command === "hide_overlay") {
        hideOverlay();
        sendResponse({ ok: true, data: { visible: false } });
      } else if (message.command === "show_pointer_path") {
        showPointerPath(message.args || {});
        sendResponse({ ok: true, data: { visible: true } });
      } else if (message.command === "hide_pointer_path") {
        hidePointerPath();
        sendResponse({ ok: true, data: { visible: false } });
      } else if (message.command === "resolve_target") {
        sendResponse(rectForTarget(targetFromArgs(message.args)));
      } else if (message.command === "click") {
        sendResponse(clickTarget(targetFromArgs(message.args), message.args || {}));
      } else if (message.command === "fill") {
        sendResponse(fillTarget(targetFromArgs(message.args), message.args?.value));
      } else if (message.command === "type") {
        typeTarget(targetFromArgs(message.args), message.args || {}).then(sendResponse).catch((error) => sendResponse(errorResponse(error)));
        return true;
      } else if (message.command === "hover") {
        sendResponse(hoverTarget(targetFromArgs(message.args)));
      } else if (message.command === "drag") {
        sendResponse(dragTarget(message.args || {}));
      } else if (message.command === "scroll") {
        sendResponse(scrollPage(message.args || {}));
      } else if (message.command === "evaluate") {
        evaluatePage(message.args?.code).then(sendResponse).catch((error) => sendResponse(errorResponse(error)));
        return true;
      } else if (message.command === "wait") {
        waitForCondition(message.args || {}).then(sendResponse).catch((error) => sendResponse(errorResponse(error)));
        return true;
      } else {
        sendResponse({ ok: false, error: { code: "unknown_command", message: `Unknown command: ${message.command}` } });
      }
    } catch (error) {
      sendResponse(errorResponse(error));
    }
    return true;
  });

  window.addEventListener("resize", () => moveTo(state.x, state.y), { passive: true });
  window.addEventListener("beforeunload", () => {
    hideOverlay();
    hidePointerPath();
  });

  window.__aceBrowserBridgeContent = {
    start,
    stop,
    moveTo,
    snapshot,
    showOverlay,
    allowBridgeEvents,
    hideOverlay,
    showPointerPath,
    hidePointerPath
  };

  try {
    chrome.runtime.sendMessage({ source: SOURCE, command: "reconnect" });
  } catch {
    // The service worker can also connect from extension lifecycle events.
  }
})();
