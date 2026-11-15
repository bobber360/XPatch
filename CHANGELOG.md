# Changelog

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
