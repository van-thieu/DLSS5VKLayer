# Game profiles

Ready-made DLSS5VKLayer settings for specific games. Each file is a profile as written by
`dlssnr_gui`, copied verbatim from `~/.config/dlssnr/profiles/`.

Profiles are grouped by the GPU they were tuned on, because the DLSS NR values that work well on
one card do not necessarily transfer to another.

| Folder     | GPU                | Notes                                      |
| ---------- | ------------------ | ------------------------------------------ |
| `rtx5090/` | GeForce RTX 5090   | Calibrated on an RTX 5090 (32 GB, driver 620.70.06) |

| Profile             | Game                 |
| ------------------- | -------------------- |
| `AION2.ini`         | Aion 2               |
| `ArcheAgeChron.ini` | ArcheAge Chronicles  |
| `Marvel Rivals.ini` | Marvel Rivals        |

## Installing a profile

Copy the `.ini` into the folder the GUI reads, then pick it from the profile dropdown:

```bash
cp game_profiles/rtx5090/AION2.ini ~/.config/dlssnr/profiles/
```

The GUI only looks at `.ini` files sitting directly in `~/.config/dlssnr/profiles/` — it does not
read subdirectories — so the folder layout here is for organising the repository, not the runtime.

Then restart `dlssnr-gui` and pick the profile from the dropdown. The dropdown is filled once at
startup, so a file copied in while the GUI is running only shows up after a restart.

## Updating a profile

Edit the settings in the GUI and press **Save**. The dialog pre-fills the name of the selected
profile, so keeping that name overwrites the file in place. Then copy it back over the one in this
folder so the repository keeps the latest version:

```bash
cp ~/.config/dlssnr/profiles/AION2.ini game_profiles/rtx5090/
git add game_profiles && git commit -m "profiles: refresh AION2"
```

## Tuning for another GPU

The values in a profile are tuned for the card it was measured on. To keep a set for a different
GPU, add a folder named after it and copy the profiles you retune into it:

```bash
mkdir -p game_profiles/<gpu>
cp ~/.config/dlssnr/profiles/AION2.ini game_profiles/<gpu>/
```

Keep the folder name the same as the GPU so it stays obvious which set applies where.

## Format

The files are plain `key=value` lines and hold only DLSS NR settings — no paths, no machine
identifiers. A profile is written in full on every save, so a refresh replaces the whole file
rather than patching individual values.
