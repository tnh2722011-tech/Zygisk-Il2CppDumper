# Zygisk-Il2CppDumper (Enhanced - 100% Full Dump)

Enhanced version of Il2CppDumper with Zygisk. Dumps il2cpp data at runtime with **100% game logic parity** compared to the PC Il2CppDumper. Bypasses protection, encryption and obfuscation.

## What's New in v2.0.0

- ✅ **100% complete dump** - Dumps ALL assemblies, classes, fields, properties, events, methods, and nested types (no missing logic)
- ✅ **script.json** - Also generates `script.json` like PC Il2CppDumper (for use with dnSpy/Il2CppInspector)
- ✅ **Saves to Download** - Output saved directly to `/sdcard/Download/Il2CppDumper/` (easy to find)
- ✅ **Decrypted global-metadata.dat** - Scans in-memory for the decrypted metadata and dumps it too
- ✅ **`il2cpp_class_for_each` fallback** - Catches all runtime-registered generic/anonymous classes
- ✅ **Dynamic target package** - Read target from `/sdcard/dump_target.txt` (no rebuild needed)
- ✅ **Full storage access** - Forces full external storage mount for the target app
- ✅ **Longer wait timeout** - Waits up to 60s (120 x 500ms) for `libil2cpp.so` to load

## Output Files

After starting the game, the following files are generated in **`/sdcard/Download/Il2CppDumper/`**:

| File | Description |
|------|-------------|
| `dump.cs` | Full C# class dump (all assemblies, 100% logic) |
| `script.json` | Method address map (compatible with PC Il2CppDumper tools) |
| `global-metadata.dat` | Decrypted in-memory metadata (if found) |

## How to use

### Method 1 - GitHub Actions (Recommended)
1. Fork this repo
2. Go to **Actions** tab → **Build** workflow → **Run workflow**
3. Enter game package name (default: `com.vng.playtogether`)
4. Download the artifact zip, install via Magisk
5. Start the game → files appear in `/sdcard/Download/Il2CppDumper/`

### Method 2 - Change target without rebuilding
1. Install the module
2. Create file `/sdcard/dump_target.txt` with the package name:
   ```
   com.vng.playtogether
   ```
3. Restart the game

### Method 3 - Android Studio
1. Edit `module/src/main/cpp/game.h`, set `GamePackageName`
2. Run gradle task `:module:assembleRelease`
3. Install the zip from `out/` via Magisk

## Requirements
- Magisk v24+ with Zygisk enabled
- Root access

---
Original project by [Perfare](https://github.com/Perfare/Zygisk-Il2CppDumper)