// ─────────────────────────────────────────────────────────────
// VeinCF ImGui Lua Bindings
//
// Exposes ~60 ImGui functions to Lua as the global "ImGui"
// table, plus enum tables for flags/colors/style vars.
//
// Convention:
//   ImGui.Begin(name [, open] [, flags]) → visible, open
//   ImGui.Button(label [, w] [, h])      → clicked
//   ImGui.SliderFloat(label, v, min, max) → changed, v
//
// All functions are thin wrappers that read Lua args from the
// stack, call the real ImGui C++ function, and push results.
// ─────────────────────────────────────────────────────────────

#include <LuaType/LuaImGui.hpp>

#include <imgui.h>
#include <string>
#include <vector>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace RC::LuaType
{
    // ── Helpers ──────────────────────────────────────────────
    static auto opt_float(lua_State* L, int idx, float def) -> float
    {
        return lua_isnoneornil(L, idx) ? def : static_cast<float>(luaL_checknumber(L, idx));
    }
    static auto opt_int(lua_State* L, int idx, int def) -> int
    {
        return lua_isnoneornil(L, idx) ? def : static_cast<int>(luaL_checkinteger(L, idx));
    }

    // ── Window management ────────────────────────────────────

    // ImGui.Begin(name [, open] [, flags]) → visible [, open]
    static int l_Begin(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        bool has_open = lua_isboolean(L, 2);
        bool p_open = has_open ? lua_toboolean(L, 2) : true;
        int flags = opt_int(L, 3, 0);

        bool visible = ImGui::Begin(name, has_open ? &p_open : nullptr, flags);
        lua_pushboolean(L, visible);
        if (has_open)
        {
            lua_pushboolean(L, p_open);
            return 2;
        }
        return 1;
    }

    static int l_End(lua_State* L)
    {
        (void)L;
        ImGui::End();
        return 0;
    }

    static int l_SetNextWindowSize(lua_State* L)
    {
        float w = static_cast<float>(luaL_checknumber(L, 1));
        float h = static_cast<float>(luaL_checknumber(L, 2));
        int cond = opt_int(L, 3, 0);
        ImGui::SetNextWindowSize(ImVec2(w, h), cond);
        return 0;
    }

    static int l_SetNextWindowPos(lua_State* L)
    {
        float x = static_cast<float>(luaL_checknumber(L, 1));
        float y = static_cast<float>(luaL_checknumber(L, 2));
        int cond = opt_int(L, 3, 0);
        ImGui::SetNextWindowPos(ImVec2(x, y), cond);
        return 0;
    }

    static int l_SetNextWindowBgAlpha(lua_State* L)
    {
        float alpha = static_cast<float>(luaL_checknumber(L, 1));
        ImGui::SetNextWindowBgAlpha(alpha);
        return 0;
    }

    static int l_SetNextItemWidth(lua_State* L)
    {
        float w = static_cast<float>(luaL_checknumber(L, 1));
        ImGui::SetNextItemWidth(w);
        return 0;
    }

    static int l_GetWindowSize(lua_State* L)
    {
        ImVec2 size = ImGui::GetWindowSize();
        lua_pushnumber(L, size.x);
        lua_pushnumber(L, size.y);
        return 2;
    }

    static int l_GetWindowPos(lua_State* L)
    {
        ImVec2 pos = ImGui::GetWindowPos();
        lua_pushnumber(L, pos.x);
        lua_pushnumber(L, pos.y);
        return 2;
    }

    // ── Layout ───────────────────────────────────────────────

    static int l_SameLine(lua_State* L)
    {
        float offset = opt_float(L, 1, 0.0f);
        float spacing = opt_float(L, 2, -1.0f);
        ImGui::SameLine(offset, spacing);
        return 0;
    }

    static int l_NewLine(lua_State* L) { (void)L; ImGui::NewLine(); return 0; }
    static int l_Separator(lua_State* L) { (void)L; ImGui::Separator(); return 0; }
    static int l_Spacing(lua_State* L) { (void)L; ImGui::Spacing(); return 0; }

    static int l_Indent(lua_State* L)
    {
        float w = opt_float(L, 1, 0.0f);
        ImGui::Indent(w);
        return 0;
    }

    static int l_Unindent(lua_State* L)
    {
        float w = opt_float(L, 1, 0.0f);
        ImGui::Unindent(w);
        return 0;
    }

    static int l_Dummy(lua_State* L)
    {
        float w = static_cast<float>(luaL_checknumber(L, 1));
        float h = static_cast<float>(luaL_checknumber(L, 2));
        ImGui::Dummy(ImVec2(w, h));
        return 0;
    }

    // ── Text ─────────────────────────────────────────────────

    static int l_Text(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ImGui::TextUnformatted(text);
        return 0;
    }

    // ImGui.TextColored(r, g, b, a, text)
    static int l_TextColored(lua_State* L)
    {
        float r = static_cast<float>(luaL_checknumber(L, 1));
        float g = static_cast<float>(luaL_checknumber(L, 2));
        float b = static_cast<float>(luaL_checknumber(L, 3));
        float a = static_cast<float>(luaL_checknumber(L, 4));
        const char* text = luaL_checkstring(L, 5);
        ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
        return 0;
    }

    static int l_TextWrapped(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ImGui::TextWrapped("%s", text);
        return 0;
    }

    static int l_TextDisabled(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ImGui::TextDisabled("%s", text);
        return 0;
    }

    static int l_BulletText(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ImGui::BulletText("%s", text);
        return 0;
    }

    // ── Widgets ──────────────────────────────────────────────

    // ImGui.Button(label [, w] [, h]) → clicked
    static int l_Button(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float w = opt_float(L, 2, 0.0f);
        float h = opt_float(L, 3, 0.0f);
        lua_pushboolean(L, ImGui::Button(label, ImVec2(w, h)));
        return 1;
    }

    static int l_SmallButton(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        lua_pushboolean(L, ImGui::SmallButton(label));
        return 1;
    }

    static int l_InvisibleButton(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        float w = static_cast<float>(luaL_checknumber(L, 2));
        float h = static_cast<float>(luaL_checknumber(L, 3));
        lua_pushboolean(L, ImGui::InvisibleButton(id, ImVec2(w, h)));
        return 1;
    }

    // ImGui.Checkbox(label, checked) → changed, checked
    static int l_Checkbox(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        bool v = lua_toboolean(L, 2);
        bool changed = ImGui::Checkbox(label, &v);
        lua_pushboolean(L, changed);
        lua_pushboolean(L, v);
        return 2;
    }

    // ImGui.RadioButton(label, active) → clicked
    static int l_RadioButton(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        bool active = lua_toboolean(L, 2);
        lua_pushboolean(L, ImGui::RadioButton(label, active));
        return 1;
    }

    // ImGui.ProgressBar(fraction [, w] [, h] [, overlay])
    static int l_ProgressBar(lua_State* L)
    {
        float fraction = static_cast<float>(luaL_checknumber(L, 1));
        float w = opt_float(L, 2, -FLT_MIN);
        float h = opt_float(L, 3, 0.0f);
        const char* overlay = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr;
        ImGui::ProgressBar(fraction, ImVec2(w, h), overlay);
        return 0;
    }

    // ImGui.SliderFloat(label, value, min, max [, format]) → changed, value
    static int l_SliderFloat(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float v = static_cast<float>(luaL_checknumber(L, 2));
        float v_min = static_cast<float>(luaL_checknumber(L, 3));
        float v_max = static_cast<float>(luaL_checknumber(L, 4));
        const char* fmt = lua_isstring(L, 5) ? lua_tostring(L, 5) : "%.3f";
        bool changed = ImGui::SliderFloat(label, &v, v_min, v_max, fmt);
        lua_pushboolean(L, changed);
        lua_pushnumber(L, v);
        return 2;
    }

    // ImGui.SliderInt(label, value, min, max [, format]) → changed, value
    static int l_SliderInt(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        int v = static_cast<int>(luaL_checkinteger(L, 2));
        int v_min = static_cast<int>(luaL_checkinteger(L, 3));
        int v_max = static_cast<int>(luaL_checkinteger(L, 4));
        const char* fmt = lua_isstring(L, 5) ? lua_tostring(L, 5) : "%d";
        bool changed = ImGui::SliderInt(label, &v, v_min, v_max, fmt);
        lua_pushboolean(L, changed);
        lua_pushinteger(L, v);
        return 2;
    }

    // ImGui.InputText(label, text [, flags]) → changed, text
    static int l_InputText(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        const char* initial = luaL_checkstring(L, 2);
        int flags = opt_int(L, 3, 0);

        // ImGui::InputText needs a mutable buffer
        char buf[1024];
        strncpy_s(buf, sizeof(buf), initial, _TRUNCATE);

        bool changed = ImGui::InputText(label, buf, sizeof(buf), flags);
        lua_pushboolean(L, changed);
        lua_pushstring(L, buf);
        return 2;
    }

    // ImGui.InputTextMultiline(label, text [, w] [, h] [, flags]) → changed, text
    static int l_InputTextMultiline(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        const char* initial = luaL_checkstring(L, 2);
        float w = opt_float(L, 3, 0.0f);
        float h = opt_float(L, 4, 0.0f);
        int flags = opt_int(L, 5, 0);

        char buf[4096];
        strncpy_s(buf, sizeof(buf), initial, _TRUNCATE);

        bool changed = ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(w, h), flags);
        lua_pushboolean(L, changed);
        lua_pushstring(L, buf);
        return 2;
    }

    // ImGui.InputFloat(label, value [, step] [, step_fast] [, format]) → changed, value
    static int l_InputFloat(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float v = static_cast<float>(luaL_checknumber(L, 2));
        float step = opt_float(L, 3, 0.0f);
        float step_fast = opt_float(L, 4, 0.0f);
        const char* fmt = lua_isstring(L, 5) ? lua_tostring(L, 5) : "%.3f";
        bool changed = ImGui::InputFloat(label, &v, step, step_fast, fmt);
        lua_pushboolean(L, changed);
        lua_pushnumber(L, v);
        return 2;
    }

    // ImGui.InputInt(label, value [, step] [, step_fast]) → changed, value
    static int l_InputInt(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        int v = static_cast<int>(luaL_checkinteger(L, 2));
        int step = opt_int(L, 3, 1);
        int step_fast = opt_int(L, 4, 100);
        bool changed = ImGui::InputInt(label, &v, step, step_fast);
        lua_pushboolean(L, changed);
        lua_pushinteger(L, v);
        return 2;
    }

    // ImGui.Combo(label, current_idx, items_table) → changed, current_idx
    // items_table is a 1-based Lua array of strings
    static int l_Combo(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        int current = static_cast<int>(luaL_checkinteger(L, 2));
        luaL_checktype(L, 3, LUA_TTABLE);

        // Build a vector of item strings
        int count = static_cast<int>(lua_rawlen(L, 3));
        std::vector<const char*> items(count);
        std::vector<std::string> storage(count);

        for (int i = 0; i < count; i++)
        {
            lua_rawgeti(L, 3, i + 1);
            storage[i] = lua_tostring(L, -1);
            items[i] = storage[i].c_str();
            lua_pop(L, 1);
        }

        bool changed = ImGui::Combo(label, &current, items.data(), count);
        lua_pushboolean(L, changed);
        lua_pushinteger(L, current);
        return 2;
    }

    // ImGui.ColorEdit3(label, r, g, b [, flags]) → changed, r, g, b
    static int l_ColorEdit3(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float col[3] = {
            static_cast<float>(luaL_checknumber(L, 2)),
            static_cast<float>(luaL_checknumber(L, 3)),
            static_cast<float>(luaL_checknumber(L, 4))
        };
        int flags = opt_int(L, 5, 0);
        bool changed = ImGui::ColorEdit3(label, col, flags);
        lua_pushboolean(L, changed);
        lua_pushnumber(L, col[0]);
        lua_pushnumber(L, col[1]);
        lua_pushnumber(L, col[2]);
        return 4;
    }

    // ImGui.ColorEdit4(label, r, g, b, a [, flags]) → changed, r, g, b, a
    static int l_ColorEdit4(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float col[4] = {
            static_cast<float>(luaL_checknumber(L, 2)),
            static_cast<float>(luaL_checknumber(L, 3)),
            static_cast<float>(luaL_checknumber(L, 4)),
            static_cast<float>(luaL_checknumber(L, 5))
        };
        int flags = opt_int(L, 6, 0);
        bool changed = ImGui::ColorEdit4(label, col, flags);
        lua_pushboolean(L, changed);
        lua_pushnumber(L, col[0]);
        lua_pushnumber(L, col[1]);
        lua_pushnumber(L, col[2]);
        lua_pushnumber(L, col[3]);
        return 5;
    }

    // ── Tree / Collapsing ────────────────────────────────────

    static int l_CollapsingHeader(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        int flags = opt_int(L, 2, 0);
        lua_pushboolean(L, ImGui::CollapsingHeader(label, flags));
        return 1;
    }

    static int l_TreeNode(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        lua_pushboolean(L, ImGui::TreeNode(label));
        return 1;
    }

    static int l_TreePop(lua_State* L) { (void)L; ImGui::TreePop(); return 0; }

    // ── Tabs ─────────────────────────────────────────────────

    static int l_BeginTabBar(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        int flags = opt_int(L, 2, 0);
        lua_pushboolean(L, ImGui::BeginTabBar(id, flags));
        return 1;
    }

    static int l_EndTabBar(lua_State* L) { (void)L; ImGui::EndTabBar(); return 0; }

    static int l_BeginTabItem(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        lua_pushboolean(L, ImGui::BeginTabItem(label));
        return 1;
    }

    static int l_EndTabItem(lua_State* L) { (void)L; ImGui::EndTabItem(); return 0; }

    // ── Tables ───────────────────────────────────────────────

    static int l_BeginTable(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        int cols = static_cast<int>(luaL_checkinteger(L, 2));
        int flags = opt_int(L, 3, 0);
        lua_pushboolean(L, ImGui::BeginTable(id, cols, flags));
        return 1;
    }

    static int l_EndTable(lua_State* L) { (void)L; ImGui::EndTable(); return 0; }
    static int l_TableNextRow(lua_State* L) { (void)L; ImGui::TableNextRow(); return 0; }

    static int l_TableNextColumn(lua_State* L)
    {
        lua_pushboolean(L, ImGui::TableNextColumn());
        return 1;
    }

    static int l_TableSetupColumn(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        int flags = opt_int(L, 2, 0);
        float width = opt_float(L, 3, 0.0f);
        ImGui::TableSetupColumn(label, flags, width);
        return 0;
    }

    static int l_TableHeadersRow(lua_State* L) { (void)L; ImGui::TableHeadersRow(); return 0; }

    // ── Style ────────────────────────────────────────────────

    // ImGui.PushStyleColor(idx, r, g, b, a)
    static int l_PushStyleColor(lua_State* L)
    {
        int idx = static_cast<int>(luaL_checkinteger(L, 1));
        float r = static_cast<float>(luaL_checknumber(L, 2));
        float g = static_cast<float>(luaL_checknumber(L, 3));
        float b = static_cast<float>(luaL_checknumber(L, 4));
        float a = static_cast<float>(luaL_checknumber(L, 5));
        ImGui::PushStyleColor(idx, ImVec4(r, g, b, a));
        return 0;
    }

    static int l_PopStyleColor(lua_State* L)
    {
        int count = opt_int(L, 1, 1);
        ImGui::PopStyleColor(count);
        return 0;
    }

    // ImGui.PushStyleVar(idx, val) or ImGui.PushStyleVar(idx, x, y)
    static int l_PushStyleVar(lua_State* L)
    {
        int idx = static_cast<int>(luaL_checkinteger(L, 1));
        if (lua_isnoneornil(L, 3))
        {
            float val = static_cast<float>(luaL_checknumber(L, 2));
            ImGui::PushStyleVar(idx, val);
        }
        else
        {
            float x = static_cast<float>(luaL_checknumber(L, 2));
            float y = static_cast<float>(luaL_checknumber(L, 3));
            ImGui::PushStyleVar(idx, ImVec2(x, y));
        }
        return 0;
    }

    static int l_PopStyleVar(lua_State* L)
    {
        int count = opt_int(L, 1, 1);
        ImGui::PopStyleVar(count);
        return 0;
    }

    // ── Child windows ────────────────────────────────────────

    static int l_BeginChild(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        float w = opt_float(L, 2, 0.0f);
        float h = opt_float(L, 3, 0.0f);
        bool border = lua_isboolean(L, 4) ? lua_toboolean(L, 4) : false;
        int flags = opt_int(L, 5, 0);
        lua_pushboolean(L, ImGui::BeginChild(id, ImVec2(w, h), border ? ImGuiChildFlags_Borders : 0, flags));
        return 1;
    }

    static int l_EndChild(lua_State* L) { (void)L; ImGui::EndChild(); return 0; }

    // ── Popups / Modals ──────────────────────────────────────

    static int l_OpenPopup(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        ImGui::OpenPopup(id);
        return 0;
    }

    static int l_BeginPopup(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        int flags = opt_int(L, 2, 0);
        lua_pushboolean(L, ImGui::BeginPopup(id, flags));
        return 1;
    }

    static int l_BeginPopupModal(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        int flags = opt_int(L, 2, 0);
        lua_pushboolean(L, ImGui::BeginPopupModal(name, nullptr, flags));
        return 1;
    }

    static int l_EndPopup(lua_State* L) { (void)L; ImGui::EndPopup(); return 0; }
    static int l_CloseCurrentPopup(lua_State* L) { (void)L; ImGui::CloseCurrentPopup(); return 0; }

    // ── Tooltips ─────────────────────────────────────────────

    static int l_BeginTooltip(lua_State* L) { (void)L; ImGui::BeginTooltip(); return 0; }
    static int l_EndTooltip(lua_State* L) { (void)L; ImGui::EndTooltip(); return 0; }
    static int l_SetTooltip(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ImGui::SetTooltip("%s", text);
        return 0;
    }

    // ── Query ────────────────────────────────────────────────

    static int l_IsItemHovered(lua_State* L) { lua_pushboolean(L, ImGui::IsItemHovered()); return 1; }
    static int l_IsItemClicked(lua_State* L) { lua_pushboolean(L, ImGui::IsItemClicked()); return 1; }
    static int l_IsItemActive(lua_State* L) { lua_pushboolean(L, ImGui::IsItemActive()); return 1; }

    static int l_GetFrameCount(lua_State* L) { lua_pushinteger(L, ImGui::GetFrameCount()); return 1; }

    static int l_GetIO_Framerate(lua_State* L)
    {
        lua_pushnumber(L, ImGui::GetIO().Framerate);
        return 1;
    }

    // ── Cursor / ID ──────────────────────────────────────────

    static int l_PushID_Str(lua_State* L)
    {
        const char* id = luaL_checkstring(L, 1);
        ImGui::PushID(id);
        return 0;
    }

    static int l_PushID_Int(lua_State* L)
    {
        int id = static_cast<int>(luaL_checkinteger(L, 1));
        ImGui::PushID(id);
        return 0;
    }

    static int l_PopID(lua_State* L) { (void)L; ImGui::PopID(); return 0; }

    // ── Menu bar ─────────────────────────────────────────────

    static int l_BeginMenuBar(lua_State* L) { lua_pushboolean(L, ImGui::BeginMenuBar()); return 1; }
    static int l_EndMenuBar(lua_State* L) { (void)L; ImGui::EndMenuBar(); return 0; }
    static int l_BeginMenu(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        bool enabled = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true;
        lua_pushboolean(L, ImGui::BeginMenu(label, enabled));
        return 1;
    }
    static int l_EndMenu(lua_State* L) { (void)L; ImGui::EndMenu(); return 0; }
    static int l_MenuItem(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        const char* shortcut = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
        bool selected = lua_isboolean(L, 3) ? lua_toboolean(L, 3) : false;
        bool enabled = lua_isboolean(L, 4) ? lua_toboolean(L, 4) : true;
        lua_pushboolean(L, ImGui::MenuItem(label, shortcut, selected, enabled));
        return 1;
    }

    // ═════════════════════════════════════════════════════════
    // Registration
    // ═════════════════════════════════════════════════════════

    static const luaL_Reg imgui_funcs[] = {
        // Window
        {"Begin", l_Begin},
        {"End", l_End},
        {"SetNextWindowSize", l_SetNextWindowSize},
        {"SetNextWindowPos", l_SetNextWindowPos},
        {"SetNextWindowBgAlpha", l_SetNextWindowBgAlpha},
        {"SetNextItemWidth", l_SetNextItemWidth},
        {"GetWindowSize", l_GetWindowSize},
        {"GetWindowPos", l_GetWindowPos},
        // Layout
        {"SameLine", l_SameLine},
        {"NewLine", l_NewLine},
        {"Separator", l_Separator},
        {"Spacing", l_Spacing},
        {"Indent", l_Indent},
        {"Unindent", l_Unindent},
        {"Dummy", l_Dummy},
        // Text
        {"Text", l_Text},
        {"TextColored", l_TextColored},
        {"TextWrapped", l_TextWrapped},
        {"TextDisabled", l_TextDisabled},
        {"BulletText", l_BulletText},
        // Widgets
        {"Button", l_Button},
        {"SmallButton", l_SmallButton},
        {"InvisibleButton", l_InvisibleButton},
        {"Checkbox", l_Checkbox},
        {"RadioButton", l_RadioButton},
        {"ProgressBar", l_ProgressBar},
        {"SliderFloat", l_SliderFloat},
        {"SliderInt", l_SliderInt},
        {"InputText", l_InputText},
        {"InputTextMultiline", l_InputTextMultiline},
        {"InputFloat", l_InputFloat},
        {"InputInt", l_InputInt},
        {"Combo", l_Combo},
        {"ColorEdit3", l_ColorEdit3},
        {"ColorEdit4", l_ColorEdit4},
        // Tree
        {"CollapsingHeader", l_CollapsingHeader},
        {"TreeNode", l_TreeNode},
        {"TreePop", l_TreePop},
        // Tabs
        {"BeginTabBar", l_BeginTabBar},
        {"EndTabBar", l_EndTabBar},
        {"BeginTabItem", l_BeginTabItem},
        {"EndTabItem", l_EndTabItem},
        // Tables
        {"BeginTable", l_BeginTable},
        {"EndTable", l_EndTable},
        {"TableNextRow", l_TableNextRow},
        {"TableNextColumn", l_TableNextColumn},
        {"TableSetupColumn", l_TableSetupColumn},
        {"TableHeadersRow", l_TableHeadersRow},
        // Style
        {"PushStyleColor", l_PushStyleColor},
        {"PopStyleColor", l_PopStyleColor},
        {"PushStyleVar", l_PushStyleVar},
        {"PopStyleVar", l_PopStyleVar},
        // Child
        {"BeginChild", l_BeginChild},
        {"EndChild", l_EndChild},
        // Popups
        {"OpenPopup", l_OpenPopup},
        {"BeginPopup", l_BeginPopup},
        {"BeginPopupModal", l_BeginPopupModal},
        {"EndPopup", l_EndPopup},
        {"CloseCurrentPopup", l_CloseCurrentPopup},
        // Tooltips
        {"BeginTooltip", l_BeginTooltip},
        {"EndTooltip", l_EndTooltip},
        {"SetTooltip", l_SetTooltip},
        // Query
        {"IsItemHovered", l_IsItemHovered},
        {"IsItemClicked", l_IsItemClicked},
        {"IsItemActive", l_IsItemActive},
        {"GetFrameCount", l_GetFrameCount},
        {"GetFramerate", l_GetIO_Framerate},
        // ID
        {"PushID", l_PushID_Str},
        {"PushIntID", l_PushID_Int},
        {"PopID", l_PopID},
        // Menu
        {"BeginMenuBar", l_BeginMenuBar},
        {"EndMenuBar", l_EndMenuBar},
        {"BeginMenu", l_BeginMenu},
        {"EndMenu", l_EndMenu},
        {"MenuItem", l_MenuItem},
        // Sentinel
        {nullptr, nullptr}
    };

    // ── Enum registration helper ─────────────────────────────
    static void set_enum(lua_State* L, const char* table_name,
                         const std::initializer_list<std::pair<const char*, int>>& values)
    {
        lua_newtable(L);
        for (const auto& [name, val] : values)
        {
            lua_pushinteger(L, val);
            lua_setfield(L, -2, name);
        }
        lua_setglobal(L, table_name);
    }

    auto register_imgui_enums(lua_State* L) -> void
    {
        // ImGuiWindowFlags
        set_enum(L, "ImGuiWindowFlags", {
            {"None", ImGuiWindowFlags_None},
            {"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
            {"NoResize", ImGuiWindowFlags_NoResize},
            {"NoMove", ImGuiWindowFlags_NoMove},
            {"NoScrollbar", ImGuiWindowFlags_NoScrollbar},
            {"NoScrollWithMouse", ImGuiWindowFlags_NoScrollWithMouse},
            {"NoCollapse", ImGuiWindowFlags_NoCollapse},
            {"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize},
            {"NoBackground", ImGuiWindowFlags_NoBackground},
            {"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings},
            {"NoMouseInputs", ImGuiWindowFlags_NoMouseInputs},
            {"MenuBar", ImGuiWindowFlags_MenuBar},
            {"HorizontalScrollbar", ImGuiWindowFlags_HorizontalScrollbar},
            {"NoFocusOnAppearing", ImGuiWindowFlags_NoFocusOnAppearing},
            {"NoBringToFrontOnFocus", ImGuiWindowFlags_NoBringToFrontOnFocus},
            {"AlwaysVerticalScrollbar", ImGuiWindowFlags_AlwaysVerticalScrollbar},
            {"AlwaysHorizontalScrollbar", ImGuiWindowFlags_AlwaysHorizontalScrollbar},
            {"NoNavInputs", ImGuiWindowFlags_NoNavInputs},
            {"NoNavFocus", ImGuiWindowFlags_NoNavFocus},
            {"NoNav", ImGuiWindowFlags_NoNav},
            {"NoDecoration", ImGuiWindowFlags_NoDecoration},
            {"NoInputs", ImGuiWindowFlags_NoInputs},
        });

        // ImGuiCond
        set_enum(L, "ImGuiCond", {
            {"None", ImGuiCond_None},
            {"Always", ImGuiCond_Always},
            {"Once", ImGuiCond_Once},
            {"FirstUseEver", ImGuiCond_FirstUseEver},
            {"Appearing", ImGuiCond_Appearing},
        });

        // ImGuiCol (most commonly used)
        set_enum(L, "ImGuiCol", {
            {"Text", ImGuiCol_Text},
            {"TextDisabled", ImGuiCol_TextDisabled},
            {"WindowBg", ImGuiCol_WindowBg},
            {"ChildBg", ImGuiCol_ChildBg},
            {"PopupBg", ImGuiCol_PopupBg},
            {"Border", ImGuiCol_Border},
            {"BorderShadow", ImGuiCol_BorderShadow},
            {"FrameBg", ImGuiCol_FrameBg},
            {"FrameBgHovered", ImGuiCol_FrameBgHovered},
            {"FrameBgActive", ImGuiCol_FrameBgActive},
            {"TitleBg", ImGuiCol_TitleBg},
            {"TitleBgActive", ImGuiCol_TitleBgActive},
            {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
            {"MenuBarBg", ImGuiCol_MenuBarBg},
            {"ScrollbarBg", ImGuiCol_ScrollbarBg},
            {"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
            {"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered},
            {"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
            {"CheckMark", ImGuiCol_CheckMark},
            {"SliderGrab", ImGuiCol_SliderGrab},
            {"SliderGrabActive", ImGuiCol_SliderGrabActive},
            {"Button", ImGuiCol_Button},
            {"ButtonHovered", ImGuiCol_ButtonHovered},
            {"ButtonActive", ImGuiCol_ButtonActive},
            {"Header", ImGuiCol_Header},
            {"HeaderHovered", ImGuiCol_HeaderHovered},
            {"HeaderActive", ImGuiCol_HeaderActive},
            {"Separator", ImGuiCol_Separator},
            {"SeparatorHovered", ImGuiCol_SeparatorHovered},
            {"SeparatorActive", ImGuiCol_SeparatorActive},
            {"ResizeGrip", ImGuiCol_ResizeGrip},
            {"ResizeGripHovered", ImGuiCol_ResizeGripHovered},
            {"ResizeGripActive", ImGuiCol_ResizeGripActive},
            {"Tab", ImGuiCol_Tab},
            {"TabHovered", ImGuiCol_TabHovered},
            {"TabSelected", ImGuiCol_TabSelected},
            {"TableHeaderBg", ImGuiCol_TableHeaderBg},
            {"TableBorderStrong", ImGuiCol_TableBorderStrong},
            {"TableBorderLight", ImGuiCol_TableBorderLight},
            {"TableRowBg", ImGuiCol_TableRowBg},
            {"TableRowBgAlt", ImGuiCol_TableRowBgAlt},
        });

        // ImGuiStyleVar
        set_enum(L, "ImGuiStyleVar", {
            {"Alpha", ImGuiStyleVar_Alpha},
            {"DisabledAlpha", ImGuiStyleVar_DisabledAlpha},
            {"WindowPadding", ImGuiStyleVar_WindowPadding},
            {"WindowRounding", ImGuiStyleVar_WindowRounding},
            {"WindowBorderSize", ImGuiStyleVar_WindowBorderSize},
            {"WindowMinSize", ImGuiStyleVar_WindowMinSize},
            {"ChildRounding", ImGuiStyleVar_ChildRounding},
            {"ChildBorderSize", ImGuiStyleVar_ChildBorderSize},
            {"PopupRounding", ImGuiStyleVar_PopupRounding},
            {"PopupBorderSize", ImGuiStyleVar_PopupBorderSize},
            {"FramePadding", ImGuiStyleVar_FramePadding},
            {"FrameRounding", ImGuiStyleVar_FrameRounding},
            {"FrameBorderSize", ImGuiStyleVar_FrameBorderSize},
            {"ItemSpacing", ImGuiStyleVar_ItemSpacing},
            {"ItemInnerSpacing", ImGuiStyleVar_ItemInnerSpacing},
            {"IndentSpacing", ImGuiStyleVar_IndentSpacing},
            {"CellPadding", ImGuiStyleVar_CellPadding},
            {"ScrollbarSize", ImGuiStyleVar_ScrollbarSize},
            {"ScrollbarRounding", ImGuiStyleVar_ScrollbarRounding},
            {"GrabMinSize", ImGuiStyleVar_GrabMinSize},
            {"GrabRounding", ImGuiStyleVar_GrabRounding},
            {"TabRounding", ImGuiStyleVar_TabRounding},
        });

        // ImGuiTableFlags (commonly used)
        set_enum(L, "ImGuiTableFlags", {
            {"None", ImGuiTableFlags_None},
            {"Resizable", ImGuiTableFlags_Resizable},
            {"Reorderable", ImGuiTableFlags_Reorderable},
            {"Hideable", ImGuiTableFlags_Hideable},
            {"Sortable", ImGuiTableFlags_Sortable},
            {"ContextMenuInBody", ImGuiTableFlags_ContextMenuInBody},
            {"RowBg", ImGuiTableFlags_RowBg},
            {"BordersInnerH", ImGuiTableFlags_BordersInnerH},
            {"BordersOuterH", ImGuiTableFlags_BordersOuterH},
            {"BordersInnerV", ImGuiTableFlags_BordersInnerV},
            {"BordersOuterV", ImGuiTableFlags_BordersOuterV},
            {"BordersH", ImGuiTableFlags_BordersH},
            {"BordersV", ImGuiTableFlags_BordersV},
            {"BordersInner", ImGuiTableFlags_BordersInner},
            {"BordersOuter", ImGuiTableFlags_BordersOuter},
            {"Borders", ImGuiTableFlags_Borders},
            {"SizingFixedFit", ImGuiTableFlags_SizingFixedFit},
            {"SizingFixedSame", ImGuiTableFlags_SizingFixedSame},
            {"SizingStretchProp", ImGuiTableFlags_SizingStretchProp},
            {"SizingStretchSame", ImGuiTableFlags_SizingStretchSame},
            {"ScrollX", ImGuiTableFlags_ScrollX},
            {"ScrollY", ImGuiTableFlags_ScrollY},
        });
    }

    auto register_imgui_bindings(lua_State* L) -> void
    {
        // Register the ImGui function table as a global
        luaL_newlib(L, imgui_funcs);
        lua_setglobal(L, "ImGui");

        // Register enum tables
        register_imgui_enums(L);
    }

} // namespace RC::LuaType
