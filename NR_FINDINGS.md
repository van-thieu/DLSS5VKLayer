# DLSS 5 NR findings (Where Winds Meet / Proton)

## Working configuration (active)

- Model: NGX **310.8** `nvngx_dlssnr.dll` (`e16bcf15e16e13f5…`, 165,840,496 bytes)
- NGX core: original 489 KB `nvngx.dll` wrapper (`f3b96176…`)
- Helper: direct-NGX path, no Streamline host in the installed binary
- Driver: 620.70.10
- Live shm settings: `enabled=1`, `applymodel=1`, `passes=3`,
  `intensity=0.7228`, `workingscale=1.5`, `bypass=0`, `compare=0`
- Verified: three `CreateFeature(18)` calls succeed; helper and layer frame
  counters advance together.

Backups:
- `~/.local/lib/dlssnr/helper/dlssnr_helper.exe.bak-before-appid-20261127`
- `~/.local/share/dlssnr/binaries/nvngx.dll.bak-wrapper-489k`
- `~/.local/share/dlssnr/binaries/nvngx_dlssnr.dll.bak-3108-20261127`

## Native in-game toggle

WWM ships NR config entries (`EnableDLSSNR`, `DLSSNRControlMask`, intensity/
tone/structure keys) and the notice text, but in this Proton session the game
never loads `sl.dlss_nr.dll` or `nvngx_dlssnr.dll` (confirmed via `/proc/<pid>/maps`).
No in-game Neural Rendering section is rendered. The gate is server-side or a
runtime capability check that Proton/vkd3d cannot satisfy; it is not a saved
user setting.

## NGX 310.9 (WWM model, 266 MB)

Differences from 310.8:
- Adds networks `hnet-almond-eagle-11m` and `hnet-crazy-cuckoo-small-fp8`,
  plus `dlssnr_guided_high_source_scratch`
- Adds `SetGPUArchFromCore` / `NvAPI_GPU_GetArchInfo` based network selection
- Adds parameter `DLSSNR.MaskBlendMode`
- Requires a real NGX-allocated parameter block; a hand-rolled `OwnParam`
  block leaves the feature with no callbacks and produces garbage/flashing

Forcing `GB200`/driver overrides in the helper, SDK 0x15, app ID `0x24480451`,
and the real 2 MB `_nvngx.dll` got core init + `AllocateParameters` working,
but the image still did not render correctly.

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

Two v4 UUID candidates found in `sl.dlss_nr.dll` `.rdata`:
- `0x48988`: `{A8B865DD-2E3D-4094-AD97-E593A70C75D6}`
- `0x49010`: `{62AB5D82-FDC1-4DC3-A9DD-070D1D495D97}`

The 80-entry table in `sl.common.dll` beginning with `30 82 01 0A 02 82 01 01 00`
is a DER RSA-2048 public key / certificate table, **not** StructType GUIDs.

## Streamline host work

Preserved on branch `streamline-host-wip`:
- `core/sl_host.cpp/.h` — loads WWM's `sl.interposer.dll`, `slInit` with
  `kFeatureDLSS_NR`, `slSetVulkanInfo`, `slSetConstants`, resource tags,
  `slEvaluateFeature`
- SDK headers vendored under `third_party/streamline/include`
- Wiring in `ngx_snippet.cpp/.h`, `helper/main.cpp`, `windows/meson.build`

Status: initializes successfully (`slInit=0`, requirements `flags=6`,
`slDLSSNRSetOptions` resolved), but `slEvaluateFeature` returns
`eErrorMissingConstants` (26) until the private `DLSSNROptions` structType GUID
is supplied. The remaining step is a runtime `slSetData` RCX dump from a live
game/helper session.

## Restore / apply working settings

```bash
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set enabled 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set applymodel 1
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set passes 3
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set intensity 0.7228
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set workingscale 1.5
dlssnr-shmctl /tmp/dlssnr-1000/shm.bin set bypass 0
```
