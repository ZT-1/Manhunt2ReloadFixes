# Manhunt 2 Reload Fixes

An ASI plugin for Manhunt 2 (PC) that fixes four vanilla reload bugs. Each fix can be turned on
or off in `ReloadFixes.ini`.

| Option | Bug |
|---|---|
| `RevolverFlick` | After a partial `Shotgun_(CT)` or `Shotgun_Torch_(CT)` reload, the 6-shooter's reload skips its cylinder-close. The cylinder stays open until you fire again. |
| `ShotgunQueuedReload` | Pressing reload while a shotgun shot or pump is still playing can end a partial reload with an extra pump that ejects a phantom shell. |
| `SniperLastCasing` | The sniper rifle never ejects the case from its last shot. |
| `ShotgunLastShell` | The pump shotgun's last spent shell comes out late in the reload, just before the closing pump, instead of on the pump after the last shot. |

This is the Manhunt 2 version of the Manhunt 1 plugin,
[ManhuntReloadFixes](https://github.com/ZT-1/ManhuntReloadFixes), which has the full
analysis of each bug. The same engine code is in both games, at different addresses.

## Install

This repo is source only, with no prebuilt binary. Build it first (see Build below).

1. You need an ASI loader in the game folder. If other `.asi` plugins already work for you, you
   have one.
2. Copy the built `bin\ReloadFixes.asi` and `bin\ReloadFixes.ini` into the game's `plugins\`
   folder.
3. Start the game. To uninstall, delete the `.asi`. Logging is off by default. To check that the
   fixes applied, set `Log=1` in the `.ini`: `reloadfixes_asi.log` then appears next to
   `Manhunt2.exe` with one `patched` line per enabled patch. Problems are always logged: `ABORT:`
   means the exe wasn't the expected one and that fix wasn't applied.

If you used the older `RevolverFlickFix.asi`, delete it. This plugin replaces it.

It patches the game in memory only. `Manhunt2.exe` is never modified. It expects the PC exe with
image base 0x400000 (no ASLR). Developed and tested with the Nostalgia Modpack installed, whose
weapon config and Manhunt 1 player animations (`Mh1PlayerAnims`) match the timings used here.

## Settings (`ReloadFixes.ini`)

```ini
[Fixes]
RevolverFlick=1
ShotgunQueuedReload=1
SniperLastCasing=1
ShotgunLastShell=1

[General]
Log=0
```

`1` turns an option on and `0` turns it off. Changes take effect on the next game start.
`Log=0` (the default) only writes the log when something goes wrong; `Log=1` writes it on every
start. If the `.ini` is missing, all four fixes are on and logging is off.

## Addresses

| | MH1 `manhunt.exe` | MH2 `Manhunt2.exe` |
|---|---|---|
| ped flag ("partial looped reload: cut at loop end") | `CPed+0x33C` bit 0x40 | `CPed+0x308` bit 0x40 |
| torso FSM | `CPed+0x2FC` | `CPed+0x2C0` |
| `SetReload` | 0x425410 | 0x492B30 |
| shotgun-only set/clear of the flag | 0x4B1114 | 0x508001 |
| non-shotgun reload start (`RevolverFlick`) | 0x4B1130 | 0x508025 |
| empty-clip reload start (`RevolverFlick`) | 0x45F479 | 0x5AB6FA |
| flag consumer (any RELOAD_LOOP weapon) | 0x4B89F9 | 0x510A8D |
| queued reload start (`ShotgunQueuedReload`) | 0x4BA11E | 0x50E18D |
| auto reload after the last round (`SniperLastCasing`, `ShotgunLastShell`) | 0x4B8857 | 0x51093B |
| bolt/pump code when rounds are loaded | 0x4B8880 | 0x510958 |
| reload-eject re-arm (`ShotgunLastShell`) | 0x4B8BD7 | 0x510BE0 |
| "shell already ejected" flag | collectable +0x13C bit 0x10 | collectable +0x1BC bit 0x10 |
| collectable type | type data +0x164 | type data +0x1F4 |
| `CT_SHOTGUN` / `CT_SHOTGUN_TORCH` | 0x2C / 0x2D | 0x2E / 0x2F |
| `CT_SNIPER_RIFLE` (and silenced) | 0x30 | 0x32 |

## How each fix works

- **`RevolverFlick`**: at both non-shotgun reload starts, the 11 bytes
  `mov ecx,[esi+2C0h]; call 0x492B30` jump to a stub that clears the flag
  (`and dword [esi+308h], 0FFFFFFBFh`), runs the original two instructions, and jumps back.
- **`ShotgunQueuedReload`**: the queued reload's `call 0x492B30` at 0x50E18D calls a stub instead.
  The stub runs `SetReload`, then sets or clears the flag from the rounds loaded in the held
  weapon, as the normal shotgun start does.
- **`SniperLastCasing` / `ShotgunLastShell`**: after the last shot, the bolt/pump state requests
  the reload on every frame (0x51093B) before reaching its eject point. For the sniper and the
  pump shotguns, the plugin holds that request until the bolt (0.55 s) or pump (0.35 s) has
  closed and runs the normal bolt/pump code (0x510958) instead, which ejects the case on time.
  Then it requests the reload as vanilla does, still running that code, so the weapon model
  stays synced to Cash's hands while the reload blends in.
- **`ShotgunLastShell`** also keeps the "shell already ejected" flag during the reload instead of
  re-arming it (0x510BE0), so the reload's own eject (`SHELL_EJECT_DELAY_RELOAD 0.7`) only
  happens if the shell is still in the gun.

Trade-off: after an empty magazine or tube, the reload starts a little later than in vanilla
(up to about 0.55 s for the sniper and 0.35 s for the shotgun), because the bolt or pump
finishes first.

Before patching, the plugin checks the image base and the original bytes at each site, and skips
a fix if they differ.

## Build

`build.cmd` builds `bin\ReloadFixes.asi` with Visual Studio's `cl` (Win32) and copies
`ReloadFixes.ini` next to it. Edit the `VCVARS` line in the script if your Visual Studio is in a
different place.

## Confirmed in game

- `RevolverFlick`: the revolver reload plays its cylinder-close on every reload following a
  `Shotgun_(CT)` reload.
- `ShotgunQueuedReload`, `SniperLastCasing`, `ShotgunLastShell`: all behave as intended in game
  (the same checks as Manhunt 1), with all five patches applied.
