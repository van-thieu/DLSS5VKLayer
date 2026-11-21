// DLSS5 browser panel. Drives the same shared-memory settings as the desktop helper, with gating
// from the active visible tab and profile management using the desktop GUI's INI format.
(() => {
  if (window.__dlss5Panel) return;
  window.__dlss5Panel = true;

  const send = (msg) => new Promise((resolve) => {
    let done = false;
    const finish = (v) => { if (!done) { done = true; resolve(v); } };
    const timer = setTimeout(() => finish({ ok: false, error: 'native host timeout', timeout: true }), 4000);
    try {
      chrome.runtime.sendMessage(msg, (r) => {
        clearTimeout(timer);
        if (chrome.runtime.lastError) { finish({ ok: false, error: chrome.runtime.lastError.message, staleContext: true }); return; }
        finish(r || { ok: false });
      });
    } catch (e) {
      clearTimeout(timer);
      finish({ ok: false, error: String(e), staleContext: /context.*invalid/i.test(String(e)) });
    }
  });

  const biggestVideo = () => {
    const vs = [...document.querySelectorAll('video')].filter(v => v.currentSrc || v.src);
    return vs.sort((a, b) => (b.videoWidth * b.videoHeight) - (a.videoWidth * a.videoHeight))[0] || null;
  };

  const videoState = () => {
    const v = biggestVideo();
    const r = v ? v.getBoundingClientRect() : null;
    const playing = !!(v && !v.paused && v.readyState >= 2 && r && r.width > 0 && r.height > 0);
    let rect = null;
    let intrinsic = null;
    if (v && r && playing) {
      const vw = v.videoWidth || 0;
      const vh = v.videoHeight || 0;
      const fit = (getComputedStyle(v).objectFit || 'fill').toLowerCase();
      let ox = 0, oy = 0, cw = r.width, ch = r.height;
      // The element box can contain letterbox bars. The layer should work on the actual picture,
      // not the black bars, so shrink the rect to the visible video content when object-fit says so.
      if (vw && vh && (fit === 'contain' || fit === 'cover')) {
        const scale = fit === 'contain' ? Math.min(r.width / vw, r.height / vh)
                                        : Math.max(r.width / vw, r.height / vh);
        const fw = vw * scale, fh = vh * scale;
        if (fit === 'contain') {
          ox = (r.width - fw) / 2;
          oy = (r.height - fh) / 2;
          cw = fw; ch = fh;
        } else {
          // cover fills the element; the cropped-away part is outside the element anyway.
          cw = r.width; ch = r.height;
        }
      }
      // getBoundingClientRect is viewport CSS pixels. The Vulkan swapchain is the whole window,
      // so add the browser UI around the viewport and convert CSS pixels to device pixels.
      // Chrome's vertical tabs/side panel sit on the left, so outerWidth-innerWidth is a LEFT offset
      // here; outerHeight-innerHeight is the bar above the viewport. The visible web area is the
      // layout viewport (clientWidth/clientHeight), which excludes the scrollbar.
      const factor = (window.devicePixelRatio || 1) * (visualViewport.scale || 1);
      const chromeLeft = Math.max(0, outerWidth - innerWidth);
      const chromeTop = Math.max(0, outerHeight - innerHeight);
      const visW = document.documentElement.clientWidth || innerWidth;
      const visH = document.documentElement.clientHeight || innerHeight;
      const left = Math.max(0, r.left + ox);
      const top = Math.max(0, r.top + oy);
      rect = {
        x: Math.round((chromeLeft + left) * factor),
        y: Math.round((chromeTop + top) * factor),
        w: Math.round(Math.max(0, Math.min(cw, visW - left)) * factor),
        h: Math.round(Math.max(0, Math.min(ch, visH - top)) * factor),
      };
      intrinsic = { w: vw, h: vh, fit };
    }
    return {
      visible: document.visibilityState === 'visible',
      playing,
      rect,
      intrinsic,
      swapW: Math.round(outerWidth * (window.devicePixelRatio || 1) * (visualViewport.scale || 1)),
      swapH: Math.round(outerHeight * (window.devicePixelRatio || 1) * (visualViewport.scale || 1)),
      url: location.href,
    };
  };

  let autoEnabled = true;
  let nrOn = true;
  let autoRestartEnabled = true;
  let helperOn = false;
  const reportState = (manual = false) => send({
    cmd: 'tab_report', ...videoState(), auto: autoEnabled, manual,
  });

  const setToggle = (el, on) => el.classList.toggle('on', !!on);
  const bindToggle = (id, get, set) => {
    const el = $(id);
    setToggle(el, get());
    const flip = () => { set(!get()); setToggle(el, get()); };
    el.addEventListener('click', flip);
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); flip(); }
    });
  };

  // Chrome hangs in vkAcquireNextImageKHR if the layer is composing while a tab's swapchain is
  // recreated on reload. Turn the pass off before the page goes away and keep it off briefly after.
  let suppressSent = false;
  const suppressForReload = () => {
    if (suppressSent) return;
    suppressSent = true;
    send({ cmd: 'suppress', ms: 4000 });
  };
  window.addEventListener('beforeunload', suppressForReload);
  window.addEventListener('pagehide', suppressForReload);
  window.addEventListener('load', () => { suppressSent = false; });

  const panel = document.createElement('div');
  panel.id = 'dlss5-panel';
  panel.innerHTML = `
    <div id="dlss5-head"><span>DLSS5 Neural Rendering</span><button id="dlss5-close">×</button></div>
    <div id="dlss5-body">
      <div id="dlss5-status">Connecting...</div>
      <div id="dlss5-roi-note" class="dlss5-note">ROI initializing...</div>
      <div class="dlss5-divider"></div>
      <div class="dlss5-row"><span class="dlss5-label">Neural Rendering</span>
        <span class="dlss5-toggle" id="dlss5-nr-toggle" role="switch" tabindex="0"><i></i></span></div>
      <div class="dlss5-row"><span class="dlss5-label">Auto NR (playing video)</span>
        <span class="dlss5-toggle" id="dlss5-auto" role="switch" tabindex="0"><i></i></span></div>
      <div class="dlss5-row"><span class="dlss5-label">Helper <small id="dlss5-helper-status">checking…</small></span>
        <span class="dlss5-toggle" id="dlss5-helper-toggle" role="switch" tabindex="0"><i></i></span>
        <button class="dlss5-icon" id="dlss5-helper-restart" title="Restart helper">⟳</button></div>
      <div class="dlss5-row"><span class="dlss5-label">Auto Restart Chrome</span>
        <span class="dlss5-toggle" id="dlss5-auto-restart" role="switch" tabindex="0"><i></i></span>
        <button class="dlss5-icon" id="dlss5-chrome-restart" title="Restart Chrome">⟳</button></div>
      <div class="dlss5-divider"></div>
      <div class="dlss5-profilebar">
        <select id="dlss5-profile-list"></select>
        <button class="dlss5-icon" id="dlss5-profile-rename" title="Rename profile">✎</button>
        <button class="dlss5-icon" id="dlss5-profile-save" title="Save current as profile">▣</button>
        <button class="dlss5-icon" id="dlss5-profile-remove" title="Delete profile">✕</button>
      </div>
      <div id="dlss5-tabs" class="dlss5-tabbar">
        <button data-tab="Rendering" class="active">Rendering</button>
        <button data-tab="Quality">Quality</button>
        <button data-tab="Composition">Composition</button>
      </div>
      <div id="dlss5-pages"></div>
    </div>`;
  document.documentElement.appendChild(panel);
  panel.style.display = 'none';   // shown only by clicking the toolbar icon

  chrome.runtime.onMessage.addListener((msg) => {
    if (msg && msg.cmd === 'toggle_panel') {
      panel.style.display = panel.style.display === 'none' ? 'block' : 'none';
      if (panel.style.display === 'block') {
        refresh();
        refreshHelper();
        refreshProfiles();
      }
    }
  });

  const $ = (id) => panel.querySelector('#' + id);
  const status = $('dlss5-status');

  const pages = $('dlss5-pages');
  const controls = new Map();

  const setStatus = (text, bad) => {
    status.textContent = text;
    status.style.color = bad ? '#ff8a80' : '#a5d6a7';
  };

  $('dlss5-close').addEventListener('click', () => {
    panel.style.display = panel.style.display === 'none' ? 'block' : 'none';
  });
  const head = $('dlss5-head');
  let drag = null;
  head.addEventListener('mousedown', (e) => {
    drag = { x: e.clientX, y: e.clientY, l: panel.offsetLeft, t: panel.offsetTop };
    e.preventDefault();
  });
  document.addEventListener('mousemove', (e) => {
    if (!drag) return;
    panel.style.left = (drag.l + e.clientX - drag.x) + 'px';
    panel.style.top = (drag.t + e.clientY - drag.y) + 'px';
  });
  document.addEventListener('mouseup', () => { drag = null; });

  // ---------- control definitions ----------
  const DEF = {
    Rendering: [
      { key: 'style', label: 'Style', type: 'choice', options: [['0', 'Default'], ['1', 'Natural'], ['2', 'Cinematic']] },
      { key: 'intensity', label: 'Intensity', type: 'range', min: 0, max: 4, step: 0.05 },
      { key: 'localstructure', label: 'Local structure', type: 'range', min: 0, max: 4, step: 0.05 },
      { key: 'localtone', label: 'Local tone', type: 'range', min: 0, max: 4, step: 0.05 },
      { key: 'skinstructure', label: 'Skin structure', type: 'range', min: -1, max: 4, step: 0.05 },
      { key: 'automask', label: 'Auto skin mask', type: 'check' },
      { key: 'sharpness', label: 'Sharpness', type: 'range', min: 0, max: 1, step: 0.05 },
      { key: 'passes', label: 'Passes', type: 'int', min: 1, max: 30 },
      { key: 'workingscale', label: 'Model resolution', type: 'range', min: 0.25, max: 2, step: 0.05 },
      { key: 'downscaler', label: 'Down-leg filter', type: 'choice', options: [
        ['1', 'Bicubic'], ['2', 'Catmull-Rom'], ['3', 'Lanczos2'], ['4', 'Lanczos3'],
        ['5', 'Kaiser2'], ['6', 'Kaiser3'], ['7', 'Magic']] },
    ],
    Quality: [
      { key: 'mvec', label: 'Estimate motion vectors', type: 'check' },
      { key: 'mvecquality', label: 'Motion quality', type: 'choice', options: [['0', 'Fast'], ['1', 'Balanced'], ['2', 'Quality']] },
      { key: 'mvecunits', label: 'Motion units', type: 'choice', options: [['0', 'Normalised'], ['1', 'Pixels'], ['2', 'UV 0..1']] },
      { key: 'mvecpixels', label: 'Motion pixel size', type: 'choice', options: [['0', '1 px'], ['1', '2 px'], ['2', '4 px'], ['3', '8 px']] },
      { key: 'hdrmode', label: 'HDR input', type: 'choice', options: [['0', 'Auto'], ['1', 'Off'], ['2', 'Force float16']] },
      { key: 'sdr16multipass', label: '16-bit SDR intermediates', type: 'check' },
      { key: 'colourmode', label: 'Frame holds', type: 'choice', options: [['0', 'Auto'], ['1', 'Display-referred'], ['2', 'Linear light']] },
    ],
    Composition: [
      { key: 'bypass', label: 'Present raw model answer', type: 'check' },
      { key: 'detail', label: 'Detail strength', type: 'range', min: 0, max: 4, step: 0.05 },
      { key: 'colour', label: 'Colour strength', type: 'range', min: 0, max: 4, step: 0.05 },
      { key: 'guard', label: 'Highlight guard', type: 'range', min: 1, max: 30, step: 0.5 },
      { key: 'transfer', label: 'How the answer is applied', type: 'choice', options: [
        ['0', 'Classic'], ['1', 'Matched residual'], ['2', 'Native + edit']] },
      { key: 'compare', label: 'Compare', type: 'choice', options: [['0', 'Off'], ['1', 'Side by side'], ['2', 'Wipe']] },
      { key: 'comparesplit', label: 'Split', type: 'range', min: 0, max: 1, step: 0.01 },
      { key: 'comparezoom', label: 'Compare zoom', type: 'range', min: 1, max: 2, step: 0.05 },
      { key: 'compareswap', label: 'Swap sides', type: 'check' },
      { key: 'applymodel', label: 'Apply the model edit', type: 'check' },
      { key: 'reversible', label: 'Proxy', type: 'choice', options: [
        ['0', 'Soft knee'], ['1', 'Neutwo'], ['2', 'Neutwo replace'], ['3', 'Hybrid'], ['4', 'Hybrid replace']] },
      { key: 'whitepointsource', label: 'White point source', type: 'choice', options: [['0', 'Manual'], ['1', 'Measured']] },
      { key: 'whitepoint', label: 'Paper white', type: 'range', min: 0.01, max: 100, step: 0.05 },
      { key: 'whitepointscale', label: 'White point scale', type: 'range', min: 0.01, max: 100, step: 0.05 },
      { key: 'whitepointtrim', label: 'White point trim', type: 'range', min: 0.01, max: 100, step: 0.05 },
      { key: 'debugview', label: 'Debug view', type: 'choice', options: [
        ['0', 'Off'], ['1', 'Model input'], ['2', 'Raw answer'], ['3', 'Amplified edit'], ['4', 'Colour bound'], ['5', 'Before bound']] },
      { key: 'debugscale', label: 'Debug scale', type: 'range', min: 0.01, max: 100, step: 0.05 },
    ],
    Profiles: [],
  };

  const makeControl = (def) => {
    const row = document.createElement('div');
    row.className = 'dlss5-row';
    const label = document.createElement('span');
    label.className = 'dlss5-label';
    label.textContent = def.label;
    row.appendChild(label);
    let el, valEl;
    if (def.type === 'check') {
      el = document.createElement('input');
      el.type = 'checkbox';
      el.addEventListener('change', () => send({ cmd: 'set', key: def.key, value: el.checked ? 1 : 0 }));
    } else if (def.type === 'choice') {
      el = document.createElement('select');
      for (const [v, text] of def.options) {
        const o = document.createElement('option');
        o.value = v; o.textContent = text; el.appendChild(o);
      }
      el.addEventListener('change', () => send({ cmd: 'set', key: def.key, value: Number(el.value) }));
    } else if (def.type === 'int') {
      el = document.createElement('input');
      el.type = 'number';
      el.min = def.min; el.max = def.max;
      el.addEventListener('change', () => send({ cmd: 'set', key: def.key, value: Number(el.value) }));
    } else {
      el = document.createElement('input');
      el.type = 'range';
      el.min = def.min; el.max = def.max; el.step = def.step;
      valEl = document.createElement('span');
      valEl.className = 'dlss5-val';
      el.addEventListener('input', () => { valEl.textContent = Number(el.value).toFixed(2); });
      el.addEventListener('change', () => send({ cmd: 'set', key: def.key, value: Number(el.value) }));
    }
    row.appendChild(el);
    if (valEl) row.appendChild(valEl);
    controls.set(def.key, { el, valEl, type: def.type });
    return row;
  };

  const buildPages = () => {
    for (const [name, defs] of Object.entries(DEF)) {
      const page = document.createElement('div');
      page.className = 'dlss5-page';
      page.dataset.tab = name;
      page.style.display = name === 'Rendering' ? 'block' : 'none';
      if (name === 'Profiles') {
        page.appendChild(buildProfiles());
      } else {
        for (const def of defs) page.appendChild(makeControl(def));
      }
      pages.appendChild(page);
    }
  };

  // Pages can block window.prompt/confirm, so profile dialogs live inside the panel.
  const askText = (title, label, initial) => new Promise((resolve) => {
    const dlg = document.createElement('div');
    dlg.className = 'dlss5-dialog';
    dlg.innerHTML = `<div class="dlss5-dialog-title">${title}</div>
      <label class="dlss5-row"><span>${label}</span><input type="text"></label>
      <div class="dlss5-actions"><button data-v="cancel">Cancel</button><button data-v="ok">OK</button></div>`;
    const input = dlg.querySelector('input');
    input.value = initial || '';
    const done = (v) => { dlg.remove(); resolve(v); };
    dlg.querySelectorAll('button').forEach(b => b.onclick = () => done(b.dataset.v === 'ok' ? input.value : null));
    input.onkeydown = (e) => { if (e.key === 'Enter') done(input.value); if (e.key === 'Escape') done(null); };
    panel.appendChild(dlg);
    input.focus(); input.select();
  });
  const askConfirm = (title, text) => new Promise((resolve) => {
    const dlg = document.createElement('div');
    dlg.className = 'dlss5-dialog';
    dlg.innerHTML = `<div class="dlss5-dialog-title">${title}</div><div class="dlss5-note">${text}</div>
      <div class="dlss5-actions"><button data-v="cancel">Cancel</button><button data-v="ok">OK</button></div>`;
    const done = (v) => { dlg.remove(); resolve(v); };
    dlg.querySelectorAll('button').forEach(b => b.onclick = () => done(b.dataset.v === 'ok'));
    panel.appendChild(dlg);
  });

  const buildProfiles = () => {
    const wrap = document.createElement('div');
    wrap.innerHTML = `
      <select id="dlss5-profile-list" size="6"></select>
      <div class="dlss5-actions">
        <button id="dlss5-profile-load">Load</button>
        <button id="dlss5-profile-save">Save as...</button>
        <button id="dlss5-profile-rename">Rename...</button>
        <button id="dlss5-profile-remove">Remove</button>
      </div>
      <div class="dlss5-actions">
        <button id="dlss5-profile-video">Apply video tuning</button>
        <button id="dlss5-profile-default">Defaults</button>
        <button id="dlss5-profile-refresh">Refresh list</button>
      </div>`;
    const list = wrap.querySelector('#dlss5-profile-list');
    const selected = () => list.value;
    wrap.querySelector('#dlss5-profile-load').onclick = async () => {
      if (!selected()) return;
      const r = await send({ cmd: 'profiles.load', name: selected() });
      setStatus(r.ok ? `Loaded profile ${selected()}.` : `Load failed: ${r.error}`, !r.ok);
      refreshSettings();
    };
    wrap.querySelector('#dlss5-profile-save').onclick = async () => {
      const name = await askText('Save profile', 'Name:', 'DLSS5 Video');
      if (!name) return;
      const r = await send({ cmd: 'profiles.save', name });
      setStatus(r.ok ? `Saved profile ${name}.` : `Save failed: ${r.error}`, !r.ok);
      refreshProfiles();
    };
    wrap.querySelector('#dlss5-profile-rename').onclick = async () => {
      if (!selected()) return;
      const name = await askText('Rename profile', 'New name:', selected());
      if (!name) return;
      const r = await send({ cmd: 'profiles.rename', name: selected(), newName: name });
      setStatus(r.ok ? `Renamed to ${name}.` : `Rename failed: ${r.error}`, !r.ok);
      refreshProfiles();
    };
    wrap.querySelector('#dlss5-profile-remove').onclick = async () => {
      if (!selected()) return;
      if (!await askConfirm('Remove profile', `Remove "${selected()}"?`)) return;
      const r = await send({ cmd: 'profiles.remove', name: selected() });
      setStatus(r.ok ? `Removed ${selected()}.` : `Remove failed: ${r.error}`, !r.ok);
      refreshProfiles();
    };
    wrap.querySelector('#dlss5-profile-video').onclick = async () => {
      const r = await send({ cmd: 'profiles.video_tuning' });
      setStatus(r.ok ? 'Applied video tuning (100% model, motion off, 8-bit).' : `Failed: ${r.error}`, !r.ok);
      refreshSettings();
    };
    wrap.querySelector('#dlss5-profile-default').onclick = async () => {
      if (!await askConfirm('Defaults', 'Reset all settings to defaults?')) return;
      const r = await send({ cmd: 'profiles.default' });
      setStatus(r.ok ? 'Defaults restored.' : `Failed: ${r.error}`, !r.ok);
      refreshSettings();
    };
    wrap.querySelector('#dlss5-profile-refresh').onclick = refreshProfiles;
    return wrap;
  };

  $('dlss5-tabs').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-tab]');
    if (!b) return;
    for (const x of $('dlss5-tabs').querySelectorAll('button')) x.classList.toggle('active', x === b);
    for (const p of pages.querySelectorAll('.dlss5-page')) p.style.display = p.dataset.tab === b.dataset.tab ? 'block' : 'none';
    if (b.dataset.tab === 'Profiles') refreshProfiles();
  });

  buildPages();

  // ---------- toggles / actions ----------
  bindToggle('dlss5-nr-toggle', () => nrOn, (v) => {
    nrOn = v;
    autoEnabled = false; setToggle($('dlss5-auto'), false);
    send({ cmd: 'set', key: 'enabled', value: v ? 1 : 0 }).then((r) => {
      setStatus(r && r.ok ? `Neural rendering ${v ? 'on' : 'off'}.` : 'Failed to toggle NR.', !(r && r.ok));
    });
    reportState(true);
  });
  bindToggle('dlss5-auto', () => autoEnabled, (v) => {
    autoEnabled = v;
    reportState().then(() => setStatus(v ? 'Auto NR on.' : 'Auto NR off.', false));
  });
  $('dlss5-helper-status') && ($('dlss5-helper-status').textContent = 'checking…');
  const helperStatus = $('dlss5-helper-status');
  const refreshHelper = async () => {
    const r = await send({ cmd: 'helper.status' });
    if (!r || !r.ok) { helperStatus.textContent = 'status unavailable'; return; }
    helperOn = r.running;
    setToggle($('dlss5-helper-toggle'), helperOn);
    helperStatus.textContent = r.running ? 'running' : 'stopped';
    helperStatus.style.color = r.running ? '#a5d6a7' : '#ff8a80';
  };
  bindToggle('dlss5-helper-toggle', () => helperOn, (v) => {
    send({ cmd: v ? 'helper.start' : 'helper.stop' }).then(async (r) => {
      setStatus(r && r.ok ? `Helper ${v ? 'started' : 'stopped'}.` : `Helper failed: ${r && r.error || 'unknown'}`, !(r && r.ok));
      await refreshHelper();
    });
  });
  $('dlss5-helper-restart').onclick = async () => {
    setStatus('Restarting helper...', false);
    const r = await send({ cmd: 'helper.restart' });
    setStatus(r && r.ok ? 'Helper restarted.' : `Helper failed: ${r && r.error || 'unknown'}`, !(r && r.ok));
    await refreshHelper();
  };
  bindToggle('dlss5-auto-restart', () => autoRestartEnabled, (v) => {
    autoRestartEnabled = v;
    setStatus(v ? 'Auto-restart Chrome on.' : 'Auto-restart Chrome off.', false);
  });
  $('dlss5-chrome-restart').onclick = async () => {
    setStatus('Restarting Chrome and reopening this video...', false);
    const r = await send({ cmd: 'chrome.restart', url: location.href });
    setStatus(r && r.ok ? 'Restarting...' : `Restart failed: ${r && r.error || 'unknown'}`, !(r && r.ok));
  };

  // ---------- refresh ----------
  const setControlValue = (key, raw) => {
    const c = controls.get(key);
    if (!c || raw == null) return;
    try {
      if (c.type === 'check') c.el.checked = Number(raw) !== 0;
      else if (c.type === 'choice') c.el.value = String(Math.round(Number(raw)));
      else {
        c.el.value = String(Number(raw));
        if (c.valEl) c.valEl.textContent = Number(raw).toFixed(2);
      }
    } catch (e) { /* a control from a newer/older build */ }
  };

  async function refreshSettings() {
    const s = await send({ cmd: 'settings' });
    if (s && s.ok && s.settings) {
      for (const [k, v] of Object.entries(s.settings)) setControlValue(k, v);
    }
  }

  async function refreshProfiles() {
    const r = await send({ cmd: 'profiles.list' });
    const list = panel.querySelector('#dlss5-profile-list');
    if (!list || !r || !r.ok) return;
    const current = list.value;
    list.innerHTML = '';
    for (const name of r.profiles) {
      const o = document.createElement('option');
      o.value = name; o.textContent = name; list.appendChild(o);
    }
    if (current && r.profiles.includes(current)) list.value = current;
  }

  let lastVideoSig = '';
  document.addEventListener('visibilitychange', () => reportState());
  const videoSig = () => {
    const s = videoState();
    const i = s.intrinsic ? `${s.intrinsic.w}x${s.intrinsic.h}:${s.intrinsic.fit}` : '';
    return `${s.visible}:${s.playing}:${i}:${s.swapW}x${s.swapH}:${s.rect ? `${s.rect.x},${s.rect.y},${s.rect.w},${s.rect.h}` : ''}`;
  };
  let tick = 0;
  let staleCount = 0;
  let restarting = false;

  let staleNotified = false;
  const showStaleWarning = () => {
    if (staleNotified) return;
    staleNotified = true;
    status.textContent = 'Extension was reloaded — refresh this tab to reconnect.';
    status.style.color = '#ff8a80';
  };

  async function refresh() {
    try {
    const sig = videoSig();
    lastVideoSig = sig;
    const s = videoState();
    const probe = await send({ cmd: 'ping' });
    if (probe && probe.staleContext) { showStaleWarning(); return; }
    reportState();
    // Send the ROI straight to the native host too. This keeps the swapchain size accurate even if
    // the background service worker is running a cached copy of its script.
    if (s.visible && s.playing && s.rect)
      send({ cmd: 'roi', rect: s.rect, swapW: s.swapW, swapH: s.swapH });
    const roiNote = panel.querySelector('#dlss5-roi-note');
    if (roiNote) {
      roiNote.textContent = s.rect
        ? `Source ${s.intrinsic ? s.intrinsic.w + '×' + s.intrinsic.h + ' ' + s.intrinsic.fit : '?'} · ROI ${s.rect.x},${s.rect.y} ${s.rect.w}×${s.rect.h} (window pixels, chrome offset applied)`
        : 'Enhanced only while a visible tab has playing video when Auto NR is on.';
    }
    const cfg = await send({ cmd: 'settings' });
    if (cfg && cfg.ok && cfg.settings && cfg.settings.enabled != null) {
      nrOn = Number(cfg.settings.enabled) !== 0;
      setToggle($('dlss5-nr-toggle'), nrOn);
    }
    if (++tick % 4 === 0) {
      refreshProfiles();
      refreshHelper();
    }
    const st = await send({ cmd: 'status' });
    if (st && st.ok) {
      const on = st.helper === 4 && st.modelUp === 1;
      const attached = st.layerAlive ? ' · layer attached to a presenter' :
                        (st.layerPid ? ' · layer PID is stale (presenter died)' : ' · no layer presenting');
      if (on && !st.layerAlive && s.playing && s.visible) {
        staleCount++;
        if (autoRestartEnabled && !restarting && staleCount >= 2) {
          restarting = true;
          setStatus('Layer stale — restarting Chrome and reopening this video...', false);
          const r = await send({ cmd: 'chrome.restart', url: s.url });
          if (!r || !r.ok) {
            restarting = false;
            setStatus(`Restart failed: ${r && r.error || 'unknown'}`, true);
          }
        } else if (!restarting) {
          setStatus('Layer stale — click Restart Chrome or disable Auto-restart.', true);
        }
      } else {
        staleCount = 0;
        setStatus(on ? `NR running${attached} · ${st.layerFrames} frames · ${st.width}×${st.height}` :
                       'Helper not running — start it with the DLSS5 Helper app.', !on || !st.layerAlive);
      }
    }
    } catch (e) {
      setStatus('Panel error: ' + (e && e.message || e), true);
    }
  }
  refresh();
  setInterval(refresh, 2500);
})();
