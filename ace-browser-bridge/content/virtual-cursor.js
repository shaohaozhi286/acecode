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
    refs: new Map(),
    attachmentRefs: new Map()
  };

  try {
    const wake = chrome.runtime.sendMessage({
      source: SOURCE,
      command: "content_ready",
      url: location.href
    });
    if (wake && typeof wake.catch === "function") wake.catch(() => {});
  } catch {
    // Service worker wake-up is best effort; direct page commands still work.
  }

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

  function overlayLabel(args = {}) {
    const text = typeof args.message === "string" && args.message.trim()
      ? args.message.trim()
      : "AI 正在操作浏览器，请暂时不要操作";
    return text.slice(0, 96);
  }

  function showOverlay(args = {}) {
    if (state.overlay) {
      const label = state.overlay.querySelector(".ace-browser-bridge-operation-label");
      if (label) label.textContent = overlayLabel(args);
      if (state.overlayTimer) window.clearTimeout(state.overlayTimer);
      state.overlayTimer = window.setTimeout(hideOverlay, Math.max(1000, Number(args.watchdog_ms || 10000)));
      refreshOverlayInputMode();
      return;
    }
    const overlay = document.createElement("div");
    overlay.id = "ace-browser-bridge-operation-overlay";
    const label = document.createElement("div");
    label.className = "ace-browser-bridge-operation-label";
    label.textContent = overlayLabel(args);
    overlay.appendChild(label);
    overlay.style.cssText = [
      "position: fixed",
      "inset: 0",
      "z-index: 2147483646",
      "pointer-events: auto",
      "box-sizing: border-box",
      "border: 6px solid transparent",
      "border-image: linear-gradient(135deg, #06b6d4, #14b8a6, #2563eb) 1",
      "background: rgba(8, 47, 73, .12)",
      "box-shadow: inset 0 0 0 2px rgba(45, 212, 191, .55), inset 0 0 52px rgba(14, 165, 233, .22)"
    ].join(";");

    const style = document.createElement("style");
    style.textContent = [
      "#ace-browser-bridge-operation-overlay .ace-browser-bridge-operation-label{",
      "position:absolute;top:18px;left:50%;transform:translateX(-50%);",
      "padding:10px 16px;border-radius:8px;background:linear-gradient(135deg,rgba(15,23,42,.96),rgba(8,47,73,.96));",
      "border:1px solid rgba(125,211,252,.72);",
      "color:#fff;font:600 15px/1.35 Arial,sans-serif;letter-spacing:0;",
      "box-shadow:0 14px 38px rgba(15,23,42,.34);",
      "max-width:min(560px,calc(100vw - 40px));text-align:center;",
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

  function normalizeSpace(value, limit = 12000) {
    return (value ?? "").toString().replace(/\s+/g, " ").trim().slice(0, limit);
  }

  function cssEscape(value) {
    if (window.CSS && typeof window.CSS.escape === "function") return window.CSS.escape(String(value));
    return String(value).replace(/\\/g, "\\\\").replace(/"/g, '\\"');
  }

  function visibleText() {
    return normalizeSpace(document.body?.innerText || "", 12000);
  }

  function textFromIds(ids) {
    return normalizeSpace(String(ids || "").split(/\s+/)
      .map((id) => document.getElementById(id)?.innerText || document.getElementById(id)?.textContent || "")
      .filter(Boolean)
      .join(" "), 240);
  }

  function elementLabelText(el) {
    const labelledBy = textFromIds(el.getAttribute("aria-labelledby"));
    if (labelledBy) return labelledBy;
    if (el.id) {
      const label = document.querySelector(`label[for="${cssEscape(el.id)}"]`);
      if (label) return normalizeSpace(label.innerText || label.textContent || "", 240);
    }
    const parentLabel = el.closest?.("label");
    if (parentLabel) return normalizeSpace(parentLabel.innerText || parentLabel.textContent || "", 240);
    return "";
  }

  function elementName(el) {
    return normalizeSpace(
      el.getAttribute("aria-label") ||
      elementLabelText(el) ||
      el.getAttribute("placeholder") ||
      el.getAttribute("alt") ||
      el.getAttribute("title") ||
      el.value ||
      el.innerText ||
      el.textContent ||
      "",
      160
    );
  }

  const targetSelector = [
    "a[href]",
    "button",
    "input",
    "textarea",
    "select",
    "[role]",
    "summary",
    "[contenteditable='true']",
    "[tabindex]"
  ].join(",");

  const locatorCandidateSelector = [
    targetSelector,
    "label",
    "img[src]",
    "[aria-label]",
    "[aria-labelledby]",
    "[data-testid]",
    "[data-test]",
    "tr",
    "td",
    "th"
  ].join(",");

  function elementRole(el) {
    if (el.getAttribute("role")) return el.getAttribute("role");
    const tag = el.tagName.toLowerCase();
    if (tag === "a") return "link";
    if (tag === "button") return "button";
    if (tag === "textarea") return "textbox";
    if (tag === "select") return "combobox";
    if (tag === "tr") return "row";
    if (tag === "td") return "cell";
    if (tag === "th") return "columnheader";
    if (tag === "img") return "img";
    if (tag === "summary") return "button";
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

  function ariaBool(el, name) {
    const value = el.getAttribute(name);
    if (value == null) return null;
    if (value === "true") return true;
    if (value === "false") return false;
    return value;
  }

  function isDisabled(el) {
    return Boolean(el.disabled) ||
      el.getAttribute("aria-disabled") === "true" ||
      Boolean(el.closest?.("[aria-disabled='true'],fieldset[disabled]"));
  }

  function elementValue(el) {
    if (el.isContentEditable) return normalizeSpace(el.innerText || el.textContent || "", 4000);
    if (el.tagName === "SELECT") return el.value ?? "";
    if ("value" in el) return String(el.value ?? "");
    return null;
  }

  function elementOptions(el) {
    if (el.tagName !== "SELECT") return undefined;
    return Array.from(el.options || []).slice(0, 80).map((option, index) => ({
      index,
      value: option.value,
      label: normalizeSpace(option.label || option.innerText || option.textContent || option.value, 200),
      selected: option.selected,
      disabled: option.disabled
    }));
  }

  function stableSelector(el) {
    if (!el || el.nodeType !== Node.ELEMENT_NODE) return null;
    if (el.id) return `#${cssEscape(el.id)}`;
    for (const attr of ["data-testid", "data-test", "name", "aria-label", "title"]) {
      const value = el.getAttribute(attr);
      if (value) {
        const selector = `${el.tagName.toLowerCase()}[${attr}="${cssEscape(value)}"]`;
        try {
          if (document.querySelectorAll(selector).length === 1) return selector;
        } catch {
          // Fall through to a structural selector.
        }
      }
    }
    const parts = [];
    let node = el;
    for (let depth = 0; node && node.nodeType === Node.ELEMENT_NODE && depth < 5; depth += 1) {
      const tag = node.tagName.toLowerCase();
      let part = tag;
      const parent = node.parentElement;
      if (parent) {
        const siblings = Array.from(parent.children).filter((child) => child.tagName === node.tagName);
        if (siblings.length > 1) part += `:nth-of-type(${siblings.indexOf(node) + 1})`;
      }
      parts.unshift(part);
      node = parent;
    }
    return parts.length ? parts.join(" > ") : null;
  }

  function selectedOptions(el) {
    if (el.tagName !== "SELECT") return undefined;
    return Array.from(el.selectedOptions || []).map((option) => option.value);
  }

  function elementContext(el) {
    const label = elementLabelText(el);
    const parent = el.closest?.("tr,[role='row'],li,section,article,form,fieldset,div") || el.parentElement;
    return {
      label: label || null,
      nearby_text: normalizeSpace(parent?.innerText || parent?.textContent || "", 320) || null
    };
  }

  function elementState(el) {
    const value = elementValue(el);
    const options = elementOptions(el);
    const disabled = isDisabled(el);
    const role = elementRole(el);
    const actionableRoles = new Set(["button", "link", "textbox", "combobox", "checkbox", "radio", "menuitem", "option", "tab", "switch"]);
    const actionable = isVisible(el) && !disabled && (
      actionableRoles.has(role) ||
      el.hasAttribute("tabindex") ||
      el.isContentEditable ||
      typeof el.click === "function"
    );
    return {
      value,
      placeholder: el.getAttribute("placeholder") || null,
      type: el.getAttribute("type") || null,
      href: el.href || null,
      src: el.currentSrc || el.src || null,
      disabled,
      aria_disabled: ariaBool(el, "aria-disabled"),
      busy: ariaBool(el, "aria-busy"),
      expanded: ariaBool(el, "aria-expanded"),
      selected: "selected" in el ? Boolean(el.selected) : ariaBool(el, "aria-selected"),
      checked: "checked" in el ? Boolean(el.checked) : ariaBool(el, "aria-checked"),
      actionable,
      options,
      selected_options: selectedOptions(el)
    };
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

  function rectObject(el) {
    const rect = el.getBoundingClientRect();
    return {
      x: Math.round(rect.x),
      y: Math.round(rect.y),
      width: Math.round(rect.width),
      height: Math.round(rect.height)
    };
  }

  function candidateSummary(el, rank = null) {
    const rect = rectObject(el);
    return {
      rank,
      role: elementRole(el),
      name: elementName(el),
      tag: el.tagName,
      text: normalizeSpace(el.innerText || el.textContent || elementName(el), 160),
      rect,
      stable_selector: stableSelector(el),
      context: elementContext(el)
    };
  }

  function describeElement(el, ref = null, snapshotId = null) {
    const stateFields = elementState(el);
    const item = {
      ref,
      snapshot_id: snapshotId,
      role: elementRole(el),
      name: elementName(el),
      tag: el.tagName,
      text: normalizeSpace(el.innerText || el.textContent || elementName(el), 240),
      rect: rectObject(el),
      stable_selector: stableSelector(el),
      context: elementContext(el),
      actionable: stateFields.actionable,
      disabled: stateFields.disabled,
      aria_disabled: stateFields.aria_disabled,
      busy: stateFields.busy,
      expanded: stateFields.expanded,
      selected: stateFields.selected,
      checked: stateFields.checked,
      value: stateFields.value,
      placeholder: stateFields.placeholder,
      type: stateFields.type,
      href: stateFields.href,
      src: stateFields.src,
      options: stateFields.options,
      selected_options: stateFields.selected_options,
      state: {
        visible: isVisible(el),
        actionable: stateFields.actionable,
        disabled: stateFields.disabled,
        aria_disabled: stateFields.aria_disabled,
        busy: stateFields.busy,
        expanded: stateFields.expanded,
        selected: stateFields.selected,
        checked: stateFields.checked
      }
    };
    Object.keys(item).forEach((key) => item[key] === undefined && delete item[key]);
    return item;
  }

  function attachmentKind(url, el) {
    const lower = String(url || "").split("?")[0].toLowerCase();
    if (el.tagName === "IMG" || /\.(png|jpe?g|gif|webp|bmp|svg)$/.test(lower)) return "image";
    if (/\.pdf$/.test(lower)) return "pdf";
    if (el.hasAttribute("download")) return "download";
    if (/\.(zip|log|txt|csv|json|har|xlsx?|docx?)$/.test(lower)) return "download";
    return null;
  }

  function collectAttachments(snapshotId) {
    state.attachmentRefs.clear();
    const attachments = [];
    let index = 1;
    const seen = new Set();
    for (const el of Array.from(document.querySelectorAll("img[src],a[href][download],a[href],embed[src],object[data]"))) {
      if (!isVisible(el) && el.tagName !== "A") continue;
      const url = el.currentSrc || el.src || el.href || el.data || "";
      if (!url || seen.has(url)) continue;
      const kind = attachmentKind(url, el);
      if (!kind) continue;
      seen.add(url);
      const ref = `@a${index++}`;
      const item = {
        ref,
        snapshot_id: snapshotId,
        kind,
        url,
        tag: el.tagName,
        name: elementName(el),
        mime_hint: kind === "image" ? "image/*" : kind === "pdf" ? "application/pdf" : null,
        rect: isVisible(el) ? rectObject(el) : null,
        context: elementContext(el)
      };
      state.attachmentRefs.set(ref, { element: el, ...item });
      attachments.push(item);
      if (attachments.length >= 80) break;
    }
    return attachments;
  }

  function attachmentInfo(args = {}) {
    const ref = args.attachment_ref || args.attachmentRef || args.ref;
    if (!ref) {
      const error = new Error("attachment_ref is required");
      error.code = "invalid_request";
      throw error;
    }
    if (!state.attachmentRefs.has(ref)) collectAttachments(`snap_${Date.now()}_${state.snapshotCounter}`);
    const item = state.attachmentRefs.get(ref);
    if (!item) {
      const error = new Error(`attachment ref not found: ${ref}`);
      error.code = "element_ref_stale";
      error.suggested_next = "Run read_page again and use a fresh attachment ref.";
      throw error;
    }
    const { element: _element, ...data } = item;
    return { ok: true, data };
  }

  function snapshot() {
    const snapshotId = `snap_${Date.now()}_${++state.snapshotCounter}`;
    state.refs.clear();
    const elements = [];
    let index = 1;
    for (const el of Array.from(document.querySelectorAll(locatorCandidateSelector))) {
      if (!isVisible(el)) continue;
      const ref = `@e${index++}`;
      state.refs.set(ref, el);
      elements.push(describeElement(el, ref, snapshotId));
      if (elements.length >= 250) break;
    }
    return {
      ok: true,
      data: {
        snapshot_id: snapshotId,
        url: location.href,
        title: document.title,
        viewport: {
          width: window.innerWidth,
          height: window.innerHeight,
          scroll_x: window.scrollX,
          scroll_y: window.scrollY,
          device_pixel_ratio: window.devicePixelRatio || 1
        },
        focused: document.activeElement && document.activeElement !== document.body
          ? candidateSummary(document.activeElement)
          : null,
        text: visibleText(),
        elements,
        attachments: collectAttachments(snapshotId)
      }
    };
  }

  function visibleCandidates(root = document) {
    const candidates = [];
    if (root !== document && root.matches?.(locatorCandidateSelector)) candidates.push(root);
    candidates.push(...Array.from(root.querySelectorAll?.(locatorCandidateSelector) || []));
    return candidates.filter((el, index, list) => list.indexOf(el) === index).filter(isVisible);
  }

  function resolveVisibleTextTarget(text, exactOnly = false) {
    const needle = String(text || "").replace(/\s+/g, " ").trim();
    if (!needle) {
      const error = new Error("target_text is required");
      error.code = "invalid_request";
      throw error;
    }
    const visible = visibleCandidates();
    const exact = visible.find((el) => elementName(el) === needle);
    if (exact) return exact;
    if (exactOnly) {
      const error = new Error(`element text not found: ${needle}`);
      error.code = "element_not_found";
      error.candidates = visible.slice(0, 8).map(candidateSummary);
      error.suggested_next = "Run read_page, inspect candidates, or relax exact matching.";
      throw error;
    }
    const partial = visible.find((el) => elementName(el).includes(needle));
    if (partial) return partial;
    const error = new Error(`element text not found: ${needle}`);
    error.code = "element_not_found";
    error.candidates = visible.slice(0, 8).map(candidateSummary);
    error.suggested_next = "Run read_page, inspect candidates, or refine target_text.";
    throw error;
  }

  function locatorHasStructuredFields(locator) {
    return ["role", "name", "near_text", "nearText", "within", "nth", "exact", "visible", "tag"].some((key) => Object.prototype.hasOwnProperty.call(locator, key));
  }

  function roleMatches(el, role) {
    if (!role) return true;
    return elementRole(el).toLowerCase() === String(role).toLowerCase();
  }

  function nameMatches(el, locator) {
    const wanted = locator.name ?? locator.text;
    if (wanted == null || wanted === "") return true;
    const haystack = normalizeSpace(elementName(el) || el.innerText || el.textContent || "", 400);
    const needle = normalizeSpace(wanted, 400);
    return locator.exact === true ? haystack === needle : haystack.includes(needle);
  }

  function nearTextMatches(el, text) {
    const needle = normalizeSpace(text, 400);
    if (!needle) return true;
    let node = el;
    for (let depth = 0; node && depth < 4; depth += 1) {
      const textValue = normalizeSpace(node.innerText || node.textContent || "", 1000);
      if (textValue.includes(needle)) return true;
      node = node.parentElement;
    }
    return false;
  }

  function resolveStructuredLocator(locator) {
    if (locator.selector) return resolveTarget(locator.selector);
    if (locator.target) return resolveTarget(locator.target);
    if (locator.ref) return resolveTarget(locator.ref);
    const root = locator.within ? resolveTarget(locator.within) : document;
    const visibleOnly = locator.visible !== false;
    const tag = locator.tag ? String(locator.tag).toLowerCase() : "";
    const candidates = visibleCandidates(root)
      .filter((el) => !visibleOnly || isVisible(el))
      .filter((el) => !tag || el.tagName.toLowerCase() === tag)
      .filter((el) => roleMatches(el, locator.role))
      .filter((el) => nameMatches(el, locator))
      .filter((el) => nearTextMatches(el, locator.near_text ?? locator.nearText));
    if (!candidates.length) {
      const error = new Error("structured locator matched no visible elements");
      error.code = "element_not_found";
      error.candidates = visibleCandidates(root).slice(0, 8).map(candidateSummary);
      error.suggested_next = "Run read_page and refine role/name/within/near_text.";
      throw error;
    }
    if (locator.nth != null) {
      const nth = Number(locator.nth);
      if (Number.isInteger(nth) && nth >= 0 && nth < candidates.length) return candidates[nth];
      const error = new Error(`structured locator nth out of range: ${locator.nth}`);
      error.code = "element_not_found";
      error.candidates = candidates.slice(0, 8).map(candidateSummary);
      error.suggested_next = "Choose an nth value within the candidate list.";
      throw error;
    }
    if (candidates.length > 1) {
      const error = new Error("structured locator is ambiguous");
      error.code = "ambiguous_target";
      error.candidates = candidates.slice(0, 8).map(candidateSummary);
      error.suggested_next = "Add within, near_text, exact, or nth to disambiguate.";
      throw error;
    }
    return candidates[0];
  }

  function resolveTarget(target) {
    if (target && typeof target === "object") {
      if (target.attachment_ref || target.attachmentRef) {
        const ref = target.attachment_ref || target.attachmentRef;
        const item = state.attachmentRefs.get(ref);
        if (!item?.element || !document.documentElement.contains(item.element)) {
          const error = new Error(`stale attachment ref: ${ref}`);
          error.code = "element_ref_stale";
          throw error;
        }
        return item.element;
      }
      if (typeof target.text === "string" && !locatorHasStructuredFields(target)) {
        return resolveVisibleTextTarget(target.text, target.exact === true);
      }
      return resolveStructuredLocator(target);
    }
    if (typeof target !== "string" || target.trim() === "") {
      const error = new Error("target is required");
      error.code = "invalid_request";
      throw error;
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
    let el = null;
    try {
      el = document.querySelector(target);
    } catch (error) {
      error.code = "invalid_selector";
      throw error;
    }
    if (!el) {
      const error = new Error(`element not found: ${target}`);
      error.code = "element_not_found";
      error.candidates = visibleCandidates().slice(0, 8).map(candidateSummary);
      error.suggested_next = "Run read_page and use @e ref, stable_selector, or a structured locator.";
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
        },
        element: describeElement(el, null, null),
        viewport: {
          width: window.innerWidth,
          height: window.innerHeight,
          scroll_x: window.scrollX,
          scroll_y: window.scrollY,
          device_pixel_ratio: window.devicePixelRatio || 1
        }
      }
    };
  }

  function targetFromArgs(args) {
    if (args?.locator && typeof args.locator === "object") return args.locator;
    if (args?.target && typeof args.target === "object") return args.target;
    if (args?.target || args?.selector) return args?.target || args?.selector;
    if (args?.target_text || args?.targetText) return { text: args.target_text || args.targetText };
    if (args?.role || args?.name || args?.near_text || args?.within) {
      const locator = {};
      for (const key of ["role", "name", "near_text", "nearText", "within", "nth", "exact", "tag"]) {
        if (Object.prototype.hasOwnProperty.call(args, key)) locator[key] = args[key];
      }
      return locator;
    }
    return null;
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
    return { ok: true, data: { success: true, mode: "dom", target: { x: point.x, y: point.y }, resolved: candidateSummary(el), path_points: 1 } };
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
    return { ok: true, data: { success: true, tag: el.tagName, mode: "dom", target: { x: point.x, y: point.y }, resolved: candidateSummary(el), path_points: 1 } };
  }

  function fillTarget(target, value) {
    const el = resolveTarget(target);
    el.focus();
    if (el.isContentEditable) {
      document.execCommand("selectAll", false, null);
      document.execCommand("insertText", false, String(value ?? ""));
      return { ok: true, data: { success: true, tag: el.tagName, mode: "contenteditable", resolved: candidateSummary(el) } };
    }
    el.value = String(value ?? "");
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
    return { ok: true, data: { success: true, tag: el.tagName, mode: "value", resolved: candidateSummary(el) } };
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
    return { ok: true, data: { success: true, tag: el.tagName, mode: "dom", typed_chars: Array.from(text).length, keys, resolved: candidateSummary(el) } };
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

  // 取目标作用域的文本:有 target 时取该元素文本,无 target 时退回整页可见文本。
  // 返回 null 表示 target 给了但解析不到(用于区分"作用域不存在"和"作用域里没有该文本")。
  function scopedText(args) {
    const target = targetFromArgs(args);
    if (!target) return visibleText();
    try {
      const el = resolveTarget(target);
      return (el.innerText || el.value || el.textContent || "").replace(/\s+/g, " ").trim();
    } catch {
      return null;
    }
  }

  // 取目标元素的"值"(input/textarea 的 value,contenteditable 的文本)。
  function scopedValue(args) {
    const target = targetFromArgs(args);
    if (!target) return null;
    try {
      const el = resolveTarget(target);
      if (el.isContentEditable) return (el.innerText || el.textContent || "").trim();
      return (el.value != null ? String(el.value) : "").trim();
    } catch {
      return null;
    }
  }

  // 统一的 DOM 条件求值器,供 wait / assert / 动作内联 expect 共用。
  // 返回 { met, observed }:met 是否满足,observed 是实际观察到的状态(失败时回给盲模型决策)。
  // 注意:network_idle / request_completed 等网络类条件在 service worker 侧求值,不在这里。
  function evaluateCondition(args = {}) {
    const condition = args.condition;
    if (condition === "url_contains") {
      return { met: location.href.includes(String(args.url || "")), observed: location.href };
    }
    if (condition === "url_matches") {
      let met = false;
      try { met = new RegExp(String(args.url || "")).test(location.href); } catch { met = false; }
      return { met, observed: location.href };
    }
    if (condition === "text_present") {
      const text = scopedText(args);
      return { met: text != null && text.includes(String(args.text || "")), observed: text };
    }
    if (condition === "text_absent") {
      const text = scopedText(args);
      return { met: text != null && !text.includes(String(args.text || "")), observed: text };
    }
    if (condition === "text_equals") {
      const text = scopedText(args);
      return { met: text != null && text === String(args.text || ""), observed: text };
    }
    if (condition === "value_equals") {
      const value = scopedValue(args);
      return { met: value != null && value === String(args.value ?? args.text ?? ""), observed: value };
    }
    if (condition === "element_present") {
      try { resolveTarget(targetFromArgs(args)); return { met: true, observed: "present" }; }
      catch { return { met: false, observed: "absent" }; }
    }
    if (condition === "element_visible" || condition === "element_clickable") {
      try {
        const visible = isVisible(resolveTarget(targetFromArgs(args)));
        return { met: visible, observed: visible ? "visible" : "hidden" };
      } catch { return { met: false, observed: "absent" }; }
    }
    if (condition === "element_absent") {
      // 解析不到 = 已消失 = 满足;解析得到但不可见 = 也算 absent。
      try {
        const visible = isVisible(resolveTarget(targetFromArgs(args)));
        return { met: !visible, observed: visible ? "visible" : "hidden" };
      } catch { return { met: true, observed: "absent" }; }
    }
    return { met: false, observed: null };
  }

  // wait 与 assert 共用的轮询器:仅 failCode 不同(wait_timeout vs assertion_failed)。
  // 成功/失败都回 observed,让调用方拿到实际状态。
  function waitForCondition(args = {}, failCode = "wait_timeout") {
    const started = Date.now();
    const timeoutMs = Math.max(1, Number(args.timeout_ms || 5000));
    return new Promise((resolve) => {
      const tick = () => {
        const { met, observed } = evaluateCondition(args);
        if (met) {
          resolve({ ok: true, data: { success: true, matched: args.condition, observed, elapsed_ms: Date.now() - started, url: location.href } });
          return;
        }
        if (Date.now() - started >= timeoutMs) {
          resolve({ ok: false, error: { code: failCode, message: `Timed out waiting for ${args.condition}`, observed } });
          return;
        }
        window.setTimeout(tick, 100);
      };
      tick();
    });
  }

  function diagnosticsForError(error) {
    let focused = null;
    try {
      focused = document.activeElement && document.activeElement !== document.body
        ? candidateSummary(document.activeElement)
        : null;
    } catch {
      focused = null;
    }
    let candidates = Array.isArray(error?.candidates) ? error.candidates : [];
    if (!candidates.length && ["element_not_found", "ambiguous_target", "element_ref_stale"].includes(error?.code)) {
      try {
        candidates = visibleCandidates().slice(0, 8).map(candidateSummary);
      } catch {
        candidates = [];
      }
    }
    return {
      url: location.href,
      title: document.title,
      focused,
      candidates,
      suggested_next: error?.suggested_next || "Run read_page, inspect elements/attachments, and retry with a refined locator."
    };
  }

  function errorResponse(error) {
    return {
      ok: false,
      error: {
        code: error?.code || "extension_error",
        message: error instanceof Error ? error.message : String(error),
        diagnostics: diagnosticsForError(error)
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
      } else if (message.command === "attachment_info") {
        sendResponse(attachmentInfo(message.args || {}));
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
      } else if (message.command === "assert") {
        // assert 与 wait 同一个轮询器,只是失败码不同,且面向"确认而非等待"的语义。
        waitForCondition(message.args || {}, "assertion_failed").then(sendResponse).catch((error) => sendResponse(errorResponse(error)));
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
