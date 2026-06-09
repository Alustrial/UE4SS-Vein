# UE4SS for VEIN

Pre-configured [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) build for **VEIN** (UE 5.6.1), with stability patches and game-specific signatures.

## Credits

- **[RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)** by **Narknon** and the UE4SS-RE team — The original UE4SS framework.
- **[xMathayus](https://github.com/xMathayus)** — VEIN-compatible UE4SS fork (`RE-UE4SS-vein-3.0.1-940`) that this build is based on.
- **Alustrial** — VEIN-specific signatures, mappings, and DLL stability patches.

## What's Included

| File | Purpose |
|------|---------|
| `UE4SS.dll` | Patched UE4SS DLL with crash fixes |
| `Vein-5.6.1-0+UE5-01e0a584.usmap` | Unreal mappings for VEIN |
| `UE4SS-settings.ini` | Game-specific configuration |
| `MemberVariableLayout.ini` | Memory layout offsets for VEIN |
| `VTableLayout.ini` | Virtual function table layout |
| `UE4SS_Signatures/` | AOB signatures (StaticConstructObject, FText) |
| `Mods/` | Default UE4SS mods (console, keybinds, BP mod loader) |

## DLL Patches

This build includes stability patches that prevent common crashes when scripting against VEIN:

- **Null pointer safety** — 16+ null checks added across the Lua ↔ UObject bridge, converting hard crashes into catchable Lua errors
- **TextProperty string support** — Lua strings can now be assigned directly to FText properties (e.g., `object.DisplayName = "My Item"`)
- **TArray safety** — Null checks on array operations (ForEach, GetArrayNum, index access) prevent crashes on invalid arrays
- **SEH note** — VEIN's UE4SS build has SEH (Structured Exception Handling) disabled. Without these patches, any null pointer dereference in the Lua bridge is a guaranteed hard crash.

## Installation

1. Navigate to your VEIN install directory:
   ```
   <Steam>/steamapps/common/Vein/Vein/Binaries/Win64/
   ```

2. If a `ue4ss` folder already exists, back it up.

3. Copy the entire contents of this release into `Win64/ue4ss/`:
   ```
   Win64/
     ue4ss/
       UE4SS.dll
       UE4SS-settings.ini
       MemberVariableLayout.ini
       VTableLayout.ini
       Vein-5.6.1-0+UE5-01e0a584.usmap
       UE4SS_Signatures/
         StaticConstructObject.lua
         FText_Constructor.lua
       Mods/
         mods.txt
         mods.json
         ...
   ```

4. You also need the UE4SS proxy DLL. Place `dwmapi.dll` in the `Win64/` directory (same folder as `Vein-Win64-Test.exe`). If you don't have it, grab it from the [official UE4SS releases](https://github.com/UE4SS-RE/RE-UE4SS/releases).

5. Launch the game. The UE4SS console window should appear.

## Configuration

The included `UE4SS-settings.ini` is pre-configured for VEIN:

- Engine version: 5.6
- GUI console: enabled
- All hooks: enabled
- Hot reload: disabled by default (set `EnableHotReloadSystem = 1` to enable Ctrl+R reload)

## Writing Lua Mods

Create a new folder in `Mods/` with a `Scripts/main.lua`:

```
Mods/
  MyMod/
    Scripts/
      main.lua
    enabled.txt       <-- empty file, enables the mod
```

Enable it by adding to `mods.txt`:
```
MyMod : 1
```

### Example: Modify Recipe Craft Time

```lua
ExecuteInGameThread(function()
    local mgr = FindFirstOf("VeinAssetManager")
    local recipes = mgr:GetAllRecipes(false)

    for i = 1, #recipes do
        local recipe = recipes[i]:get()
        if recipe:IsValid() and recipe:GetFName():ToString() == "CR_Kindling" then
            recipe.CraftTime = 1.0  -- was 6.0
            print("Kindling craft time set to 1 second!")
            break
        end
    end
end)
```

## Game Version

Tested with VEIN build as of June 2026 (UE 5.6.1). Signatures and mappings may need updating after game patches.

## License

UE4SS is licensed under MIT. See [LICENSE](LICENSE) for details.
