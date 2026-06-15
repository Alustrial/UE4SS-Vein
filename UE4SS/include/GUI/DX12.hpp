#pragma once
// ─────────────────────────────────────────────────────────────
// VeinCF DX12 In-Game ImGui Overlay
//
// Hooks the game's DXGI SwapChain::Present to render ImGui
// directly inside the VEIN game viewport.  All VeinCF mod UI
// (admin panels, trader menus, debug overlays) renders here.
//
// The hook is installed once during UE4SS startup and
// dispatches registered Lua draw callbacks every frame.
// ─────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

struct ImGuiContext;

namespace RC::GUI
{
    // ── Callback types ───────────────────────────────────────
    // C++ mods (or the Lua dispatch layer) register a draw
    // callback that is invoked once per frame inside the
    // ImGui NewFrame / Render pair.
    using OverlayDrawCallback = std::function<void()>;

    struct OverlayPanel
    {
        std::string name;
        OverlayDrawCallback callback;
        bool visible{true};
    };

    // ── Public API ───────────────────────────────────────────
    class DX12Overlay
    {
    public:
        // Install the Present hook.  Call once from on_program_start().
        static auto install() -> bool;

        // Tear everything down (ImGui context, D3D12 resources, hook).
        static auto shutdown() -> void;

        // Panel management — thread-safe.
        static auto register_panel(const std::string& name, OverlayDrawCallback cb) -> void;
        static auto unregister_panel(const std::string& name) -> void;
        static auto set_panel_visible(const std::string& name, bool visible) -> void;
        static auto get_panel_count() -> size_t;

        // Global overlay toggle (e.g. bound to a hotkey).
        static auto set_overlay_visible(bool visible) -> void;
        static auto is_overlay_visible() -> bool;
        static auto toggle_overlay() -> void;

        // Raw draw callback — lower level than panels.
        // Called every frame regardless of panel state.
        static auto register_draw_callback(const std::string& id, OverlayDrawCallback cb) -> void;
        static auto unregister_draw_callback(const std::string& id) -> void;

        // Retrieve the D3D12 command queue the game is using (needed by some subsystems).
        static auto get_command_queue() -> ID3D12CommandQueue*;

    private:
        // The actual Present hook implementation.
        static auto STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap_chain_base, UINT sync_interval, UINT flags) -> HRESULT;

        // Internal init called on first Present after hook install.
        static auto init_imgui(IDXGISwapChain3* swap_chain) -> bool;
        static auto create_render_targets(IDXGISwapChain3* swap_chain) -> bool;
        static auto cleanup_render_targets() -> void;
        static auto render_overlay() -> void;

        // WndProc sub-class for input forwarding to ImGui.
        static auto CALLBACK wndproc_hook(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT;
    };

} // namespace RC::GUI
