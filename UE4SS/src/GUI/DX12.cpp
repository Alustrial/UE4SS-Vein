// ─────────────────────────────────────────────────────────────
// VeinCF DX12 In-Game ImGui Overlay
//
// Hooks IDXGISwapChain::Present via vtable patching to render
// ImGui directly inside the VEIN (UE5.6 / DX12) game window.
//
// Flow:
//   install()
//     → create a throwaway D3D12 device + swap chain
//     → read Present's address from the vtable
//     → patch it with hooked_present()
//     → release throwaway resources
//
//   hooked_present()  (called every frame by the engine)
//     → first call: init_imgui() — create ImGui context,
//       descriptor heaps, command allocators, render targets
//     → every frame: NewFrame → render_overlay() → Render
//       → execute command list → call original Present
// ─────────────────────────────────────────────────────────────

#include <GUI/DX12.hpp>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <Helpers/String.hpp>
#include <vector>

// Windows + DX12
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// ImGui
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

// PolyHook for the vtable swap
#include <polyhook2/Detour/x64Detour.hpp>

// UE4SS output for logging
#include <DynamicOutput/DynamicOutput.hpp>

// Forward-declare the Win32 ImGui handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace RC;

namespace RC::GUI
{
    // ── Static state ─────────────────────────────────────────
    // D3D12 resources we create for ImGui rendering
    static ID3D12Device*                    g_device            = nullptr;
    static ID3D12CommandQueue*              g_command_queue      = nullptr;  // game's queue (captured from hook)
    static ID3D12CommandQueue*              g_our_command_queue  = nullptr;  // our own queue for ImGui
    static ID3D12DescriptorHeap*            g_rtv_heap          = nullptr;
    static ID3D12DescriptorHeap*            g_srv_heap          = nullptr;
    static ID3D12GraphicsCommandList*       g_command_list       = nullptr;
    static std::vector<ID3D12CommandAllocator*> g_command_allocators;
    static std::vector<ID3D12Resource*>     g_back_buffers;
    static UINT                             g_buffer_count       = 0;
    static UINT                             g_rtv_descriptor_size = 0;

    // Hook plumbing
    static std::unique_ptr<PLH::x64Detour>  g_present_hook;
    static uint64_t                         g_present_trampoline = 0;
    static bool                             g_initialized        = false;
    static bool                             g_overlay_visible    = false;
    static DXGI_FORMAT                      g_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Input forwarding
    static HWND                             g_game_hwnd          = nullptr;
    static WNDPROC                          g_original_wndproc   = nullptr;

    // Panel / callback registry
    static std::mutex                       g_panels_mutex;
    static std::vector<OverlayPanel>        g_panels;

    struct DrawCallbackEntry { std::string id; OverlayDrawCallback cb; };
    static std::mutex                       g_draw_cb_mutex;
    static std::vector<DrawCallbackEntry>   g_draw_callbacks;

    // ── Helper: get Present vtable index ─────────────────────
    // IDXGISwapChain::Present is vtable index 8.
    static constexpr int PRESENT_VTABLE_INDEX = 8;
    // ExecuteCommandLists is vtable index 10 on ID3D12CommandQueue.
    static constexpr int EXECUTE_CMD_LISTS_VTABLE_INDEX = 10;

    // ── Helper: get the game's command queue ─────────────────
    // We intercept ExecuteCommandLists to grab the queue pointer
    // because DXGI doesn't give us direct access to it.
    using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
        ID3D12CommandQueue* self, UINT num, ID3D12CommandList* const* lists);
    static uint64_t g_execute_trampoline = 0;
    static std::unique_ptr<PLH::x64Detour> g_execute_hook;

    static void STDMETHODCALLTYPE hooked_execute_command_lists(
        ID3D12CommandQueue* self, UINT num, ID3D12CommandList* const* lists)
    {
        // Capture the first queue we see — that's the game's main queue.
        if (!g_command_queue)
        {
            g_command_queue = self;
            OutputDebugStringW(L"[VeinCF/DX12] Captured command queue\n");
        }
        auto original = reinterpret_cast<ExecuteCommandListsFn>(g_execute_trampoline);
        original(self, num, lists);
    }

    // ── WndProc hook ─────────────────────────────────────────
    auto CALLBACK DX12Overlay::wndproc_hook(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT
    {
        if (g_overlay_visible && g_initialized)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
            {
                return TRUE;
            }

            // If ImGui wants mouse/keyboard, consume the message
            if (ImGui::GetIO().WantCaptureMouse)
            {
                switch (msg)
                {
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                case WM_MOUSEMOVE:
                    return TRUE;
                }
            }
            if (ImGui::GetIO().WantCaptureKeyboard)
            {
                switch (msg)
                {
                case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
                case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                    return TRUE;
                }
            }
        }
        return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
    }

    // ── Create render targets ────────────────────────────────
    auto DX12Overlay::create_render_targets(IDXGISwapChain3* swap_chain) -> bool
    {
        for (UINT i = 0; i < g_buffer_count; i++)
        {
            ID3D12Resource* back_buffer = nullptr;
            if (FAILED(swap_chain->GetBuffer(i, IID_PPV_ARGS(&back_buffer))))
                return false;
            g_back_buffers.push_back(back_buffer);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv_handle.ptr += static_cast<SIZE_T>(i) * g_rtv_descriptor_size;
            g_device->CreateRenderTargetView(back_buffer, nullptr, rtv_handle);
        }
        return true;
    }

    auto DX12Overlay::cleanup_render_targets() -> void
    {
        for (auto* bb : g_back_buffers)
        {
            if (bb) bb->Release();
        }
        g_back_buffers.clear();
    }

    // ── Helper: wait for GPU to finish all queued work ─────
    static bool wait_for_gpu()
    {
        ID3D12Fence* fence = nullptr;
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) { fence->Release(); return false; }

        g_command_queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1)
        {
            fence->SetEventOnCompletion(1, event);
            WaitForSingleObject(event, 5000);  // 5 second timeout
        }

        CloseHandle(event);
        fence->Release();
        return true;
    }

    // ── ImGui initialization (first Present call) ────────────
    auto DX12Overlay::init_imgui(IDXGISwapChain3* swap_chain) -> bool
    {
        // NOTE: No Output::send calls in this function!
        // We're on the render thread inside Present — calling Output::send
        // can deadlock if UE4SS's console GUI holds a lock.

        // Get the device from the swap chain
        if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&g_device))))
            return false;

        // Get swap chain desc for buffer count and window handle
        DXGI_SWAP_CHAIN_DESC sc_desc{};
        if (FAILED(swap_chain->GetDesc(&sc_desc)))
            return false;

        g_buffer_count = sc_desc.BufferCount;
        g_game_hwnd = sc_desc.OutputWindow;
        g_back_buffer_format = sc_desc.BufferDesc.Format;

        // Wait for the game's command queue to be captured (so we know the device works)
        if (!g_command_queue)
            return false;

        // Create RTV descriptor heap
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = g_buffer_count;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;
            if (FAILED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_rtv_heap))))
                return false;
            g_rtv_descriptor_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }

        // Create SRV descriptor heap (for ImGui font texture)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            desc.NodeMask = 0;
            if (FAILED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_srv_heap))))
                return false;
        }

        // Create command allocators (one per back buffer)
        g_command_allocators.resize(g_buffer_count);
        for (UINT i = 0; i < g_buffer_count; i++)
        {
            if (FAILED(g_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&g_command_allocators[i]))))
                return false;
        }

        // Create command list
        if (FAILED(g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_command_allocators[0], nullptr,
                IID_PPV_ARGS(&g_command_list))))
            return false;
        g_command_list->Close();

        // Create render target views
        if (!create_render_targets(swap_chain))
            return false;

        // ── ImGui setup ──────────────────────────────────────
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;  // Don't save layout to disk

        // Dark style tuned for game overlay readability
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.Alpha = 0.95f;
        style.WindowRounding = 6.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.WindowBorderSize = 1.0f;

        // Font setup — use default for now, mods can add fonts later
        io.Fonts->AddFontDefault();

        // Init ImGui backends
        ImGui_ImplWin32_Init(g_game_hwnd);
        ImGui_ImplDX12_Init(
            g_device,
            static_cast<int>(g_buffer_count),
            g_back_buffer_format,
            g_srv_heap,
            g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
            g_srv_heap->GetGPUDescriptorHandleForHeapStart()
        );

        // Sub-class the game window to intercept input for ImGui
        // DISABLED FOR BISECT — testing init only
        // g_original_wndproc = reinterpret_cast<WNDPROC>(
        //     SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc_hook)));

        g_initialized = true;
        return true;
    }

    // ── Overlay rendering ────────────────────────────────────
    auto DX12Overlay::render_overlay() -> void
    {
        // Draw registered panels
        {
            std::lock_guard lock(g_panels_mutex);
            for (auto& panel : g_panels)
            {
                if (!panel.visible) continue;
                try
                {
                    panel.callback();
                }
                catch (...)
                {
                    // Swallow — we're on the render thread, can't safely log
                }
            }
        }

        // Draw raw callbacks
        {
            std::lock_guard lock(g_draw_cb_mutex);
            for (auto& entry : g_draw_callbacks)
            {
                try
                {
                    entry.cb();
                }
                catch (...)
                {
                    // Swallow — we're on the render thread, can't safely log
                }
            }
        }
    }

    static bool g_init_failed = false;  // permanent bail-out flag

    // ═══════════════════════════════════════════════════════════
    // INCREMENTAL RENDER STEPS — change this value to advance:
    //   1 = Hook only (passthrough, log on F7)
    //   2 = Draw colored rectangle via raw D3D12 (no ImGui)
    //   3 = ImGui init + empty frame (NewFrame/EndFrame, no draw)
    //   4 = Full ImGui render with panels
    // ═══════════════════════════════════════════════════════════
    #define VEINCF_RENDER_STEP 2

    // ── Hooked Present ───────────────────────────────────────
    auto STDMETHODCALLTYPE DX12Overlay::hooked_present(
        IDXGISwapChain* swap_chain_base, UINT sync_interval, UINT flags) -> HRESULT
    {
        // ── STEP 1: Pure passthrough — just prove the hook works ──
        // On F7 toggle, log via OutputDebugString. No GPU work at all.
#if VEINCF_RENDER_STEP >= 1
        {
            static bool s_logged_hook = false;
            if (!s_logged_hook)
            {
                OutputDebugStringW(L"[VeinCF/DX12] STEP 1: Present hook is running\n");
                s_logged_hook = true;
            }
            if (g_overlay_visible)
            {
                static bool s_logged_f7 = false;
                if (!s_logged_f7)
                {
                    OutputDebugStringW(L"[VeinCF/DX12] STEP 1: F7 toggled — overlay requested\n");
                    s_logged_f7 = true;
                }
            }
        }
#endif

#if VEINCF_RENDER_STEP <= 1
        // Step 1: pure passthrough — no GPU work
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        auto original = reinterpret_cast<PresentFn>(g_present_trampoline);
        return original(swap_chain_base, sync_interval, flags);
#endif

        // ── STEP 2+: Need swap chain and device ──
#if VEINCF_RENDER_STEP >= 2
        if (g_init_failed || !g_overlay_visible)
        {
            using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
            auto original = reinterpret_cast<PresentFn>(g_present_trampoline);
            return original(swap_chain_base, sync_interval, flags);
        }
#endif

        // ── STEP 2: Draw a colored rectangle using raw D3D12 ──
        // Tests: command allocator, command list, resource barriers,
        //        back buffer access, ExecuteCommandLists — all without ImGui.
#if VEINCF_RENDER_STEP == 2
        if (!g_initialized && g_command_queue)
        {
            // Bare minimum: get device, create 1 command allocator + 1 command list
            IDXGISwapChain3* sc3 = nullptr;
            if (FAILED(swap_chain_base->QueryInterface(IID_PPV_ARGS(&sc3))))
            { g_init_failed = true; goto passthrough; }

            if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&g_device))))
            { sc3->Release(); g_init_failed = true; goto passthrough; }

            sc3->Release();

            g_command_allocators.resize(1);
            if (FAILED(g_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&g_command_allocators[0]))))
            { g_init_failed = true; goto passthrough; }

            if (FAILED(g_device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    g_command_allocators[0], nullptr,
                    IID_PPV_ARGS(&g_command_list))))
            { g_init_failed = true; goto passthrough; }

            g_command_list->Close();
            g_initialized = true;
            OutputDebugStringW(L"[VeinCF/DX12] STEP 2: Empty cmd list ready\n");
        }

        // Step 2a: Submit an EMPTY command list — no barriers, no draws.
        // Just proves we can call ExecuteCommandLists on the game's queue.
        if (g_initialized && g_command_queue)
        {
            auto* alloc = g_command_allocators[0];
            alloc->Reset();
            g_command_list->Reset(alloc, nullptr);
            g_command_list->Close();

            ID3D12CommandList* cmd_lists[] = { g_command_list };
            auto original_ecl = reinterpret_cast<ExecuteCommandListsFn>(g_execute_trampoline);
            original_ecl(g_command_queue, 1, cmd_lists);
        }

        passthrough:
#endif  // STEP 2

        // ── STEP 3: ImGui init + empty frame ──
#if VEINCF_RENDER_STEP == 3
        if (!g_initialized)
        {
            IDXGISwapChain3* swap_chain = nullptr;
            if (FAILED(swap_chain_base->QueryInterface(IID_PPV_ARGS(&swap_chain))))
            {
                g_init_failed = true;
                goto step3_passthrough;
            }
            if (!init_imgui(swap_chain))
            {
                swap_chain->Release();
                g_overlay_visible = false;
                goto step3_passthrough;
            }
            swap_chain->Release();
            OutputDebugStringW(L"[VeinCF/DX12] STEP 3: ImGui initialized\n");
        }

        // Run ImGui frame cycle but draw nothing
        if (g_initialized && g_command_queue)
        {
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swap_chain_base->QueryInterface(IID_PPV_ARGS(&sc3))))
            {
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                // Draw nothing — just an empty frame
                ImGui::EndFrame();
                ImGui::Render();

                UINT idx = sc3->GetCurrentBackBufferIndex();
                if (idx < g_command_allocators.size() && idx < g_back_buffers.size())
                {
                    auto* alloc = g_command_allocators[idx];
                    alloc->Reset();
                    g_command_list->Reset(alloc, nullptr);

                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = g_back_buffers[idx];
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    g_command_list->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += static_cast<SIZE_T>(idx) * g_rtv_descriptor_size;
                    g_command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                    ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
                    g_command_list->SetDescriptorHeaps(1, heaps);
                    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);

                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    g_command_list->ResourceBarrier(1, &barrier);

                    g_command_list->Close();

                    ID3D12CommandList* cmd_lists[] = { g_command_list };
                    auto original_ecl = reinterpret_cast<ExecuteCommandListsFn>(g_execute_trampoline);
                    original_ecl(g_command_queue, 1, cmd_lists);
                }
                sc3->Release();
            }
        }

        step3_passthrough:
#endif  // STEP 3

        // ── STEP 4: Full ImGui render with panels ──
#if VEINCF_RENDER_STEP >= 4
        if (!g_initialized)
        {
            IDXGISwapChain3* swap_chain = nullptr;
            if (FAILED(swap_chain_base->QueryInterface(IID_PPV_ARGS(&swap_chain))))
            {
                g_init_failed = true;
                goto step4_passthrough;
            }
            if (!init_imgui(swap_chain))
            {
                swap_chain->Release();
                g_overlay_visible = false;
                goto step4_passthrough;
            }
            swap_chain->Release();
        }

        if (g_initialized && g_command_queue)
        {
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swap_chain_base->QueryInterface(IID_PPV_ARGS(&sc3))))
            {
                try
                {
                    ImGui_ImplDX12_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();

                    render_overlay();

                    ImGui::EndFrame();
                    ImGui::Render();

                    UINT idx = sc3->GetCurrentBackBufferIndex();
                    if (idx < g_command_allocators.size() && idx < g_back_buffers.size())
                    {
                        auto* alloc = g_command_allocators[idx];
                        alloc->Reset();
                        g_command_list->Reset(alloc, nullptr);

                        D3D12_RESOURCE_BARRIER barrier{};
                        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        barrier.Transition.pResource = g_back_buffers[idx];
                        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        g_command_list->ResourceBarrier(1, &barrier);

                        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                        rtv.ptr += static_cast<SIZE_T>(idx) * g_rtv_descriptor_size;
                        g_command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                        ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
                        g_command_list->SetDescriptorHeaps(1, heaps);
                        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);

                        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                        g_command_list->ResourceBarrier(1, &barrier);

                        g_command_list->Close();

                        ID3D12CommandList* cmd_lists[] = { g_command_list };
                        auto original_ecl = reinterpret_cast<ExecuteCommandListsFn>(g_execute_trampoline);
                        original_ecl(g_command_queue, 1, cmd_lists);
                    }
                }
                catch (...)
                {
                    // Skip frame on error
                }
                sc3->Release();
            }
        }

        step4_passthrough:
#endif  // STEP 4

        // Call original Present
        {
            using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
            auto original = reinterpret_cast<PresentFn>(g_present_trampoline);
            return original(swap_chain_base, sync_interval, flags);
        }
    }

    // ── Install the hook ─────────────────────────────────────
    auto DX12Overlay::install() -> bool
    {
        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] Installing DX12 Present hook...\n"));

        // Create a throwaway D3D12 device to get the vtable
        ID3D12Device* temp_device = nullptr;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&temp_device))))
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to create temp D3D12 device\n"));
            return false;
        }

        // Create a temp command queue to hook ExecuteCommandLists
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        ID3D12CommandQueue* temp_queue = nullptr;
        if (FAILED(temp_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&temp_queue))))
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to create temp command queue\n"));
            temp_device->Release();
            return false;
        }

        // Create a throwaway DXGI factory + swap chain
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to create DXGI factory\n"));
            temp_queue->Release();
            temp_device->Release();
            return false;
        }

        // We need a real HWND for the swap chain
        HWND temp_hwnd = CreateWindowExW(0, L"STATIC", L"VeinCF_DX12_Init",
            WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);

        DXGI_SWAP_CHAIN_DESC1 sc_desc{};
        sc_desc.Width = 1;
        sc_desc.Height = 1;
        sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc_desc.SampleDesc.Count = 1;
        sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc_desc.BufferCount = 2;
        sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        IDXGISwapChain1* temp_swap_chain1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(temp_queue, temp_hwnd, &sc_desc, nullptr, nullptr, &temp_swap_chain1)))
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to create temp swap chain\n"));
            DestroyWindow(temp_hwnd);
            factory->Release();
            temp_queue->Release();
            temp_device->Release();
            return false;
        }

        // Get vtable pointers
        void** swap_chain_vtable = *reinterpret_cast<void***>(temp_swap_chain1);
        void** command_queue_vtable = *reinterpret_cast<void***>(temp_queue);

        auto present_target = reinterpret_cast<uint64_t>(swap_chain_vtable[PRESENT_VTABLE_INDEX]);
        auto execute_target = reinterpret_cast<uint64_t>(command_queue_vtable[EXECUTE_CMD_LISTS_VTABLE_INDEX]);

        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] Present address: {:#x}\n"), present_target);
        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] ExecuteCommandLists address: {:#x}\n"), execute_target);

        // Clean up temp resources
        temp_swap_chain1->Release();
        DestroyWindow(temp_hwnd);
        factory->Release();
        temp_queue->Release();
        temp_device->Release();

        // ── Hook ExecuteCommandLists to capture the game's command queue ──
        g_execute_hook = std::make_unique<PLH::x64Detour>(
            execute_target,
            reinterpret_cast<uint64_t>(&hooked_execute_command_lists),
            &g_execute_trampoline);

        if (!g_execute_hook->hook())
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to hook ExecuteCommandLists\n"));
            return false;
        }
        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] ExecuteCommandLists hooked\n"));

        // ── Hook Present ──
        g_present_hook = std::make_unique<PLH::x64Detour>(
            present_target,
            reinterpret_cast<uint64_t>(&hooked_present),
            &g_present_trampoline);

        if (!g_present_hook->hook())
        {
            Output::send<LogLevel::Error>(STR("[VeinCF/DX12] Failed to hook Present\n"));
            g_execute_hook->unHook();
            return false;
        }

        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] Present hooked — DX12 overlay will activate on next frame\n"));
        return true;
    }

    // ── Shutdown ─────────────────────────────────────────────
    auto DX12Overlay::shutdown() -> void
    {
        if (g_initialized)
        {
            // Restore WndProc
            if (g_game_hwnd && g_original_wndproc)
            {
                SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
                g_original_wndproc = nullptr;
            }

            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();

            cleanup_render_targets();

            if (g_command_list) { g_command_list->Release(); g_command_list = nullptr; }
            for (auto* alloc : g_command_allocators)
            {
                if (alloc) alloc->Release();
            }
            g_command_allocators.clear();
            if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
            if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
            // Release our own queue, but not the game's
            if (g_our_command_queue) { g_our_command_queue->Release(); g_our_command_queue = nullptr; }
            // Don't release g_device or g_command_queue — they belong to the game
            g_device = nullptr;
            g_command_queue = nullptr;

            g_initialized = false;
        }

        if (g_present_hook) { g_present_hook->unHook(); g_present_hook.reset(); }
        if (g_execute_hook) { g_execute_hook->unHook(); g_execute_hook.reset(); }

        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] Overlay shut down\n"));
    }

    // ── Panel management ─────────────────────────────────────
    auto DX12Overlay::register_panel(const std::string& name, OverlayDrawCallback cb) -> void
    {
        std::lock_guard lock(g_panels_mutex);
        // Remove existing with same name
        g_panels.erase(
            std::remove_if(g_panels.begin(), g_panels.end(),
                [&](const OverlayPanel& p) { return p.name == name; }),
            g_panels.end());
        g_panels.push_back({name, std::move(cb), true});
        Output::send<LogLevel::Verbose>(STR("[VeinCF/DX12] Panel registered: {}\n"), ensure_str(name));
    }

    auto DX12Overlay::unregister_panel(const std::string& name) -> void
    {
        std::lock_guard lock(g_panels_mutex);
        g_panels.erase(
            std::remove_if(g_panels.begin(), g_panels.end(),
                [&](const OverlayPanel& p) { return p.name == name; }),
            g_panels.end());
    }

    auto DX12Overlay::set_panel_visible(const std::string& name, bool visible) -> void
    {
        std::lock_guard lock(g_panels_mutex);
        for (auto& panel : g_panels)
        {
            if (panel.name == name)
            {
                panel.visible = visible;
                break;
            }
        }
    }

    auto DX12Overlay::get_panel_count() -> size_t
    {
        std::lock_guard lock(g_panels_mutex);
        return g_panels.size();
    }

    auto DX12Overlay::set_overlay_visible(bool visible) -> void { g_overlay_visible = visible; }
    auto DX12Overlay::is_overlay_visible() -> bool { return g_overlay_visible; }
    auto DX12Overlay::toggle_overlay() -> void { g_overlay_visible = !g_overlay_visible; }

    auto DX12Overlay::register_draw_callback(const std::string& id, OverlayDrawCallback cb) -> void
    {
        std::lock_guard lock(g_draw_cb_mutex);
        g_draw_callbacks.erase(
            std::remove_if(g_draw_callbacks.begin(), g_draw_callbacks.end(),
                [&](const DrawCallbackEntry& e) { return e.id == id; }),
            g_draw_callbacks.end());
        g_draw_callbacks.push_back({id, std::move(cb)});
    }

    auto DX12Overlay::unregister_draw_callback(const std::string& id) -> void
    {
        std::lock_guard lock(g_draw_cb_mutex);
        g_draw_callbacks.erase(
            std::remove_if(g_draw_callbacks.begin(), g_draw_callbacks.end(),
                [&](const DrawCallbackEntry& e) { return e.id == id; }),
            g_draw_callbacks.end());
    }

    auto DX12Overlay::get_command_queue() -> ID3D12CommandQueue*
    {
        return g_command_queue;
    }

} // namespace RC::GUI
