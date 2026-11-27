# DLSS 5 NR findings (Where Winds Meet / Proton)

This document records what was learned while getting DLSS 5 Neural Rendering to run
under Proton for Where Winds Meet. It is intentionally explicit about what works,
what does not, and which numbers mean what.

## Status at a glance

| Item | Status |
|---|---|
| NR executes on Linux/Proton | Yes — NGX feature 18 is created in the Proton helper |
| Helper loads `nvngx_dlssnr.dll` | Yes |
| Vulkan layer attaches to WWM | Yes (`layer_pid` non-zero after the v22 fix) |
| Frames cross the layer/helper boundary | Yes |
| Composed image visible in-game | **Unsolved** — see "Open problems" below |
| Native in-game NR toggle | Does not appear on Linux/Proton |

NR is **not** Windows-only. The helper is a Windows PE running under Proton/Wine and
creates NGX features successfully; the layer is a native Linux `.so`. The unsolved
part is presentation, not execution.

## Active configuration

- Model: NGX **310.9** `nvngx_dlssnr.dll` (`2bc19bc09…`, 266 MB, from WWM's
  `Engine/Binaries/Win64r/Streamline/`)
- NGX core: real 2 MB `_nvngx.dll` copied into `binaries/nvngx.dll` (`1bad5e68…`)
- NGX init: SDK `0x15`, app ID `0x24480451`, `DLSSNR.UICorrection=1`
- Shared-memory protocol: **version 22** (`kShmVersion`) — required after adding
  four fields to `ShmHeader`
- Driver: 620.70.10; GPU: RTX 5090 (`dxvk_device=2b85`)
- Helper: direct-NGX path; the Streamline host lives on `streamline-host-wip`

Backups of the known-good 310.8 state:
- `~/.local/lib/dlssnr/helper/dlssnr_helper.exe.bak-before-appid-20261127`
- `~/.local/share/dlssnr/binaries/nvngx.dll.bak-wrapper-489k`
- `~/.local/share/dlssnr/binaries/nvngx_dlssnr.dll.bak-3108-20261127`

## WWM's default NR parameters

Extracted from `Engine/Config/GlobalOptions.bin` and mapped to the model's real
NGX parameter names (`DLSSNR.*` strings in `nvngx_dlssnr.dll`):

| WWM setting | Default | Real NGX key | Meaning |
|---|---|---|---|
| `EnableDLSSNR` | `0` | — | engine default; server flag turns it on |
| `DLSSNRIntensity` | `1.0` | `DLSSNR.Intensity` | model strength |
| `DLSSNRLocalToneStrength` | `1.0` | `DLSSNR.LocalToneStrength` | tone strength |
| `DLSSNRLocalStructureStrength` | `1.0` | `DLSSNR.LocalStructureStrength` | structure strength |
| `DLSSNRSkinStructureStrength` | `1.0` | `DLSSNR.SkinStructureStrength` | skin strength |
| `DLSSNRStyle` | `0` | `DLSSNR.Style` | 0=Default (standard), 1=Natural, 2=Cinematic (community names) |
| `DLSSNRPreset` | `0` | `DLSSNR.Hint.Render.Preset` | 0=Default, 1-3 presets; different scale from DLSS SR presets |
| `DLSSNRUseAutoMask` | `0` | `DLSSNR.UseAutoMask` | boolean |
| `DLSSNRMaskBlendMode` | `0` | `DLSSNR.MaskBlendMode` | 0=default; NVIDIA publishes no other names |
| `DLSSNRPerformanceMode` | `6` | **not an NGX key** | `sl::DLSSMode` value 6 = `eDLAA`; plugin accepts 3=`eMaxQuality` and 6=`eDLAA` |
| `DLSSNRGlobalToneStrength` | `1.0` | **not an NGX key** | WWM-side composition setting; OptiScaler deliberately does not write it |
| `DLSSNRModelMask` | `0xFFFFFFFF` | **not an NGX key** | WWM-side bitmask, all bits set |

`sl::DLSSMode` enum (from public `sl_dlss.h`):
`0=eOff, 1=eMaxPerformance, 2=eBalanced, 3=eMaxQuality,
4=eUltraPerformance, 5=eUltraQuality, 6=eDLAA`.

**Important:** only `DLSSNR.MaskBlendMode` is a real NGX parameter among the four
extra settings we added. `PerformanceMode`, `ModelMask` and `GlobalToneStrength`
are WWM-side settings and writing them to the NGX parameter block is a no-op.
The GUI documents this.

## GUI control types

Controls are typed by the data they represent:
- booleans / on-off → checkbox toggle
- enums → dropdown (Style, Preset, Performance mode, Mask blend mode, Motion
  quality/units/pixel size, HDR input, Down-leg filter, Transfer, Colour mode,
  White point, Proxy, Debug view, Compare)
- numeric values → slider with an editable number beside it

Adding `DLSSNRPerformanceMode`, `DLSSNRMaskBlendMode`, `DLSSNRGlobalToneStrength`
and `DLSSNRModelMask` to `ShmHeader` shifted every later field by 16 bytes;
`kShmVersion` was bumped 21 → 22 and all three processes (layer `.so`, helper
`.exe`, GUI + shmctl) must be rebuilt together. Running WWM must be restarted to
load the new layer, otherwise `layer_pid` stays 0 and no frames are composed.

## NGX 310.9 vs 310.8

310.9 (266 MB) differs from 310.8 (166 MB):
- adds networks `hnet-almond-eagle-11m` and `hnet-crazy-cuckoo-small-fp8`
- adds internal buffer `dlssnr_guided_high_source_scratch`
- adds `SetGPUArchFromCore` / `NvAPI_GPU_GetArchInfo` network selection
- adds NGX key `DLSSNR.MaskBlendMode`
- requires a real NGX-allocated parameter block — a hand-rolled `OwnParam` block
  leaves the feature with no callbacks and produces garbage/flashing

Core init + `AllocateParameters` succeeded with the real 2 MB `_nvngx.dll`, SDK
0x15, app ID `0x24480451` and `DXVK_NVAPI_GPU_ARCH=GB200` reported to the helper.

## Private `DLSSNROptions` layout (from `sl.dlss_nr.dll` disassembly)

| Offset | Type | Field |
|---|---|---|
| `+0xd0`–`+0xdc` | uint32 ×4 | BidirectionalDistortionField subrect |
| `+0xe0`, `+0xe4` | uint32 | width / height |
| `+0xf8` | float | `DLSSNR.Intensity` |
| `+0xfc` | float | `DLSSNR.LocalToneStrength` |
| `+0x100` | float | `DLSSNR.LocalStructureStrength` |
| `+0x108` | uint32 | `DLSSNR.Style` |
| `+0x110` | uint8 | `DLSSNR.UseAutoMask` |
| `+0x114` | float | `DLSSNR.SkinStructureStrength` |
| `+0x11c` | uint32 | `DLSSNR.MaskBlendMode` |
| `+0x122` | uint8 | `DLSSNR.UICorrection` |

`sl.dlss_nr.dll` exports only `DllMain` and `slGetPluginFunction` — no
`slSetData` export and no hardcoded 16-byte GUID compare. The struct type is
resolved through the SL core registry.

Two v4 UUID candidates in `sl.dlss_nr.dll` `.rdata`:
- `0x48988`: `{A8B865DD-2E3D-4094-AD97-E593A70C75D6}`
- `0x49010`: `{62AB5D82-FDC1-4DC3-A9DD-070D1D495D97}`

Passing either directly to `slDLSSNRSetOptions` crashes (`0xC0000005`) under all
tested signatures — the inline SDK helper supplies a client context we cannot
obtain publicly.

The 80-entry table in `sl.common.dll` beginning with `30 82 01 0A 02 82 01 01 00`
is a DER RSA-2048 public key / certificate table, **not** StructType GUIDs.

## Streamline host work

Preserved on branch `streamline-host-wip`:
- `core/sl_host.cpp/.h` — loads WWM's `sl.interposer.dll`, `slInit` with
  `kFeatureDLSS_NR`, `slSetVulkanInfo`, `slSetConstants`, resource tags,
  `slEvaluateFeature`
- SDK headers vendored under `third_party/streamline/include`
- Wiring in `ngx_snippet.cpp/.h`, `helper/main.cpp`, `windows/meson.build`

Status: initializes (`slInit=0`, requirements `flags=6`, `slSetVulkanInfo=0`,
`slDLSSNRSetOptions` resolved) but `slEvaluateFeature` returns
`eErrorMissingConstants` (26) until the private `DLSSNROptions` structType GUID
and client context are supplied. The remaining step is a runtime `slSetData` RCX
dump from a live game/helper session.

## Open problems

1. **Composed output not visible.** With `bypass=1` the raw model answer produced
   pixelated flashes; with `bypass=0` the composed frame showed no visible
   change. Both model versions behave this way, so this is a
   composition/presentation problem, not a model-version problem.
2. **Per-pass override checkbox semantics.** The "Use per-pass settings" box only
   toggles `kPassEnabled` (bit 31); the individual field checkboxes keep bits
   0-7, so unchecking the master box does not disable per-pass values (WWM.ini
   used `0xFF`). The GUI now exposes each field explicitly.
3. **DLSS5Video vs game layer conflict.** `VK_LAYER_NV_dlssnr_video` and
   `VK_LAYER_NV_dlssnr` are both global implicit layers with the same
   `enable_environment`. The video manifest was moved to
   `~/.local/share/vulkan/implicit_layer.d/disabled/` so only the game layer loads.

## Restore / apply settings

```bash
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set enabled 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set applymodel 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set passes 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set intensity 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set localtone 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set localstructure 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set skinstructure 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set automask 0
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set style 0
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set preset 0
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set perf_mode 6
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set mask_blend 0
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set model_mask 4294967295
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set global_tone 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set workingscale 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set bypass 0
```

Profiles: `~/.config/dlssnr/profiles/WWM-defaults.ini` (WWM values from
`GlobalOptions.bin`) and `WWM.ini`.
