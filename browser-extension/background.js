// Native messaging bridge plus active-video gating.
//
// Content scripts report whether their tab is visible and has a playing <video>. The pass is enabled
// only while at least one visible, auto-enabled tab is playing video. Manual NR on/off buttons take
// over for 30 seconds, then auto resumes. The layer reads `enabled` live from the shared header.
let port = null;
const reports = new Map();       // tabId -> { visible, playing, auto, rect, ts }
let manualUntil = 0;
let lastDesired = null;
let offSince = 0;                // debounce: video can briefly report paused while buffering/scrolling
const OFF_DELAY_MS = 6000;
let pauseHold = false;           // compare mode keeps the last frame frozen while the video is paused

// Auto-profile-by-site: remember a profile per hostname and apply it when that site's video starts.
let autoProfileEnabled = true;
let siteProfiles = {};           // host -> profile name
let lastAutoHost = '';
let lastAutoProfile = '';
let profilesCache = null;
const HOST_NOISE = new Set(['www', 'com', 'co', 'net', 'org', 'tv', 'cn', 'ca', 'io', 'app', 'm']);

const norm = (s) => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
function hostOf(url) {
  try { return new URL(url || '').hostname.toLowerCase().replace(/^www\./, ''); }
  catch (e) { return ''; }
}
function activeReport() {
  const now = Date.now();
  return [...reports.values()].find(r => r.auto && r.visible && r.playing && now - r.ts < 8000) || null;
}
function visibleReport() {
  const now = Date.now();
  const fresh = [...reports.values()].filter(r => r.auto && r.visible && now - r.ts < 8000);
  return fresh.find(r => r.playing) || fresh[0] || null;
}
async function getProfiles() {
  if (profilesCache) return profilesCache;
  const r = await requestToNative({ cmd: 'profiles.list' });
  if (r && r.ok) profilesCache = r.profiles || [];
  return profilesCache || [];
}
function inferProfile(host, profiles) {
  if (!host) return '';
  const tokens = host.split('.').filter(t => t && !HOST_NOISE.has(t));
  const full = tokens.join('');
  for (const p of profiles) {
    const n = norm(p);
    if (!n) continue;
    if (full.includes(n) || n.includes(full)) return p;
    for (const t of tokens) {
      if (t.length >= 3 && (n.includes(t) || t.includes(n))) return p;
    }
  }
  return '';
}
function saveAutoSettings() {
  if (chrome.storage && chrome.storage.local)
    chrome.storage.local.set({ autoProfileEnabled, siteProfiles });
}
if (chrome.storage && chrome.storage.local) {
  chrome.storage.local.get(['autoProfileEnabled', 'siteProfiles'], (d) => {
    if (d && typeof d.autoProfileEnabled === 'boolean') autoProfileEnabled = d.autoProfileEnabled;
    if (d && d.siteProfiles && typeof d.siteProfiles === 'object') siteProfiles = d.siteProfiles;
  });
}

async function applyAutoProfile() {
  if (!autoProfileEnabled) return;
  const active = visibleReport();
  if (!active) return;
  const host = hostOf(active.url);
  if (!host || lastAutoHost === host) return;
  lastAutoHost = host;
  const profiles = await getProfiles();
  const name = siteProfiles[host] || inferProfile(host, profiles);
  if (!name) { lastAutoProfile = ''; return; }
  const r = await requestToNative({ cmd: 'profiles.load', name });
  if (r && r.ok) lastAutoProfile = name;
}

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
  if (pauseHold) return;          // frozen frame: do not turn the pass off
  if (Date.now() < manualUntil) return;
  const now = Date.now();
  const anyAuto = [...reports.values()].some(r => r.auto && now - r.ts < 8000);
  if (!anyAuto) return;                       // no auto-enabled tab: leave manual state alone
  const active = [...reports.values()].find(r => r.auto && r.visible && r.playing && now - r.ts < 8000);
  const desired = active ? 1 : 0;
  if (desired === 0 && lastDesired !== 0) {
    // A video can briefly report paused/blank during a scroll transition or rebuffer. Don't let that
    // flicker the whole pass off; require several seconds of no active video first.
    if (!offSince) offSince = now;
    if (now - offSince < OFF_DELAY_MS) return;
  } else {
    offSince = 0;   // video is playing again — re-enable immediately
  }
  if (desired === lastDesired) {
    if (desired && active) requestToNative({ cmd: 'roi', rect: active.rect, swapW: active.swapW, swapH: active.swapH }).catch(() => {});
    return;
  }
  if (desired && active) {
    // Do not fire the pass until the helper, layer presenter and Chrome GPU process are all up.
    requestToNative({ cmd: 'roi', rect: active.rect, swapW: active.swapW, swapH: active.swapH })
      .then(() => requestToNative({ cmd: 'chrome.ready' }))
      .then((r) => {
        if (!r || !r.ready) return;        // leave lastDesired at 0 so we retry next poll
        lastDesired = 1;
        return requestToNative({ cmd: 'set', key: 'enabled', value: 1 });
      })
      .catch(() => {});
  } else {
    lastDesired = 0;
    requestToNative({ cmd: 'set', key: 'enabled', value: 0 }).catch(() => {});
  }
}

// The toolbar action toggles the panel in the active tab. The content script stays injected for
// gating (it must keep reporting video state), but the UI starts hidden until the icon is clicked.
async function showPanel(tabId) {
  try {
    await chrome.tabs.sendMessage(tabId, { cmd: 'show_panel' });
  } catch (e) {
    await chrome.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
    await chrome.tabs.sendMessage(tabId, { cmd: 'show_panel' });
  }
}

chrome.action.onClicked.addListener(async (tab) => {
  await showPanel(tab.id);
});

// PWA/app windows have no extension toolbar icon, so a global shortcut toggles the panel there.
chrome.commands.onCommand.addListener(async (command) => {
  if (command !== '_execute_action') return;
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (!tab) return;
    try {
      await chrome.tabs.sendMessage(tab.id, { cmd: 'show_panel' });
    } catch (e) {
      await chrome.scripting.executeScript({ target: { tabId: tab.id }, files: ['content.js'] });
      await chrome.tabs.sendMessage(tab.id, { cmd: 'show_panel' });
    }
  } catch (e) { /* no page / chrome:// */ }
});

// After an extension reload, existing tabs keep a stale content script. Re-inject into all pages
// so the panel reconnects without the user refreshing each tab manually.
async function reinjectAll() {
  try {
    const tabs = await chrome.tabs.query({});
    for (const t of tabs) {
      if (!t.id || !/^https?:/.test(t.url || '')) continue;
      try {
        await chrome.scripting.executeScript({ target: { tabId: t.id }, files: ['content.js'] });
      } catch (e) { /* page not injectable */ }
    }
  } catch (e) { /* tabs API unavailable */ }
}
chrome.runtime.onInstalled.addListener(() => reinjectAll());
chrome.runtime.onStartup.addListener(() => reinjectAll());

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.cmd === 'pause_hold') {
    const was = pauseHold;
    pauseHold = !!msg.active;
    if (pauseHold) {
      offSince = 0; lastDesired = 1;
      requestToNative({ cmd: 'set', key: 'enabled', value: 1 }).catch(() => {});
    } else if (was) {
      offSince = 0;
      applyGating();
    }
    sendResponse({ ok: true, active: pauseHold });
    return false;
  }
  if (msg && msg.cmd === 'auto_profile_get') {
    const active = visibleReport();
    const host = active ? hostOf(active.url) : '';
    sendResponse({ ok: true, enabled: autoProfileEnabled, host,
                   profile: (host && siteProfiles[host]) || lastAutoProfile || '' });
    return false;
  }
  if (msg && msg.cmd === 'auto_profile_set') {
    autoProfileEnabled = !!msg.enabled;
    saveAutoSettings();
    if (autoProfileEnabled) { lastAutoHost = ''; applyAutoProfile(); }
    sendResponse({ ok: true, enabled: autoProfileEnabled });
    return false;
  }
  if (msg && msg.cmd === 'profile_manual') {
    const host = hostOf(msg.url);
    if (host && msg.name) {
      siteProfiles[host] = msg.name;
      lastAutoHost = host;
      lastAutoProfile = msg.name;
      saveAutoSettings();
    }
    sendResponse({ ok: true, host });
    return false;
  }
  if (msg && msg.cmd === 'profile_renamed') {
    for (const h of Object.keys(siteProfiles))
      if (siteProfiles[h] === msg.oldName) siteProfiles[h] = msg.newName;
    if (lastAutoProfile === msg.oldName) lastAutoProfile = msg.newName;
    profilesCache = null;
    saveAutoSettings();
    sendResponse({ ok: true });
    return false;
  }
  if (msg && msg.cmd === 'profile_removed') {
    for (const h of Object.keys(siteProfiles))
      if (siteProfiles[h] === msg.name) delete siteProfiles[h];
    if (lastAutoProfile === msg.name) lastAutoProfile = '';
    profilesCache = null;
    saveAutoSettings();
    sendResponse({ ok: true });
    return false;
  }
  if (msg && msg.cmd === 'profiles.save') {
    profilesCache = null;
    // Let requestToNative handle it, but invalidate our cache first.
  }
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
      applyAutoProfile();
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
chrome.tabs.onActivated.addListener(() => { applyGating(); applyAutoProfile(); });
