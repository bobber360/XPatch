# Changelog

## v1.2

- **Console patches.** New patches made for and tested on a real Xbox 360, and console-safe versions of Xenia
  patches that crash or do nothing on real hardware: GTA IV TU8 (render resolution 1120x630 / 960x540, locked
  30 FPS frame pacer, traffic density, cheap audio, depth of field, warp shadow), Red Dead Redemption TU9
  (Disable V-Sync / 60 FPS V-Sync), Gears of War TU5 (1120x630, 2x MSAA off), Gears of War 3 TU6 (Performance
  Patch Option A that doesn't crash), Batman: Arkham City TU6 (60 FPS V-Sync / Disable V-Sync with a 60 FPS cap).
  See [Console patches](README.md#console-patches).
- **Converter with console fixes.** `Convert-XeniaPatches.ps1` takes out the Xenia patches that don't work on a
  console, adds the console patches (`tools/converter-fixes/`) and leaves out games that gain nothing.
- **Early patching** (`early_patch`, off by default): patches go in before the game's first instruction, through a
  hook on the kernel's `XexStartExecutable`. Needed by patches marked *(early patching required)*.
- **Settings** (`XPatch360.ini` and *[ Settings ]* in Aurora): notification, log level, log size limit, early
  patching, debug mode. Changes made in Aurora apply to the next game without a restart.
- **Own folder**: the log, settings, cache and dumps are now in `Hdd:\Patches\XPatch360\`; the v1.1 files are moved
  there at the first start.
- **Conflict check**: two enabled patches writing the same address are reported in the log, in the notification
  and marked *(!) conflict* in Aurora.
- **Errors in patch files** are logged with the file name and line number.
- **Aurora**: *[ Turn all patches off ]* at the top of every patch list; *(console)* patches listed first.
- **Log size limit**: a log bigger than `log_max_kb` is started again at boot.
- **Debug mode and debug_tools** for patch makers: memory dumps from the console (`XPatch360.dump`, Aurora entries),
  `ppcdis.py` (small PowerPC disassembler) and a guide. See [debug_tools](debug_tools/README.md).
- Notifications are queued (3 s apart) and never hold up the patching; the plugin uses less memory.

## v1.1

- **Renamed to XPatch360.** On the console: `XPatch360.xex`, `Hdd:\Patches\XPatch360.log`, Aurora folder
  `XPatch360`. See [Updating from v1.0](README.md#updating-from-v10-xpatch).
- **Version check by hash.** The plugin computes Xenia's module hash on the console and applies a patch file only if
  it lists that hash, so files for another TU, region or disc are skipped. The v1.0 TU / `media_id` check is
  kept as a fallback.
- **Hash cache** (`Hdd:\Patches\XPatch360.cache`): known game versions are patched right away and checked afterwards,
  so early patches like *Skip Legal Screens* still work. On a mismatch the original bytes are restored.
- **New notification** for skipped files: `XPatch360: 2 patches skipped - game version doesn't match`.
- **Aurora: patch info window.** Selecting a patch shows its author and description, with Turn ON / Turn OFF and
  Back.
- **Aurora:** descriptions that only point to the Xenia README get a note on what the patch means on a console.

## v1.0

- First release: Dashlaunch plugin that applies Xenia Canary patches in memory after the title update loads.
- TU check by file name, `media_id` check.
- Aurora script: installed games, patch files by TU, patches on/off.
- Notification with the number of patches applied, log in `Hdd:\Patches\XPatch.log`.
- No dependency on other plugins.
