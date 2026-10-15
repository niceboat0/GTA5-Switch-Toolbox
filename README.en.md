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

## Features and controls (v7.0)

Native 1280×720 interface with a fixed sidebar, overview, segmented libraries,
selection details and one action bar. Chinese and English follow the system language.
Controller and touch share the same actions. No extra UI libraries are required.

Copy `gta5save.nro` to `/switch/` and launch it from hbmenu. Choose a workspace from
Overview or the sidebar. Select an item, review its details, and confirm before writing.

| Action | Controller | Touch |
|---|---|---|
| Switch workspace | L / R | Sidebar |
| Select an item | D-pad or either stick up/down | Tap a row (selection only) |
| Change category | Up from the first row, then left/right; Down enters the list | Category header |
| Use the action bar | X, left/right, A; X or Up returns to the list | Tap a button |
| Apply selection | A, then A on the confirmation | Primary button, then Confirm |
| Adjust values | ZL / ZR in graphics fine-tuning or advanced parameters; hold to repeat | Decrease / Increase (graphics) |
| Overview / cancel | B | Overview / Cancel |
| Exit | +; unsaved changes require confirmation | — |

**Script mods:** Built-in mods, External scripts and Restore stock are separate categories.
Select a bundled mod directly to install it. Put external `.nsc` files in
`/switch/gta5save/script/` and Rescan. Existing conflict detection and read-back verification
are preserved. In-game menu: **L + D-pad down** for MEGATARD/ragemenu;
**B + D-pad right** for HotCoffee. Removing an entry does not automatically restore stock.

**DLC library:** Installed handles registration, conversion, HD-texture hiding and deletion.
Ready to import scans folders containing `dlc.rpf` under `/switch/gta5save/dlc/`.
Installed packs live under
`/atmosphere/contents/0100b00b51230000/romfs/update/switch/dlcpacks/`.
PC-format packs are labelled and blocked from direct import. Convert them to Switch format first.
Inspect opens the resource report.

**Graphics:** Presets open by default with stock-clock choices first. Fine-tuning changes stay
in memory until Save changes. Import XML scans `/switch/gta5save/`. High graphics require
extreme overclocking; actual performance depends on the device.

**Saves & backups:** Story saves, historical backups and the target slot are visible categories.
The confirmation shows the target path. Confirming loads and writes the selected save after
backing up the actual on-disk target. Backups within the same second use separate folders;
backup failures cancel the write. Switching slots does not immediately overwrite a save.
Writing is disabled without a readable target; create an in-game save first.

**Advanced:** Manual gameconfig parameters, original defaults, diagnostics and read-back verification.
Writes and restoration require confirmation. Reports scroll with Up/Down.
Exiting with + does not automatically write unsaved changes.

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

## v7.1 radio and mod update

MEGATARD v1.0.5 supports Chinese through its menu language setting. Classic ragemenu (original English) by **maritoguionyo** is also bundled from the user-supplied file. Installing either classic version replaces the shared menu/loader and restores stock shop_controller, removing the corresponding MEGATARD replacement.

The new Radio workspace installs verified Self Radio packages or paired Radio Editor outputs, with backups, restore and interruption recovery. **The Self Radio runtime is not bundled**: the author's v1.1 download folder was empty when checked. Obtain its matching HARD runtime and release-verification.json separately; use tools/export_package.py to prepare it. See docs/RADIO.zh.md for paths and commands. This build requires console testing.

## v7.2 navigation and DLC loading

Swipe lists and detailed reports. Taps activate once on release; dragging or adding a second finger cancels the tap. Press Left or B to focus the sidebar, Up/Down to choose a page, and Right or A to open it. B from the sidebar returns to Overview. Both sticks and the D-pad work; L/R shortcuts remain available. Page changes use a short 160 ms fade and offset while input remains active.

DLC folders load in bounded batches, with cached reentry and visible-item resource probes. Rescan after changing files externally. Inspect lists vehicle model/spawn names and checks each base/_hi pair for installed or import-source packs. It reads directory data and necessary nested containers instead of loading whole DLC archives. Partial inspection is explicitly reported; names and pairing do not prove in-game compatibility.

## v7.2.1 stick and sidebar focus

Stick activation dead zone increases from 4000 to 12000, with release hysteresis at 8000 and dominant-axis filtering. Holding waits 450 ms before repeating at 180 ms intervals; both sticks follow the same rule. B still enters the sidebar and A still opens a page, retaining sidebar focus. Press Right explicitly to enter the content area. Pressing A on the active page does not reload it.
