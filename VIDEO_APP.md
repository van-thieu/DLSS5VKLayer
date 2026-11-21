# DLSS5 Video

A companion to the DLSS5VKLayer Helper: a player for TikTok/Douyin and local video whose final
frame goes through the **same** Vulkan layer, NGX Feature 18 model, helper, settings and composition
that games use. It is not a second enhancement pipeline.

## Launch

- Desktop entry: **DLSS5 Video** (Audio/Video). It opens **two windows**: the settings panel and a
  separate player. Keep the player visible (tick **On top**) while changing settings in the other
  window — the video never hides behind a tab.
- Command line: `dlssnr-gui-video --video '<url or file>'`

`Chrome for DLSS5 Video` is a second desktop entry: it launches Chrome with
`--remote-debugging-port=9222 --remote-allow-origins="*"`, which the player's **Grab browser video**
button needs. Quit your normal Chrome first, or the new flags are ignored by the already-running
instance.

## Browser extension

`browser-extension/chrome/` and `browser-extension/firefox/` contain an unpacked extension for each
browser. Load it with *Developer mode → Load unpacked* (`chrome://extensions` or
`about:debugging#/runtime/this-firefox`). The Chrome build carries a fixed key, so its ID is
`habnnghdbobdldgenhnnghcinaegpiif`; the native host manifest already allows it. The Firefox build
uses `24d3d842-83df-5342-9b6f-dcfd0efb52ba`.

A native messaging host (`browser-extension/host/dlssnr_bridge.py`) is registered for both browsers
under `com.dlssnr.bridge`. It talks to the same shared memory via `dlssnr-shmctl` and starts/stops
the helper through `dlssnr-helper`, so the desktop GUI is **not required** for video: the
extension's **Helper → Start / Restart / Stop** buttons manage the model process, and the same
**Auto** toggle can keep NR on only while a visible tab has playing video.

The panel is hidden at startup. Click the extension's toolbar icon to show/hide it; the background
content script keeps reporting video state for gating even while the panel is hidden.

**Profiles are segregated.** The extension reads and writes only
`~/.config/dlssnr/video-profiles/`; the desktop helper keeps its game profiles in
`~/.config/dlssnr/profiles/`. Removing or renaming a profile in the browser can no longer affect a
game profile. Once installed, a floating **DLSS5 Neural Rendering** panel appears on video pages with:

- **Enhance this video** — sends the page's `<video>` URL to the running DLSS5 Video player (or
  launches it), via a local socket. The enhanced video plays in that window, not in Chrome.
- Style, Intensity, Show original (A/B), Compare + Split, Capture A/B frames.
- Live status (helper running, frames, resolution).

The panel cannot change Chrome's own video surface: neither Chrome nor Firefox presents through
Vulkan, so no extension can inject the layer into the page. It drives the desktop player and the
shared settings instead. For a `blob:` (MSE) source the panel tells you to use the app's
**Grab browser video** CDP path, which reads the stream URL out of the tab.

## Enhancing Chrome itself (Vulkan presentation)

Chrome can be made to present its final composited frame through Vulkan on this machine:

```
google-chrome-stable --ozone-platform=x11 --use-angle=vulkan \
  --enable-features=Vulkan,SkiaGraphite ... 
```

with `VKLayer_DLSS5=1 VK_INSTANCE_LAYERS=VK_LAYER_NV_dlssnr` in the environment. The
**Chrome DLSS5 (Vulkan)** desktop entry does exactly that (and keeps the DevTools port for the
extension). Quit any running Chrome first, then launch that entry. Verified here:

```text
[dlssnr-layer] swapchain 945x1028 fmt=44
[dlssnr-layer] [comp] building 945x1028, model 1266x1378
[dlssnr] [ngx] STATUS: Feature=18 created=true ... size=1266x1378
```

The layer then enhances the whole Chrome window in place, exactly as it does a game. Practical
notes:

- The layer has no UI mask. Use **F11 / Fullscreen video** in the extension so the window contains
  only the video; otherwise page text and browser chrome are enhanced too.
- The launcher applies a Chrome-specific cost profile on every start: model resolution 100%,
  motion vectors off, 16-bit intermediates off, reversible proxy 0. With the game profile's 134%
  and quality optical flow the helper runs a 2573×1447 model with 14 MB crossing each way per frame,
  which makes Chrome visibly lag. Chrome hardware video decode is also disabled in the launcher;
  it caused a GPU-process `SIGTRAP` (`Bad file descriptor`) with the layer.
- The extension's **NR on (this page)** / **NR off** buttons write `enabled` in the shared header,
  so you can keep the pass off while browsing and on only while watching.
- For a lower-overhead Chrome window, run `dlss5-chrome --app="<url>"`: app mode gives Chrome one
  tab and one main swapchain instead of the many swapchains a full browser session creates.
- An occasional "present fence still pending; swapchain passes through" is the layer's fail-open
  path; it is not fatal and Chrome keeps presenting.
- The normal `google-chrome.desktop` (ANGLE GL) does not load the layer. This is a separate entry.

## Browser video (CDP grab)

**Grab browser video** asks Chrome's DevTools which tab is playing a `<video>`, reads its URL, and
loads it into the player together with the page's Referer/User-Agent/Cookie headers. If the site
uses Media Source Extensions (a `blob:` src), the player watches the tab's network requests for
`.m3u8`/`.mp4` and uses the first master playlist it sees. This is how to watch sites yt-dlp does not
support (e.g. anime streaming sites) through the layer. The video is then played by mpv in our own
window; the browser tab is only the source of the URL.

Chrome cannot be enhanced in place like a game: its compositor presents through GL, not Vulkan, so
the layer never sees those frames. The grab path is the reliable equivalent.

The app starts mpv embedded in its own preview area with `--wid`, under X11 (XWayland on a Wayland
session) and `VKLayer_DLSS5=1`. mpv's Vulkan output creates the swapchain the layer intercepts, so
the frame path is identical to a game: capture -> proxy encode -> shared memory/dma-buf -> helper ->
Feature 18 -> composition -> present. Video is enhanced at the **displayed** resolution; there is no
output upscaling and no deblocking/denoising pass.

Requirements: the implicit layer installed (`~/.local/share/vulkan/implicit_layer.d/`), the helper
running with `nvngx_dlssnr.dll` imported, and `mpv` + `yt-dlp` (both from Ubuntu):
`sudo apt install mpv yt-dlp`.

## Using it

- Paste a TikTok/Douyin link (share text is fine; the first URL is extracted) or a local file, then
  **Play**. `Browse...` and `Paste` are shortcuts. The audio comes from mpv, so sync is mpv's.
- Transport: Pause/Play, seek, volume, and Loop are wired to mpv over its JSON IPC.
- **Portrait 9:16** caps the preview to a phone-shaped area for short-form video.
- The other tabs are the game settings, unchanged. Changes apply live; the helper rebuilds the
  feature when a latched setting changes. Style/Intensity are also in the quick panel.
- **Bypass NR (show original)** is an instant A/B: the whole pass still runs, only the picture
  differs. **Compare** (off / side-by-side / wipe) shows edited vs original in the same frame; use
  the wipe and move **Split**.
- **Capture A/B** writes four matched `before_NN.png` / `after_NN.png` pairs plus a manifest to
  `~/.local/state/dlssnr/captures`; **Open folder** opens it. That is the generated NR output you
  can inspect side by side.
- **Player log** shows mpv and layer output if a source will not play.

### Douyin

Douyin rejects anonymous yt-dlp requests. Log into Douyin in Chrome, then tick **Use Chrome
cookies (Douyin)** (toggling restarts the player), or export a `cookies.txt` and use **Cookies
file...**. mpv passes these to yt-dlp via `--ytdl-raw-options`. TikTok works without either.

### The two links that failed

- `anikototv.bz/...`: yt-dlp has no extractor for that site (`Unsupported URL`). Use **Grab browser
  video** from a Chrome tab playing it.
- YouTube: the extractor works (a JS runtime is now passed, `js-runtimes=node`, and a current
  standalone yt-dlp is installed in `~/.local/bin`), but Googlevideo returned `403 Forbidden` for
  this machine's network when fetching the media URL. That is upstream/account/network-side; a
  different network or Chrome cookies may be needed. The player reports the real HTTP error instead
  of failing silently.

### Tuning notes

- Model resolution 100% is the strict same-resolution behaviour. The shipped game profile uses
  134%, which is *internal* supersampling followed by a downscale; it costs more and is optional.
- Motion vectors are estimated by the helper with `VK_NV_optical_flow`; reset on scene cuts is
  bounded to the host transport, so if cuts ghost, set `DLSSNR_DMABUF=0` for this player.
- DRM content is not supported. Netflix/Disney+ and other EME streams stay in the browser, which
  does not present through the layer.
- Closing the video app leaves the helper running; the settings GUI keeps its stop-on-close habit.

## Source

`gui/videopage.{h,cpp}` (player, transport, A/B, capture), `gui/mainwindow.{h,cpp}` (inserts the
Video tab; `--video` mode leaves the helper running), `gui/main.cpp` (args + X11/layer env),
`gui/meson.build`, `gui/dlssnr_gui.pro`. Built with `ninja -C build/native gui/dlssnr_gui` and
installed to `~/.local/bin/dlssnr-gui-video`.
