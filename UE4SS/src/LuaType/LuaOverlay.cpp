// ─────────────────────────────────────────────────────────────
// VeinCF Overlay Lua API
//
// Bridges Lua scripts to the DX12 in-game overlay system.
// Lua mods call RegisterPanel() with a name and draw function;
// the draw function is dispatched every frame inside the
// ImGui context that the DX12 Present hook provides.
//
// Thread safety: Lua draw callbacks are dispatched on the
// render thread (inside Present).  The Lua state is shared
// with the game thread, so we must be careful.  The approach:
// each RegisterPanel stores a Lua registry ref to the callback.
// The DX12 overlay calls a C++ dispatch function that locks
// the Lua state mutex, pushes the ref, and pcalls it.
// ─────────────────────────────────────────────────────────────

#include <LuaType/LuaOverlay.hpp>
#include <GUI/DX12.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

using namespace RC;

namespace RC::LuaType
{
    // ── Internal state ───────────────────────────────────────
    // We need to track Lua callback references so we can dispatch
    // them from the render thread.
    struct LuaPanelRef
    {
        lua_State* L;
        int callback_ref;       // LUA_REGISTRYINDEX ref
        std::string name;
    };

    static std::mutex s_lua_panels_mutex;
    static std::vector<LuaPanelRef> s_lua_panels;

    struct LuaDrawCbRef
    {
        lua_State* L;
        int callback_ref;
        std::string id;
    };

    static std::mutex s_lua_draw_cb_mutex;
    static std::vector<LuaDrawCbRef> s_lua_draw_cbs;

    // ── Dispatch: called from DX12 render thread ─────────────
    // This is the C++ callback registered with DX12Overlay that
    // dispatches into the Lua state.
    static void dispatch_lua_panel(lua_State* L, int ref, const std::string& name)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L, -1);
            Output::send<LogLevel::Error>(STR("[VeinCF/Overlay] Panel '{}' Lua error: {}\n"),
                ensure_str(name), ensure_str(err ? err : "unknown"));
            lua_pop(L, 1);
        }
    }

    // ── RegisterPanel(name, callback) ────────────────────────
    static int l_RegisterPanel(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);

        // Store the callback in the Lua registry
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);

        // Track it internally
        {
            std::lock_guard lock(s_lua_panels_mutex);
            // Remove existing panel with same name
            for (auto it = s_lua_panels.begin(); it != s_lua_panels.end(); ++it)
            {
                if (it->name == name)
                {
                    luaL_unref(it->L, LUA_REGISTRYINDEX, it->callback_ref);
                    s_lua_panels.erase(it);
                    break;
                }
            }
            s_lua_panels.push_back({L, ref, name});
        }

        // Register with the DX12 overlay
        // Capture the Lua state and ref for the render-thread callback
        std::string panel_name = name;
        lua_State* captured_L = L;
        int captured_ref = ref;

        GUI::DX12Overlay::register_panel(panel_name, [captured_L, captured_ref, panel_name]() {
            dispatch_lua_panel(captured_L, captured_ref, panel_name);
        });

        Output::send<LogLevel::Verbose>(STR("[VeinCF/Overlay] Lua panel registered: {}\n"), ensure_str(name));
        return 0;
    }

    // ── UnregisterPanel(name) ────────────────────────────────
    static int l_UnregisterPanel(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);

        {
            std::lock_guard lock(s_lua_panels_mutex);
            for (auto it = s_lua_panels.begin(); it != s_lua_panels.end(); ++it)
            {
                if (it->name == name)
                {
                    luaL_unref(it->L, LUA_REGISTRYINDEX, it->callback_ref);
                    s_lua_panels.erase(it);
                    break;
                }
            }
        }

        GUI::DX12Overlay::unregister_panel(name);
        return 0;
    }

    // ── SetPanelVisible(name, visible) ───────────────────────
    static int l_SetPanelVisible(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        bool visible = lua_toboolean(L, 2);
        GUI::DX12Overlay::set_panel_visible(name, visible);
        return 0;
    }

    // ── Overlay visibility ───────────────────────────────────
    static int l_SetOverlayVisible(lua_State* L)
    {
        bool visible = lua_toboolean(L, 1);
        GUI::DX12Overlay::set_overlay_visible(visible);
        return 0;
    }

    static int l_IsOverlayVisible(lua_State* L)
    {
        lua_pushboolean(L, GUI::DX12Overlay::is_overlay_visible());
        return 1;
    }

    static int l_ToggleOverlay(lua_State* L)
    {
        (void)L;
        GUI::DX12Overlay::toggle_overlay();
        return 0;
    }

    // ── RegisterDrawCallback(id, callback) ───────────────────
    static int l_RegisterDrawCallback(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);

        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);

        {
            std::lock_guard lock(s_lua_draw_cb_mutex);
            for (auto it = s_lua_draw_cbs.begin(); it != s_lua_draw_cbs.end(); ++it)
            {
                if (it->id == id)
                {
                    luaL_unref(it->L, LUA_REGISTRYINDEX, it->callback_ref);
                    s_lua_draw_cbs.erase(it);
                    break;
                }
            }
            s_lua_draw_cbs.push_back({L, ref, id});
        }

        std::string cb_id = id;
        lua_State* captured_L = L;
        int captured_ref = ref;

        GUI::DX12Overlay::register_draw_callback(cb_id, [captured_L, captured_ref, cb_id]() {
            lua_rawgeti(captured_L, LUA_REGISTRYINDEX, captured_ref);
            if (lua_pcall(captured_L, 0, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(captured_L, -1);
                Output::send<LogLevel::Error>(STR("[VeinCF/Overlay] Draw callback '{}' error: {}\n"),
                    ensure_str(cb_id), ensure_str(err ? err : "unknown"));
                lua_pop(captured_L, 1);
            }
        });

        return 0;
    }

    // ── UnregisterDrawCallback(id) ───────────────────────────
    static int l_UnregisterDrawCallback(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);

        {
            std::lock_guard lock(s_lua_draw_cb_mutex);
            for (auto it = s_lua_draw_cbs.begin(); it != s_lua_draw_cbs.end(); ++it)
            {
                if (it->id == id)
                {
                    luaL_unref(it->L, LUA_REGISTRYINDEX, it->callback_ref);
                    s_lua_draw_cbs.erase(it);
                    break;
                }
            }
        }

        GUI::DX12Overlay::unregister_draw_callback(id);
        return 0;
    }

    // ── ReinstallAllMods() ───────────────────────────────────
    // Exposes the UE4SS hot-reload to Lua so mods can build
    // an in-game reload button.
    static int l_ReinstallAllMods(lua_State* L);

    // We need to forward-declare this because UE4SSProgram.hpp
    // has a complex include chain.  The actual implementation
    // calls UE4SSProgram::get_program().queue_reinstall_mods().
    // We'll define it below after including the program header.

    // ── GetPanelCount() ──────────────────────────────────────
    static int l_GetPanelCount(lua_State* L)
    {
        lua_pushinteger(L, static_cast<lua_Integer>(GUI::DX12Overlay::get_panel_count()));
        return 1;
    }

    // ── Cleanup: called on mod uninstall / hot-reload ────────
    // Releases all Lua registry refs so the old Lua state can
    // be destroyed cleanly.
    static void cleanup_lua_refs()
    {
        {
            std::lock_guard lock(s_lua_panels_mutex);
            for (auto& panel : s_lua_panels)
            {
                luaL_unref(panel.L, LUA_REGISTRYINDEX, panel.callback_ref);
            }
            s_lua_panels.clear();
        }
        {
            std::lock_guard lock(s_lua_draw_cb_mutex);
            for (auto& cb : s_lua_draw_cbs)
            {
                luaL_unref(cb.L, LUA_REGISTRYINDEX, cb.callback_ref);
            }
            s_lua_draw_cbs.clear();
        }
    }

    // ═════════════════════════════════════════════════════════
    // Registration
    // ═════════════════════════════════════════════════════════

    auto register_overlay_bindings(lua_State* L) -> void
    {
        lua_register(L, "RegisterPanel", l_RegisterPanel);
        lua_register(L, "UnregisterPanel", l_UnregisterPanel);
        lua_register(L, "SetPanelVisible", l_SetPanelVisible);
        lua_register(L, "SetOverlayVisible", l_SetOverlayVisible);
        lua_register(L, "IsOverlayVisible", l_IsOverlayVisible);
        lua_register(L, "ToggleOverlay", l_ToggleOverlay);
        lua_register(L, "RegisterDrawCallback", l_RegisterDrawCallback);
        lua_register(L, "UnregisterDrawCallback", l_UnregisterDrawCallback);
        lua_register(L, "GetPanelCount", l_GetPanelCount);
        // ReinstallAllMods is registered separately via the program integration
    }

} // namespace RC::LuaType
