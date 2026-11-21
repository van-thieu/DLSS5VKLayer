#!/usr/bin/env python3
"""Native Messaging host for the DLSS5 browser extension.

Speaks Chrome/Firefox native messaging (4-byte little-endian length + JSON) on stdin/stdout and
translates the extension's verbs into the same shared-memory controls the Qt helper uses. `play`
hands a video URL to the running DLSS5 Video player over a local socket, or launches it.
"""
import json
import os
import socket
import subprocess
import sys
import time
import asyncio
import urllib.request
import websockets

UID = os.getuid()
SHM = os.environ.get("DLSSNR_SHM", f"/tmp/dlssnr-video-{UID}/shm.bin")
SHMCTL = os.path.expanduser("~/.local/bin/dlssnr-video-shmctl")
HELPER_CLI = os.path.expanduser("~/.local/bin/dlssnr-video-helper")
PLAYER = os.path.expanduser("~/.local/bin/dlssnr-gui-video")
APP_SOCK = f"/tmp/dlssnr-video/app-{UID}.sock"
SUPPRESS_FILE = f"/tmp/dlssnr-video/suppress-until-{UID}"
RESTART_FLAG = f"/tmp/dlssnr-video/last-restart-{UID}"
CHROME_CDP = "http://127.0.0.1:9222/json/version"
# Video profiles are deliberately separate from the desktop helper's game profiles. Removing or
# renaming a profile from the browser must never touch a game profile.
PROFILE_DIR = os.path.expanduser("~/.config/dlssnr/video-profiles")

# shmctl setting name -> the set_* key used by dlssnr_gui profile files.
PROFILE_KEYS = {
    "enabled": "set_enabled", "passes": "set_passes",
    "rebuildms": "set_rebuild_settle_ms", "workingscale": "set_model_resolution",
    "downscaler": "set_down_leg_filter", "detail": "set_detail_strength",
    "colour": "set_colour_strength", "guard": "set_highlight_guard",
    "transfer": "set_enlargement", "bypass": "set_composition_enabled",
    "preset": "set_preset", "style": "set_style", "intensity": "set_intensity",
    "localstructure": "set_local_structure", "localtone": "set_local_tone",
    "skinstructure": "set_skin_structure", "automask": "set_auto_mask",
    "sharpness": "set_sharpness", "mvec": "set_motion_enabled",
    "mvecquality": "set_motion_quality", "mvecunits": "set_motion_units",
    "mvecpixels": "set_mvec_pixels", "colourmode": "set_colour_mode",
    "hdrmode": "set_hdr_mode", "sdr16multipass": "set_sdr_16bit_multipass",
    "whitepointsource": "set_white_point_source", "whitepoint": "set_paper_white",
    "whitepointscale": "set_white_point_scale", "whitepointtrim": "set_white_point_trim",
    "applymodel": "set_apply_model", "reversible": "set_proxy",
    "debugview": "set_debug_view", "debugscale": "set_debug_scale",
    "compare": "set_compare", "comparesplit": "set_compare_split",
    "comparezoom": "set_compare_zoom", "compareswap": "set_compare_swap",
    "togglekey": "set_toggle_key",
}
KEY_TO_SHM = {v: k for k, v in PROFILE_KEYS.items()}


def read_msg():
    raw = sys.stdin.buffer.read(4)
    if len(raw) < 4:
        return None
    n = int.from_bytes(raw, "little")
    data = b""
    while len(data) < n:
        chunk = sys.stdin.buffer.read(n - len(data))
        if not chunk:
            return None
        data += chunk
    return json.loads(data.decode("utf-8"))


def send_msg(obj):
    data = json.dumps(obj).encode("utf-8")
    sys.stdout.buffer.write(len(data).to_bytes(4, "little") + data)
    sys.stdout.buffer.flush()


def shmctl(*args):
    try:
        p = subprocess.run([SHMCTL, SHM, *args], capture_output=True, text=True, timeout=5)
        return p.returncode == 0, (p.stdout or p.stderr).strip()
    except Exception as e:
        return False, str(e)


def parse_pairs(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def helper_ctl(action, timeout=30):
    if not os.path.isfile(HELPER_CLI):
        return False, "dlssnr-helper not found"
    try:
        p = subprocess.run([HELPER_CLI, action], capture_output=True, text=True, timeout=timeout)
        return p.returncode == 0, (p.stdout or p.stderr).strip()
    except subprocess.TimeoutExpired:
        return False, "timed out"
    except Exception as e:
        return False, str(e)


def restart_chrome(url):
    # A clean Browser.close is much kinder to the profile than SIGKILL. Spawn a detached relauncher
    # first (it waits for Chrome to exit), then ask Chrome to close over DevTools.
    try:
        last = float(open(RESTART_FLAG).read().strip())
        if time.time() - last < 90:
            return False, "restart throttled (90s cooldown)"
    except Exception:
        pass
    with open(RESTART_FLAG, "w") as f:
        f.write(str(time.time()))
    script = f"/tmp/dlssnr-video/restart-{UID}.sh"
    safe = str(url).replace("'", "'\\''")
    with open(script, "w") as f:
        f.write("#!/bin/bash\nsleep 3\n")
        f.write("nohup /home/vthieu/.local/bin/dlss5-chrome '" + safe +
                "' > /tmp/opencode/dlss5-restart.log 2>&1 &\n")
    os.chmod(script, 0o755)
    subprocess.Popen(["setsid", "bash", script],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)

    async def close_browser():
        try:
            ver = json.load(urllib.request.urlopen(CHROME_CDP, timeout=3))
            async with websockets.connect(ver["webSocketDebuggerUrl"]) as ws:
                await ws.send(json.dumps({"id": 1, "method": "Browser.close"}))
                try:
                    await asyncio.wait_for(ws.recv(), 3)
                except Exception:
                    pass
        except Exception:
            # If DevTools is gone, the detached relauncher still handles it after the old process exits.
            pass
    try:
        asyncio.run(close_browser())
    except Exception:
        pass
    return True, "chrome restart initiated"


def handle(msg):
    cmd = msg.get("cmd", "")
    if cmd == "chrome.restart":
        ok, text = restart_chrome(msg.get("url", ""))
        return {"ok": ok, "text": text}
    if cmd == "helper.status":
        ok, text = helper_ctl("status", timeout=8)
        return {"ok": ok, "text": text, "running": ok and not text.lower().startswith("helper not running")}
    if cmd == "helper.start":
        ok, text = helper_ctl("start", timeout=60)
        return {"ok": ok, "text": text}
    if cmd == "helper.stop":
        ok, text = helper_ctl("stop", timeout=15)
        return {"ok": ok, "text": text}
    if cmd == "helper.restart":
        ok, text = helper_ctl("restart", timeout=60)
        return {"ok": ok, "text": text}

    if cmd == "ping":
        return {"ok": True, "version": 1, "shm": SHM}

    if cmd == "status":
        ok, text = shmctl("status")
        if not ok:
            return {"ok": False, "error": text}
        s = parse_pairs(text)
        layer_pid = int(s.get("layer_pid", 0))
        return {
            "ok": True,
            "helper": int(s.get("helper_state", 0)),
            "modelUp": int(s.get("model_up", 0)),
            "layerPid": layer_pid,
            "layerAlive": bool(layer_pid) and os.path.isdir(f"/proc/{layer_pid}"),
            "layerFrames": int(s.get("layer_frames", 0)),
            "width": int(s.get("width", 0)),
            "height": int(s.get("height", 0)),
        }

    if cmd == "settings":
        ok, text = shmctl("settings")
        if not ok:
            return {"ok": False, "error": text}
        s = parse_pairs(text)
        keys = ["style", "intensity", "applymodel", "compare", "comparesplit",
                "workingscale", "passes", "mvec", "enabled"]
        return {"ok": True, "settings": {k: s.get(k) for k in keys}}

    if cmd == "suppress":
        try:
            ms = max(0, min(10000, int(msg.get("ms", 3000))))
            with open(SUPPRESS_FILE, "w") as f:
                f.write(str(time.time() + ms / 1000.0))
            ok, text = shmctl("set", "enabled", "0")
            return {"ok": ok, "error": None if ok else text, "untilMs": ms}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    if cmd == "set":
        key = str(msg.get("key", ""))
        value = msg.get("value")
        if key not in {"style", "intensity", "applymodel", "compare", "comparesplit",
                       "workingscale", "passes", "mvec", "enabled"}:
            return {"ok": False, "error": f"unknown key {key}"}
        if key == "enabled" and int(value) == 1:
            try:
                suppressed = time.time() < float(open(SUPPRESS_FILE).read().strip())
                if suppressed:
                    return {"ok": True, "suppressed": True}
            except Exception:
                pass
        ok, text = shmctl("set", key, str(value))
        return {"ok": ok, "error": None if ok else text}

    if cmd == "capture":
        ok, text = shmctl("capture", "4")
        return {"ok": ok, "error": None if ok else text}

    if cmd == "roi":
        with open("/tmp/dlssnr-roi.log", "a") as _log:
            _log.write(json.dumps(msg) + "\n")
        rect = msg.get("rect") or {}
        try:
            x = max(0, int(rect.get("x", 0)))
            y = max(0, int(rect.get("y", 0)))
            w = max(0, int(rect.get("w", 0)))
            h = max(0, int(rect.get("h", 0)))
        except Exception as e:
            return {"ok": False, "error": str(e)}
        try:
            swap_w = max(0, int(msg.get("swapW", 0)))
            swap_h = max(0, int(msg.get("swapH", 0)))
        except Exception:
            swap_w = swap_h = 0
        fields = [("roix", x), ("roiy", y), ("roiw", w), ("roih", h),
                  ("roienabled", 1 if w >= 64 and h >= 64 else 0)]
        # A cached background worker can still send roi without the swap size. Never overwrite a
        # known size with zero -- that would let the layer process the wrong swapchain again.
        if swap_w >= 64 and swap_h >= 64:
            fields += [("roiswapw", swap_w), ("roiswaph", swap_h)]
        for key, value in fields:
            ok, text = shmctl("set", key, str(value))
            if not ok:
                return {"ok": False, "error": text}
        return {"ok": True, "rect": {"x": x, "y": y, "w": w, "h": h}}

    if cmd == "profiles.list":
        try:
            os.makedirs(PROFILE_DIR, exist_ok=True)
            names = sorted(f[:-4] for f in os.listdir(PROFILE_DIR)
                           if f.lower().endswith(".ini") and os.path.isfile(os.path.join(PROFILE_DIR, f)))
            return {"ok": True, "profiles": names}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def profile_path(name):
        name = os.path.basename(str(name)).strip()
        if not name:
            raise ValueError("empty profile name")
        if not name.lower().endswith(".ini"):
            name += ".ini"
        return os.path.join(PROFILE_DIR, name)

    if cmd == "profiles.load":
        try:
            path = profile_path(msg.get("name", ""))
            values = {}
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    shm = KEY_TO_SHM.get(k.strip())
                    if shm:
                        values[shm] = v.strip()
            for shm, value in values.items():
                ok, text = shmctl("set", shm, value)
                if not ok:
                    return {"ok": False, "error": text}
            return {"ok": True, "name": os.path.basename(path)[:-4], "count": len(values)}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    if cmd == "profiles.save":
        try:
            path = profile_path(msg.get("name", ""))
            ok, text = shmctl("settings")
            if not ok:
                return {"ok": False, "error": text}
            values = parse_pairs(text)
            os.makedirs(PROFILE_DIR, exist_ok=True)
            with open(path, "w") as f:
                f.write("# DLSS5VKLayer settings\n")
                f.write("# Generated by DLSS5 browser extension\n\n")
                for shm, key in PROFILE_KEYS.items():
                    if shm in values:
                        f.write(f"{key}={values[shm]}\n")
            return {"ok": True, "name": os.path.basename(path)[:-4]}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    if cmd == "profiles.rename":
        try:
            src = profile_path(msg.get("name", ""))
            dst = profile_path(msg.get("newName", ""))
            if not os.path.isfile(src):
                return {"ok": False, "error": "profile not found"}
            if os.path.exists(dst):
                return {"ok": False, "error": "a profile with that name already exists"}
            os.rename(src, dst)
            return {"ok": True, "name": os.path.basename(dst)[:-4]}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    if cmd == "profiles.remove":
        try:
            path = profile_path(msg.get("name", ""))
            if not os.path.isfile(path):
                return {"ok": False, "error": "profile not found"}
            os.remove(path)
            return {"ok": True, "name": os.path.basename(path)[:-4]}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    if cmd == "profiles.default":
        ok, text = shmctl("reset")
        return {"ok": ok, "error": None if ok else text}

    if cmd == "profiles.video_tuning":
        tuned = {
            "enabled": 1, "workingscale": 1, "mvec": 0, "sdr16multipass": 0,
            "reversible": 0, "passes": 1, "style": 2, "intensity": 1.32,
            "localtone": 0.85, "localstructure": 1.8, "sharpness": 1,
            "automask": 1, "applymodel": 1, "compare": 0, "hdrmode": 0,
        }
        for shm, value in tuned.items():
            ok, text = shmctl("set", shm, str(value))
            if not ok:
                return {"ok": False, "error": text}
        return {"ok": True, "applied": list(tuned)}

    if cmd == "play":
        url = str(msg.get("url", "")).strip()
        if not url:
            return {"ok": False, "error": "empty url"}
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(2)
            s.connect(APP_SOCK)
            s.sendall(url.encode("utf-8"))
            s.close()
            return {"ok": True, "launched": False}
        except OSError:
            pass
        try:
            env = os.environ.copy()
            env.setdefault("DISPLAY", ":0")
            subprocess.Popen([PLAYER, "--video", url], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return {"ok": True, "launched": True}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    return {"ok": False, "error": f"unknown command {cmd}"}


def main():
    while True:
        try:
            msg = read_msg()
        except Exception:
            return
        if msg is None:
            return
        try:
            with open("/tmp/dlssnr-bridge.log", "a") as log:
                log.write(json.dumps({"cmd": msg.get("cmd"), "id": msg.get("id")}) + "\n")
        except Exception:
            pass
        try:
            resp = handle(msg)
        except Exception as e:
            resp = {"ok": False, "error": repr(e)}
        if "id" in msg:
            resp["id"] = msg["id"]
        send_msg(resp)


if __name__ == "__main__":
    main()
