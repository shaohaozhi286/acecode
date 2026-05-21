const statusEl = document.getElementById("status");
const daemonEl = document.getElementById("daemon");
const pluginEl = document.getElementById("plugin");
const startButton = document.getElementById("start");
const stopButton = document.getElementById("stop");
const reconnectButton = document.getElementById("reconnect");
const releaseButton = document.getElementById("release");

function setStatus(message) {
  statusEl.textContent = message;
}

async function getActiveTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab?.id) {
    throw new Error("No active tab found.");
  }
  if (!tab.url || /^(chrome|edge|about):/i.test(tab.url)) {
    throw new Error("This page cannot run extension scripts. Open a normal web page first.");
  }
  return tab;
}

async function ensureCursorScript(tabId) {
  await chrome.scripting.executeScript({
    target: { tabId },
    files: ["content/virtual-cursor.js"]
  });
}

async function sendCursorCommand(command) {
  const tab = await getActiveTab();
  await ensureCursorScript(tab.id);
  return chrome.tabs.sendMessage(tab.id, {
    source: "ace-browser-bridge",
    command
  });
}

async function sendWorkerCommand(command) {
  return chrome.runtime.sendMessage({
    source: "ace-browser-bridge",
    command
  });
}

async function refreshStatus() {
  const response = await sendWorkerCommand("get_status");
  if (!response?.ok) {
    throw new Error(response?.error || "Cannot read plugin status.");
  }
  const data = response.data;
  daemonEl.textContent = `127.0.0.1:${data.port || 52007}`;
  pluginEl.textContent = `v${data.extensionVersion || chrome.runtime.getManifest().version}`;
  setStatus(data.connected ? "Connected to ace-browser-host." : (data.lastError || "Daemon is not connected."));
}

async function run(command) {
  startButton.disabled = true;
  stopButton.disabled = true;
  try {
    const response = await sendCursorCommand(command);
    if (!response?.ok) {
      throw new Error(response?.error || "Cursor command failed.");
    }
    setStatus(command === "start" ? "Virtual cursor is moving." : "Virtual cursor stopped.");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error));
  } finally {
    startButton.disabled = false;
    stopButton.disabled = false;
  }
}

startButton.addEventListener("click", () => run("start"));
stopButton.addEventListener("click", () => run("stop"));
reconnectButton.addEventListener("click", async () => {
  reconnectButton.disabled = true;
  try {
    await sendWorkerCommand("reconnect");
    await refreshStatus();
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error));
  } finally {
    reconnectButton.disabled = false;
  }
});
releaseButton.addEventListener("click", async () => {
  releaseButton.disabled = true;
  try {
    const response = await sendWorkerCommand("release");
    if (!response?.ok) throw new Error(response?.error || "Release failed.");
    setStatus("Page control released.");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error));
  } finally {
    releaseButton.disabled = false;
  }
});

refreshStatus().catch((error) => {
  setStatus(error instanceof Error ? error.message : String(error));
});
