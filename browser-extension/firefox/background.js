// Native messaging bridge plus active-video gating.
//
// Content scripts report whether their tab is visible and has a playing <video>. The pass is enabled
// only while at least one visible, auto-enabled tab is playing video. Manual NR on/off buttons take
// over for 30 seconds, then auto resumes. The layer reads `enabled` live from the shared header.
let port = null;
const reports = new Map();       // tabId -> { visible, playing, auto, rect, ts }
let manualUntil = 0;
let lastDesired = null;

function connect() {
  port = chrome.runtime.connectNative('com.dlssnr.bridge');
  port.onDisconnect.addListener(() => {
    port = null;
    setTimeout(connect, 1000);
  });
  port.onMessage.addListener(() => {
    // Responses are dispatched in requestToNative() below by id.
  });
}
connect();

function requestToNative(msg) {
  return new Promise((resolve) => {
    if (!port) connect();
    const id = Math.random().toString(36).slice(2);
    const handler = (resp) => {
      if (resp && resp.id === id) {
        port.onMessage.removeListener(handler);
        resolve(resp);
      }
    };
    port.onMessage.addListener(handler);
    try {
      port.postMessage({ ...msg, id });
    } catch (e) {
      port.onMessage.removeListener(handler);
      resolve({ ok: false, error: String(e) });
    }
  });
}

function applyGating() {
  if (Date.now() < manualUntil) return;
  const now = Date.now();
  const anyAuto = [...reports.values()].some(r => r.auto && now - r.ts < 8000);
  if (!anyAuto) return;                       // no auto-enabled tab: leave manual state alone
  const active = [...reports.values()].find(r => r.auto && r.visible && r.playing && now - r.ts < 8000);
  const desired = active ? 1 : 0;
  if (desired === lastDesired) {
    if (desired && active) requestToNative({ cmd: 'roi', rect: active.rect, swapW: active.swapW, swapH: active.swapH }).catch(() => {});
    return;
  }
  lastDesired = desired;
  if (desired && active) {
    requestToNative({ cmd: 'roi', rect: active.rect, swapW: active.swapW, swapH: active.swapH })
      .then(() => requestToNative({ cmd: 'set', key: 'enabled', value: 1 }))
      .catch(() => {});
  } else {
    requestToNative({ cmd: 'set', key: 'enabled', value: 0 }).catch(() => {});
  }
}

// The toolbar action toggles the panel in the active tab. The content script stays injected for
// gating (it must keep reporting video state), but the UI starts hidden until the icon is clicked.
chrome.action.onClicked.addListener(async (tab) => {
  try {
    await chrome.tabs.sendMessage(tab.id, { cmd: 'toggle_panel' });
  } catch (e) {
    // Page may not have the content script (e.g. chrome://), inject it once.
    await chrome.scripting.executeScript({ target: { tabId: tab.id }, files: ['content.js'] });
    await chrome.tabs.sendMessage(tab.id, { cmd: 'toggle_panel' });
  }
});

// PWA/app windows have no extension toolbar icon, so a global shortcut toggles the panel there.
chrome.commands.onCommand.addListener(async (command) => {
  if (command !== '_execute_action') return;
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (!tab) return;
    try {
      await chrome.tabs.sendMessage(tab.id, { cmd: 'toggle_panel' });
    } catch (e) {
      await chrome.scripting.executeScript({ target: { tabId: tab.id }, files: ['content.js'] });
      await chrome.tabs.sendMessage(tab.id, { cmd: 'toggle_panel' });
    }
  } catch (e) { /* no page / chrome:// */ }
});

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.cmd === 'tab_report') {
    const id = sender.tab && sender.tab.id;
    if (id != null) {
      reports.set(id, {
        visible: !!msg.visible,
        playing: !!msg.playing,
        auto: !!msg.auto,
        rect: msg.rect || null,
        swapW: msg.swapW || 0,
        swapH: msg.swapH || 0,
        url: msg.url || '',
        ts: Date.now(),
      });
      if (msg.manual) manualUntil = Date.now() + 30000;
      applyGating();
    }
    sendResponse({ ok: true });
    return false;
  }
  requestToNative(msg).then(sendResponse);
  return true;
});

chrome.tabs.onRemoved.addListener((tabId) => {
  reports.delete(tabId);
  applyGating();
});
chrome.tabs.onActivated.addListener(() => applyGating());
