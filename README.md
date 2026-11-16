# Manhunt 2 Revolver Reload Fix

An ASI plugin for Manhunt 2 (PC) that fixes a vanilla engine bug: after you reload the pump
shotgun (`Shotgun_(CT)` or `Shotgun_Torch_(CT)`) with shells still in it, the 6-shooter's next
reload skips its cylinder-close. The cylinder stays swung open until you fire again.

This is the Manhunt 2 version of
[ManhuntRevolverReloadFix](https://github.com/ZT-1/ManhuntRevolverReloadFix) (Manhunt 1), which
has the full analysis.

## Install

This repo is source only, with no prebuilt binary. Build it first (see Build below).

1. You need an ASI loader in the game folder. If other `.asi` plugins already work for you, you
   have one.
2. Copy the built `bin\RevolverFlickFix.asi` into the game's `plugins\` folder (next to
   `Manhunt2.exe`).
3. Start the game. `revolverflickfix_asi.log` appears next to `Manhunt2.exe`. A good run contains
   two `patched 0x...` lines. `ABORT:` means the exe wasn't the expected one and nothing was
   changed. To uninstall, delete the `.asi`.

It patches the game in memory only. `Manhunt2.exe` is never modified. It expects the PC exe with
image base 0x400000 (no ASLR).

## Cause

The same logic as Manhunt 1, at different addresses:

| | MH1 `manhunt.exe` | MH2 `Manhunt2.exe` |
|---|---|---|
| ped flag ("partial looped reload: cut at loop end") | `CPed+0x33C` bit 0x40 | `CPed+0x308` bit 0x40 |
| torso FSM | `CPed+0x2FC` | `CPed+0x2C0` |
| `SetReload` | 0x425410 | 0x492B30 |
| shotgun-only set/clear | 0x4B1114 | 0x508001 |
| non-shotgun reload start (doesn't touch the bit) | 0x4B1130 | 0x508025 |
| empty-clip reload start (doesn't touch the bit) | 0x45F479 | 0x5AB6FA |
| consumer (any RELOAD_LOOP weapon, cuts the anim) | 0x4B89F9 | 0x510A8D |
| `CT_SHOTGUN` / `CT_SHOTGUN_TORCH` | 0x2C / 0x2D | 0x2E / 0x2F |

`RELOAD_LOOP` in `WeaponTypeData.ini` parses to weapon-data +0xA8 (flag), +0xAC (start) and +0xB0
(end) in both games (MH2 parser at 0x4F10AC). The flag is set only when the held collectable is
a pump shotgun, and it is checked for every RELOAD_LOOP weapon, so the revolver's reload gets cut
before its cylinder closes. The re-reload after the pump (0x510941) already clears the flag
itself.

## The fix

At both non-shotgun reload starts (0x508025 and 0x5AB6FA), the 11 bytes
`mov ecx,[esi+2C0h]; call 0x492B30` are replaced with a jump to a stub. The stub runs
`and dword [esi+308h], 0FFFFFFBFh`, then the original two instructions, then jumps back. The
shotgun's own path is untouched, so partial shotgun reloads still skip the pump as in vanilla.
Before patching, the plugin checks the image base and the original bytes at both sites, and does
nothing if they differ.

## Build

`build.cmd` builds `bin\RevolverFlickFix.asi` with Visual Studio's `cl` (Win32). Edit the `VCVARS`
line in the script if your Visual Studio is in a different place.

## Confirmed in game

After the fix, the revolver reload plays its cylinder-close on every reload following a
`Shotgun_(CT)` reload.
