(function initAceVirtualCursor() {
  if (window.__aceBrowserBridgeCursor) {
    return;
  }

  const state = {
    cursor: null,
    badge: null,
    timer: 0,
    x: Math.max(24, Math.round(window.innerWidth * 0.5)),
    y: Math.max(24, Math.round(window.innerHeight * 0.5)),
    running: false
  };

  function createCursor() {
    if (state.cursor) {
      return;
    }

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
    const nextX = edgePadding + Math.random() * width;
    const nextY = edgePadding + Math.random() * height;
    moveTo(nextX, nextY);
  }

  function start() {
    createCursor();
    if (state.running) {
      return;
    }
    state.running = true;
    if (state.badge) {
      state.badge.textContent = "ACE cursor moving";
    }
    randomMove();
    state.timer = window.setInterval(randomMove, 900);
  }

  function stop() {
    state.running = false;
    if (state.timer) {
      window.clearInterval(state.timer);
      state.timer = 0;
    }
    if (state.badge) {
      state.badge.textContent = "ACE cursor stopped";
    }
  }

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (message?.source !== "ace-browser-bridge") {
      return false;
    }

    try {
      if (message.command === "start") {
        start();
        sendResponse({ ok: true, running: true });
        return true;
      }
      if (message.command === "stop") {
        stop();
        sendResponse({ ok: true, running: false });
        return true;
      }
      sendResponse({ ok: false, error: `Unknown command: ${message.command}` });
    } catch (error) {
      sendResponse({ ok: false, error: error instanceof Error ? error.message : String(error) });
    }
    return true;
  });

  window.addEventListener("resize", () => moveTo(state.x, state.y), { passive: true });

  window.__aceBrowserBridgeCursor = {
    start,
    stop,
    moveTo
  };
})();
