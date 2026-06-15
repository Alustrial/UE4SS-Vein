#pragma once
// ─────────────────────────────────────────────────────────────
// VeinCF Overlay Lua API
//
// Exposes panel registration, overlay toggling, draw callbacks,
// and mod reload to Lua scripts.
//
// Lua API:
//   RegisterPanel(name, draw_callback)
//   UnregisterPanel(name)
//   SetPanelVisible(name, bool)
//   SetOverlayVisible(bool)
//   IsOverlayVisible() → bool
//   ToggleOverlay()
//   RegisterDrawCallback(id, callback)
//   UnregisterDrawCallback(id)
//   ReinstallAllMods()
// ─────────────────────────────────────────────────────────────

struct lua_State;

namespace RC::LuaType
{
    // Register overlay control functions as Lua globals.
    auto register_overlay_bindings(lua_State* L) -> void;
}
