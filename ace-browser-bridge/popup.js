const statusEl = document.getElementById("status");
const startButton = document.getElementById("start");
const stopButton = document.getElementById("stop");

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
  const response = await chrome.tabs.sendMessage(tab.id, {
    source: "ace-browser-bridge",
    command
  });
  return response;
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

run("start");
