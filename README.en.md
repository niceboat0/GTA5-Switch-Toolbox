# GTA5-Switch-Toolbox

A multi-purpose toolbox NRO for the GTA V Switch homebrew port (Title ID `0100b00b51230000`) —
DLC management, script MOD install/restore, graphics presets and builtin mods, all in one .nro.

English | [中文](README.md)

## ⚠️ Read this first: back up update.rpf AND update2.rpf

**A backup is strongly recommended.** Any RPF write (mod install / uninstall /
stock restore / perf tuning write) carries a small risk:

```
SDcard:/atmosphere/contents/0100b00b51230000/romfs/update/update.rpf     (~900 MB)
SDcard:/atmosphere/contents/0100b00b51230000/romfs/update/update2.rpf    (~700 MB)
```

Copy **both** files somewhere safe (PC or another folder on the SD card).
Restoring them recovers everything:
- **update2.rpf**: script mods live here (script_rel.rpf)
- **update.rpf**: gameconfig (perf tuning) and dlclist.xml (DLC registration)

The in-tool "Restore stock scripts" only restores 3 script entries —
**it is not a substitute for a full backup**.

## Features (v6.2)

- **DLC tab**: register/uninstall add-on vehicle DLCs (dual registration in
  dlclist.xml + extratitleupdatedata.meta)
- **Scripts tab**: install .nsc script mods (auto-detects official RSC7 format vs raw mod
  format), one-tap stock script restore, 4 builtin mods with mutual-exclusion checks
- **Graphics tab**: one-tap settings.xml presets (low/medium/high/extreme-OC)
- **Perf tab**: gameconfig tuning (settled: the bottleneck is the Tegra X1 —
  overclocking is the real fix)
- **Tools tab**: 5 story progress saves (4% ~ 100%), info view

## Usage

### Install & launch

1. Download `gta5save.nro` from Releases, copy it to `/switch/` on the SD card
2. Game files must be at `atmosphere/contents/0100b00b51230000/romfs/`
3. Launch via hbmenu (hold R while starting the game, or close the game and open hbmenu)
4. In-tool: **L/R switch tabs**, D-pad to navigate, A confirm, B back/exit

### Scripts tab (core feature)

**Install a builtin mod**: pick "*Mod Mgr" → a submenu opens (3 stock restore entries +
4 builtin mods) → select one → it lists the RPF entries it will overwrite → confirm.
Mutually exclusive mods (e.g. the two ragemenu-based ones) are flagged before install.

**Install external mods**: put .nsc files into `/switch/gta5save/script/` on the SD card
(auto-created on first visit; dropping them directly in `/switch/gta5save/` also works),
then select one in the list and press A. Both official (RSC7) and raw mod formats are
auto-detected.

**Restore stock**: in Mod Mgr, pick a stock script (error_listener /
achievement_controller / shop_controller) and restore it individually.

**In-game mod menu hotkeys**: after installing, press **L + D-pad Down** (MEGATARD/ragemenu)
or **B + D-pad Right** (Hot Coffee) in game.

### DLC tab

**Directories**:
- **Installed** (read by the game): `/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/`
- **Pending import** (drop dlcpack folders here): `/switch/gta5save/dlc/` (recommended).
  Also scanned: `/switch/gta5save/`, `/switch/GTA5DLC/`, `/dlc/`, `/gta5dlc/`, `/switch/`

**How to**: put the ported dlcpack folder (containing `dlc.rpf`) into the import folder,
press **Y** on the DLC tab to switch to "Pending", select and press **A** to import
(auto-copies to dlcpacks/ + registers in dlclist.xml + extratitleupdatedata.meta).
PC-format packs are auto-converted on import; packs flagged "will crash" should not be
imported directly. Add one pack at a time and reboot to verify.

### Graphics tab

Select a preset and press A to apply the settings.xml:
- **Low (stock clocks)**: for non-overclocked consoles
- **Medium (default)**: balanced choice after overclocking
- **High (extreme OC only)**: CPU 1963 / GPU 768 / RAM 2666 and above
- **VSync off variants**: aggressive framerate-first options

### Tools tab

- **Builtin saves**: press ZL, 5 progress levels (4% / 20% / 31.6% / 61.1% / 100%)
- **Info**: current mod status, RPF free space, etc.

## Builtin mods (romfs/builtin_mods/)

| Mod | Author | Slot |
|---|---|---|
| MEGATARD v0.9.9 (ZH) | Geekmaxxer | ragemenu |
| Classic ragemenu v13 (ZH) | maritoguionyo | ragemenu (mutually exclusive with the above) |
| Hot Coffee + Zombies | Je11yb0ne/CinnamonCoffee | simple_zombies |
| Hot Coffee (no zombies) | Je11yb0ne/CinnamonCoffee | simple_zombies (mutually exclusive with the above) |

Stock scripts (romfs/stock_scripts/) were extracted from a pristine build-2699
update2.rpf, in RSC7 resource format.

## Build

Requires the devkitPro A64 toolchain:

```bash
make
```

Produces `gta5save.nro`; copy it to `/switch/gta5save.nro` on the SD card and launch
from hbmenu.

## Credits

- **[Geekmaxxer](https://github.com/Geekmaxxer)** — [MEGATARD GTA5-NX Menu SDK](https://github.com/Geekmaxxer/GTA5NX-MG-Menu)
  (author of MEGATARD v0.9.9), the [GTA5-NX-Tutorial](https://geekmaxxer.github.io/GTA5-NX-Tutorial/)
  site, and [RPF Radio Editor](https://github.com/Geekmaxxer/gta-radio-editor)
- **[Je11yb0ne](https://github.com/Je11yb0ne)** — [HotCoffee-NX](https://github.com/Je11yb0ne/HotCoffee-NX)
  (the Switch port of CinnamonCoffee, with and without zombies)
- **maritoguionyo** — the classic ragemenu v13 Switch port (no public repo, Discord release)
- **[ShinyWasabi](https://github.com/ShinyWasabi)** — [RageMenu](https://github.com/ShinyWasabi/RageMenu)
  (the menu base the classic ragemenu was built on)
- The GTA V Switch port community (Geekmaxxer's tutorial site, GBAtemp)
