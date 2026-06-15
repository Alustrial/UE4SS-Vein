#pragma once
// ─────────────────────────────────────────────────────────────
// VeinCF ImGui Lua Bindings
//
// Exposes the ImGui API to Lua as a global "ImGui" table plus
// enum tables (ImGuiCol, ImGuiCond, ImGuiStyleVar, etc.).
//
// These calls are ONLY valid inside a RegisterPanel callback
// or a RegisterDrawCallback, where the ImGui context is active.
// ─────────────────────────────────────────────────────────────

struct lua_State;

namespace RC::LuaType
{
    // Register the ImGui global table and enum tables on a Lua state.
    auto register_imgui_bindings(lua_State* L) -> void;

    // Register enum tables only (ImGuiCol, ImGuiCond, etc.)
    auto register_imgui_enums(lua_State* L) -> void;
}
