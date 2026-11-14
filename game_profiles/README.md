# Game profiles

Ready-made DLSS5VKLayer settings for specific games. Each file is a profile as written by
`dlssnr_gui`, copied verbatim from `~/.config/dlssnr/profiles/`.

| Profile            | Game             |
| ------------------ | ---------------- |
| `AION2.ini`        | Aion 2          |
| `ArcheAgeChron.ini`| ArcheAge Chronicles |
| `Marvel Rivals.ini`| Marvel Rivals   |

## Installing a profile

Copy the `.ini` into the folder the GUI reads, then pick it from the profile dropdown:

```bash
cp game_profiles/AION2.ini ~/.config/dlssnr/profiles/
```

Then restart `dlssnr-gui` and pick the profile from the dropdown. The dropdown is filled once at
startup, so a file copied in while the GUI is running only shows up after a restart.

## Updating a profile

Edit the settings in the GUI and press **Save**. The dialog pre-fills the name of the selected
profile, so keeping that name overwrites the file in place. Then copy it back over the one in this
folder so the repository keeps the latest version:

```bash
cp ~/.config/dlssnr/profiles/AION2.ini game_profiles/
git add game_profiles && git commit -m "profiles: refresh AION2"
```

## Format

The files are plain `key=value` lines and hold only DLSS NR settings — no paths, no machine
identifiers. A profile is written in full on every save, so a refresh replaces the whole file
rather than patching individual values.
