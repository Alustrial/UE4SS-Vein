#define NOMINMAX

#include <atomic>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <polyhook2/Detour/x64Detour.hpp>
#include <Zydis/Zydis.h>  // VeinCF: native disassembler (DisasmFunction)

#include <DynamicOutput/DynamicOutput.hpp>
#include <ExceptionHandling.hpp>
#include <Helpers/Format.hpp>
#include <Helpers/String.hpp>
#include <Input/Handler.hpp>
#include <LuaLibrary.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaType/LuaAActor.hpp>
#include <LuaType/LuaCustomProperty.hpp>
#include <LuaType/LuaFName.hpp>
#include <LuaType/LuaFText.hpp>
#include <LuaType/LuaUnrealString.hpp>
#include <LuaType/LuaFOutputDevice.hpp>
#include <LuaType/LuaModRef.hpp>
#include <LuaType/LuaUClass.hpp>
#include <LuaType/LuaUObject.hpp>
#include <LuaType/LuaFURL.hpp>
#include <LuaType/LuaThreadId.hpp>
#include <Mod/CppMod.hpp>
#include <Mod/LuaMod.hpp>
#pragma warning(disable : 4005)
#include <GUI/Dumpers.hpp>
#include <UE4SSProgram.hpp>
#include <USMapGenerator/Generator.hpp>
#include <Unreal/Core/HAL/Platform.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/FURL.hpp>
#include <Unreal/FWorldContext.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/PackageName.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Core/Containers/FUtf8String.hpp>
#include <Unreal/Core/Containers/FAnsiString.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/CoreUObject/UObject/FUtf8StrProperty.hpp>
#include <Unreal/TypeChecker.hpp>
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/FPrimaryAssetId.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/Core/Containers/FString.hpp>  // VeinCF: FString for ScanPathsForType

// VeinCF: In-game DX12 ImGui overlay + Lua bindings
#include <LuaType/LuaImGui.hpp>
#include <LuaType/LuaOverlay.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UGameViewportClient.hpp>
#include <Unreal/UKismetSystemLibrary.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UPackage.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <UnrealCustom/CustomProperty.hpp>
#include <UE4SSRuntime.hpp>
#include <Unreal/UnrealInitializer.hpp>

#if PLATFORM_WINDOWS
#include <Unreal/Core/Windows/AllowWindowsPlatformTypes.hpp>
#endif
#pragma warning(default : 4005)

// VeinCF: SEH helper functions — must not use C++ objects with destructors
// These are in a separate .c-style compilation unit conceptually; no std::string allowed.
#pragma optimize("", off)

// Probe if a pointer-sized value at `val` is a valid UObject.
// Writes the object and class FName ComparisonIndex into out params.
// Returns 1 if valid, 0 if not.
// SEH-safe: probe whether a raw pointer is a valid UObject
static int veincf_probe_uobject(uintptr_t val, Unreal::UObject** out_obj)
{
    if (val == 0 || val < 0x10000 || val == 0xFFFFFFFFFFFFFFFF || val == 0xCDCDCDCDCDCDCDCD)
        return 0;

    __try
    {
        Unreal::UObject* candidate = reinterpret_cast<Unreal::UObject*>(val);
        auto* cls = candidate->GetClassPrivate();
        if (!cls) return 0;
        volatile auto idx = cls->GetFName().GetComparisonIndex();
        (void)idx;
        if (out_obj) *out_obj = candidate;
        return 1;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Inner helper: ToString allocates (std::wstring has a destructor), so it lives
// in THIS frame, not the __try frame. Required so the SEH wrapper holds no
// unwindable C++ objects (avoids C2712).
static bool veincf_class_name_inner(Unreal::UObject* obj, std::wstring& out)
{
    out = obj->GetClassPrivate()->GetFName().ToString();
    return true;
}

// SEH-safe: resolve the class FName string of an ALREADY-PROBED UObject.
// Only call after veincf_probe_uobject returns 1 for this pointer.
static bool veincf_safe_class_name(Unreal::UObject* obj, std::wstring& out)
{
    __try
    {
        return veincf_class_name_inner(obj, out);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Inner helper for an object's OWN FName (e.g. a UClass's own name like
// "BP_Workbench_C"), as opposed to its class's name. ToString allocates -> inner frame.
static bool veincf_object_name_inner(Unreal::UObject* obj, std::wstring& out)
{
    out = obj->GetFName().ToString();
    return true;
}

// SEH-safe: resolve an object's own FName string.
static bool veincf_safe_object_name(Unreal::UObject* obj, std::wstring& out)
{
    __try
    {
        return veincf_object_name_inner(obj, out);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-safe: interpret 8 bytes at slot_addr as an FWeakObjectPtr {int32 index,
// int32 serial} and resolve it through GUObjectArray. Returns the live UObject
// or nullptr (stale/garbage/not-a-weak-ptr). FWeakObjectPtr::Get() itself does a
// serial-number match so it returns null rather than crashing on a dead handle.
static Unreal::UObject* veincf_resolve_weak(void* slot_addr)
{
    __try
    {
        int32_t index = *reinterpret_cast<int32_t*>(slot_addr);
        int32_t serial = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(slot_addr) + 4);
        // Plausibility gate: real live handles have positive index and serial.
        if (serial <= 0 || index <= 0 || index > 0x08000000) return nullptr;
        Unreal::FWeakObjectPtr* wp = reinterpret_cast<Unreal::FWeakObjectPtr*>(slot_addr);
        return wp->Get();
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// SEH-safe: read 8 bytes at addr. Returns false if the address faults (e.g. we
// scanned past the end of an object's allocation into an unmapped page). The graph
// mapper uses this so it can walk arbitrary/varied-size objects without crashing.
static bool veincf_safe_read_u64(void* addr, uintptr_t* out)
{
    __try
    {
        *out = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// VeinCF: SEH-safe FName -> string by comparison index. Reconstructs an FName
// from a raw ComparisonIndex (as found in memory, e.g. a TMap<FName,...> key) and
// resolves its display string via the global name pool. ToString allocates
// (std::wstring has a destructor) so it lives in this inner frame; the __try
// wrapper below stays POD (avoids C2712). The universal "name a raw FName" verb.
static bool veincf_fname_string_inner(int32_t comparison_index, std::wstring& out)
{
    Unreal::FName n(static_cast<int64_t>(comparison_index));
    out = n.ToString();
    return true;
}
static bool veincf_safe_fname_string(int32_t comparison_index, std::wstring& out)
{
    __try
    {
        return veincf_fname_string_inner(comparison_index, out);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-safe: scan `len` bytes of code at base for E8 (call rel32) instructions.
// Fills offsets[] + targets[] (computed absolute call targets) up to maxOut.
// Returns count, or -1 on fault. Used to find the real native method that an
// exec-thunk calls. POD-only frame (no C++ objects) for SEH.
static int veincf_scan_calls(uint8_t* base, int len, int32_t* offsets, uintptr_t* targets, int maxOut)
{
    __try
    {
        int n = 0;
        for (int i = 0; i + 5 <= len && n < maxOut; ++i)
        {
            if (base[i] == 0xE8)
            {
                int32_t rel = *reinterpret_cast<int32_t*>(base + i + 1);
                offsets[n] = i;
                targets[n] = reinterpret_cast<uintptr_t>(base + i + 5) + static_cast<intptr_t>(rel);
                ++n;
            }
        }
        return n;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// VeinCF: SEH-safe single-instruction disassembly via Zydis. Decodes the
// instruction at `addr`, writes its length + formatted Intel text into a POD
// frame. Returns false on decode failure OR if the code bytes fault (e.g. we
// ran off into unmapped memory). No C++ objects in the frame (SEH requires it).
// This is the keystone RE primitive: read what a native function actually DOES.
struct VeincfDisasmOut { uint8_t length; bool ok; char text[112]; };
static bool veincf_disasm_one(uintptr_t addr, VeincfDisasmOut* out)
{
    out->ok = false; out->length = 0; out->text[0] = 0;
    __try
    {
        ZydisDisassembledInstruction insn;
        ZyanStatus s = ZydisDisassembleIntel(
            ZYDIS_MACHINE_MODE_LONG_64,
            (ZyanU64)addr,
            reinterpret_cast<const void*>(addr),
            16,
            &insn);
        if (!ZYAN_SUCCESS(s)) return false;
        out->length = (uint8_t)insn.info.length;
        // copy the formatted text (Zydis text buffer is small + null-terminated)
        size_t n = 0;
        while (insn.text[n] && n < sizeof(out->text) - 1) { out->text[n] = insn.text[n]; ++n; }
        out->text[n] = 0;
        out->ok = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out->ok = false;
        return false;
    }
}

// VeinCF: SEH-safe invoke of UAssetManager::ScanPathsForPrimaryAssets
//   int32 (this, FPrimaryAssetType byVal, const TArray<FString>& Paths, UClass* Base,
//          bool bHasBlueprintClasses, bool bIsEditorOnly, bool bForceSynchronousScan)
// POD frame (no C++ objects) so SEH is legal; the FName/FString/TArray are built
// by the caller and passed as raw value/pointers.
static int32_t veincf_call_scanpaths(void* fn, void* am, uint64_t typeVal, void* paths, void* baseClass)
{
    __try
    {
        typedef int32_t (*ScanFn)(void*, uint64_t, void*, void*, bool, bool, bool);
        return reinterpret_cast<ScanFn>(fn)(am, typeVal, paths, baseClass, false, false, true);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -999; }
}

// VeinCF: SEH-safe call of UAssetManager::TryUpdateCachedAssetData
//   bool (this, const FPrimaryAssetId&, const FAssetData&, bool bAllowDuplicates)
// The insert funnel BOTH ScanPathsForPrimaryAssets and RegisterSpecificPrimaryAsset
// reach (AssetManager.cpp). String-resolvable via "Tried to add primary asset...".
// Writes the asset into the per-type AssetMap -> what GetAllRecipes reads.
static bool veincf_call_inserter(uintptr_t fn, void* am, void* id, void* assetData, bool allowDup, bool* out)
{
    __try
    {
        typedef bool (*Fn)(void*, void*, void*, bool);
        *out = reinterpret_cast<Fn>(fn)(am, id, assetData, allowDup);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// VeinCF: SEH-safe call of an IAssetRegistry scan virtual (resolved via vtable):
//   void (this, const TArray<FString>& InPaths, bool bForceRescan, bool bIgnoreDenyList)
// The FRONT-DOOR loader verb: force-rescan + index a content path (e.g. a mounted mod
// pak's folder) so the AssetManager's scan then finds it. POD frame for SEH.
static bool veincf_call_scan_vtbl(uintptr_t fn, void* ar, void* paths, bool forceRescan, bool ignoreDeny)
{
    __try
    {
        typedef void (*Fn)(void*, void*, bool, bool);
        reinterpret_cast<Fn>(fn)(ar, paths, forceRescan, ignoreDeny);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// VeinCF: SEH-safe call of FAssetRegistryState::LoadFromDisk (static):
//   bool (const TCHAR* InPath, const FAssetRegistryLoadOptions& InOptions, FAssetRegistryState& OutState)
// Loads a cooked AssetRegistry.bin into OutState (the engine does all the FArchive +
// deserialization internally). The front-door content-merge half: load our pak's cooked
// registry, then AppendState it into the live registry so our asset becomes ON-DISK.
static bool veincf_call_loadfromdisk(uintptr_t fn, const wchar_t* path, void* options, void* state)
{
    __try
    {
        // 4th arg = FAssetRegistryVersion::Type* OutVersion (optional) -> pass nullptr so
        // LoadFromDisk skips the version write (it uses r9; a 3-arg call leaves r9 garbage -> fault).
        typedef bool (*Fn)(const wchar_t*, void*, void*, void*);
        return reinterpret_cast<Fn>(fn)(path, options, state, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// VeinCF: SEH-safe native function call (Microsoft x64 ABI, integer/pointer
// args only). Calls fn(a[0..argc-1]) and captures the return (rax). Covers the
// common case — `this` + const-ref struct args (which are just pointers) +
// ints/bools. This is the universal "call" verb: combined with DisasmFunction +
// ReadU64, it lets us invoke any engine function we can resolve an address for.
// POD-only frame for SEH (catches faults from a bad address / bad signature).
static bool veincf_call_native(uintptr_t fn, int argc, const uintptr_t* a, uintptr_t* out)
{
    __try
    {
        uintptr_t r;
        switch (argc)
        {
        case 0:  r = reinterpret_cast<uintptr_t(*)()>(fn)(); break;
        case 1:  r = reinterpret_cast<uintptr_t(*)(uintptr_t)>(fn)(a[0]); break;
        case 2:  r = reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t)>(fn)(a[0],a[1]); break;
        case 3:  r = reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t)>(fn)(a[0],a[1],a[2]); break;
        case 4:  r = reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(fn)(a[0],a[1],a[2],a[3]); break;
        case 5:  r = reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(fn)(a[0],a[1],a[2],a[3],a[4]); break;
        default: r = reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(fn)(a[0],a[1],a[2],a[3],a[4],a[5]); break;
        }
        *out = r;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// VeinCF: minimal GetModuleHandleW decl (avoid pulling all of Windows.h into
// this UE-macro-heavy TU). Guarded so it coexists if Windows.h is transitively
// included by a dependency.
#ifndef _WINDOWS_
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);
#endif

// VeinCF: locate the main game module + its .text section by parsing the PE
// header via raw offsets (no IMAGE_* structs -> no Windows macro clashes).
static bool veincf_get_main_module(uint8_t** outBase, size_t* outSize,
                                   uint8_t** outTextBase, size_t* outTextSize)
{
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    __try
    {
        if (*reinterpret_cast<uint16_t*>(base) != 0x5A4D) return false;        // 'MZ'
        int32_t e_lfanew = *reinterpret_cast<int32_t*>(base + 0x3C);
        uint8_t* nt = base + e_lfanew;
        if (*reinterpret_cast<uint32_t*>(nt) != 0x00004550) return false;       // 'PE\0\0'
        uint32_t sizeOfImage  = *reinterpret_cast<uint32_t*>(nt + 0x18 + 0x38); // OptionalHeader.SizeOfImage
        uint16_t numSections  = *reinterpret_cast<uint16_t*>(nt + 0x06);        // FileHeader.NumberOfSections
        uint16_t sizeOfOptHdr = *reinterpret_cast<uint16_t*>(nt + 0x14);        // FileHeader.SizeOfOptionalHeader
        uint8_t* secTable = nt + 0x18 + sizeOfOptHdr;
        *outBase = base; *outSize = sizeOfImage;
        *outTextBase = base; *outTextSize = sizeOfImage; // fallback: whole module
        for (uint16_t i = 0; i < numSections; ++i)
        {
            uint8_t* sh = secTable + (size_t)i * 0x28;
            if (sh[0]=='.' && sh[1]=='t' && sh[2]=='e' && sh[3]=='x' && sh[4]=='t')
            {
                *outTextBase = base + *reinterpret_cast<uint32_t*>(sh + 0x0C); // VirtualAddress
                *outTextSize = *reinterpret_cast<uint32_t*>(sh + 0x08);        // VirtualSize
                break;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// VeinCF: nearest preceding MSVC int3-padding (0xCC) before a code address ->
// the function start (the byte after the pad). Best-effort.
static uintptr_t veincf_find_func_start(uint8_t* ref, uint8_t* textBase, size_t maxBack)
{
    __try
    {
        for (size_t b = 1; b <= maxBack; ++b)
        {
            uint8_t* p = ref - b;
            if (p <= textBase) break;
            if (p[0] == 0xCC) return reinterpret_cast<uintptr_t>(p + 1);
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// VeinCF: find all CALL sites that target a known function address (E8 rel32). The
// by-ADDRESS xref verb -> resolves functions that have no usable string anchor (cracks
// the codeRefs=0 wide-string wall): given a resolvable callee, find its callers. Fills
// refs[] (call-site addrs) + funcs[] (their function starts). POD/SEH frame.
static int veincf_find_callers(uintptr_t targetAddr, uintptr_t* refs, uintptr_t* funcs, int maxOut)
{
    uint8_t *base, *textBase; size_t size, textSize;
    if (!veincf_get_main_module(&base, &size, &textBase, &textSize)) return -1;
    int n = 0;
    __try
    {
        for (size_t p = 0; p + 5 <= textSize && n < maxOut; ++p)
        {
            if (textBase[p] == 0xE8) // call rel32
            {
                int32_t rel = *reinterpret_cast<int32_t*>(textBase + p + 1);
                uintptr_t tgt = reinterpret_cast<uintptr_t>(textBase + p + 5) + static_cast<intptr_t>(rel);
                if (tgt == targetAddr)
                {
                    uint8_t* refAddr = textBase + p;
                    refs[n]  = reinterpret_cast<uintptr_t>(refAddr);
                    funcs[n] = veincf_find_func_start(refAddr, textBase, 0x4000);
                    ++n;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
}

// VeinCF: find code that references a string (ANSI or UTF-16) -> the universal
// "locate a function by a string it uses" verb. UE binaries are full of unique
// log/ensure strings; this resolves the function that contains them without
// symbols. Fills refs[] (rip-relative ref code addrs) + funcs[] (their function
// starts). POD/SEH frame. Returns ref count, *strHits = string occurrences.
static int veincf_find_string_refs(const char* s, uintptr_t* refs, uintptr_t* funcs,
                                   int maxOut, int* strHits)
{
    *strHits = 0;
    uint8_t *base, *textBase; size_t size, textSize;
    if (!veincf_get_main_module(&base, &size, &textBase, &textSize)) return -1;
    int n = 0;
    __try
    {
        size_t slen = strlen(s);
        if (slen == 0 || slen > 250) return 0;
        uintptr_t strAddrs[64]; int sc = 0;
        uint8_t c0 = (uint8_t)s[0];
        // ANSI occurrences (null-terminated)
        for (size_t i = 0; i + slen + 1 < size && sc < 64; ++i)
        {
            if (base[i] == c0 && memcmp(base + i, s, slen) == 0 && base[i + slen] == 0)
                strAddrs[sc++] = reinterpret_cast<uintptr_t>(base + i);
        }
        // UTF-16LE occurrences (UE TEXT() strings)
        for (size_t i = 0; i + slen * 2 + 2 < size && sc < 64; ++i)
        {
            if (base[i] != c0 || base[i + 1] != 0) continue;
            bool ok = true;
            for (size_t j = 0; j < slen; ++j)
                if (base[i + j*2] != (uint8_t)s[j] || base[i + j*2 + 1] != 0) { ok = false; break; }
            if (ok && base[i + slen*2] == 0 && base[i + slen*2 + 1] == 0)
                strAddrs[sc++] = reinterpret_cast<uintptr_t>(base + i);
        }
        *strHits = sc;
        if (sc == 0) return 0;
        // scan .text for rip-relative disp32 fields that resolve to a string addr
        for (size_t p = 0; p + 4 <= textSize && n < maxOut; ++p)
        {
            int32_t disp = *reinterpret_cast<int32_t*>(textBase + p);
            uintptr_t target = reinterpret_cast<uintptr_t>(textBase + p + 4) + (intptr_t)disp;
            for (int k = 0; k < sc; ++k)
            {
                if (target == strAddrs[k])
                {
                    uint8_t* refAddr = textBase + p;
                    refs[n]  = reinterpret_cast<uintptr_t>(refAddr);
                    funcs[n] = veincf_find_func_start(refAddr, textBase, 0x4000);
                    ++n;
                    break;
                }
            }
        }
        // ALSO scan for the string address embedded as a 64-bit IMMEDIATE
        // (mov r64, imm64) -- optimized builds load string addrs this way instead
        // of rip-relative lea, which is why pure-disp32 scans returned 0 refs.
        for (size_t p = 0; p + 8 <= textSize && n < maxOut; ++p)
        {
            uintptr_t imm = *reinterpret_cast<uintptr_t*>(textBase + p);
            for (int k = 0; k < sc; ++k)
            {
                if (imm == strAddrs[k])
                {
                    uint8_t* refAddr = textBase + p;
                    refs[n]  = reinterpret_cast<uintptr_t>(refAddr);
                    funcs[n] = veincf_find_func_start(refAddr, textBase, 0x4000);
                    ++n;
                    break;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
}

// VeinCF: SEH-safe copy of a string (ANSI or UTF-16) at an address into an ascii
// buffer. Used to read string constants we located in the module.
static bool veincf_copy_str(uint8_t* addr, int len, bool wide, char* out, int outSize)
{
    __try
    {
        int n = 0;
        for (int k = 0; k < len && n < outSize - 1; ++k)
            out[n++] = wide ? (char)addr[(size_t)k * 2] : (char)addr[k];
        out[n] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

// VeinCF: find all string constants (ANSI + UTF-16) in the main module that
// CONTAIN a substring, expanded to the full enclosing string. Reads the real
// strings the binary uses instead of guessing them -> pick a true anchor for
// FindStringRefs. Fills out[] with {addr,len,wide}. SEH/POD frame.
struct VeincfStrHit { uintptr_t addr; uint16_t len; uint8_t wide; };
static int veincf_strings_containing(const char* sub, VeincfStrHit* out, int maxOut)
{
    uint8_t *base, *tb; size_t size, ts;
    if (!veincf_get_main_module(&base, &size, &tb, &ts)) return -1;
    int n = 0;
    __try
    {
        size_t sl = strlen(sub);
        if (sl == 0 || sl > 120) return 0;
        uint8_t c0 = (uint8_t)sub[0];
        // ANSI
        for (size_t i = 0; i + sl < size && n < maxOut; ++i)
        {
            if (base[i] != c0 || memcmp(base + i, sub, sl) != 0) continue;
            size_t s = i; while (s > 0 && base[s-1] >= 0x20 && base[s-1] < 0x7F) --s;
            size_t e = i + sl; while (e < size && base[e] >= 0x20 && base[e] < 0x7F) ++e;
            out[n].addr = reinterpret_cast<uintptr_t>(base + s);
            out[n].len = (uint16_t)((e - s) > 250 ? 250 : (e - s)); out[n].wide = 0; ++n;
            i = e;
        }
        // UTF-16LE
        for (size_t i = 0; i + sl * 2 < size && n < maxOut; ++i)
        {
            if (base[i] != c0 || base[i+1] != 0) continue;
            bool ok = true;
            for (size_t j = 0; j < sl; ++j)
                if (base[i + j*2] != (uint8_t)sub[j] || base[i + j*2 + 1] != 0) { ok = false; break; }
            if (!ok) continue;
            size_t s = i; while (s >= 2 && base[s-2] >= 0x20 && base[s-2] < 0x7F && base[s-1] == 0) s -= 2;
            size_t e = i + sl*2; while (e + 1 < size && base[e] >= 0x20 && base[e] < 0x7F && base[e+1] == 0) e += 2;
            out[n].addr = reinterpret_cast<uintptr_t>(base + s);
            out[n].len = (uint16_t)(((e - s) / 2) > 250 ? 250 : ((e - s) / 2)); out[n].wide = 1; ++n;
            i = e;
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
}

// SEH-safe: fetch an object's UClass (used while sweeping all of GUObjectArray).
static Unreal::UClass* veincf_safe_getclass(Unreal::UObject* o)
{
    __try
    {
        return o ? o->GetClassPrivate() : nullptr;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// SEH-safe: treat arrPtr as a UE TArray { void* Data; int32 Num; int32 Max } and
// check whether any 8-byte element equals target. Used to locate which array
// property holds a given object (e.g. the cached recipe list). Writes element
// count to *outCount (-1 on fault). No C++ objects in this frame (POD only).
static bool veincf_raw_array_contains(void* arrPtr, Unreal::UObject* target, int32_t* outCount)
{
    __try
    {
        struct RawArr { void* Data; int32_t Num; int32_t Max; };
        RawArr* a = reinterpret_cast<RawArr*>(arrPtr);
        int32_t n = a->Num;
        *outCount = n;
        if (n <= 0 || n > 2000000 || a->Data == nullptr) return false;
        void** data = reinterpret_cast<void**>(a->Data);
        void* t = reinterpret_cast<void*>(target);
        for (int32_t k = 0; k < n; ++k)
        {
            if (data[k] == t) return true;
        }
        return false;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        *outCount = -1;
        return false;
    }
}

// SEH-safe: same as above but for a TArray<FWeakObjectPtr> (each element is
// {int32 index, int32 serial} = 8 bytes). Resolves each weak handle and compares
// to target. This catches the case the strong scan misses (recipe caches are
// commonly weak-ptr arrays). Writes element count to *outCount (-1 on fault).
static bool veincf_raw_weakarray_contains(void* arrPtr, Unreal::UObject* target, int32_t* outCount)
{
    __try
    {
        struct RawArr { void* Data; int32_t Num; int32_t Max; };
        RawArr* a = reinterpret_cast<RawArr*>(arrPtr);
        int32_t n = a->Num;
        *outCount = n;
        if (n <= 0 || n > 2000000 || a->Data == nullptr) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(a->Data);
        for (int32_t k = 0; k < n; ++k)
        {
            Unreal::UObject* resolved = veincf_resolve_weak(base + static_cast<size_t>(k) * 8);
            if (resolved == target) return true;
        }
        return false;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        *outCount = -1;
        return false;
    }
}


#pragma optimize("", on)

// ── VeinCF: binary detour on UVeinAssetManager::GetAllRecipes ───────────────
// The workbench calls GetAllRecipes natively (bypassing the UFunction/ProcessEvent
// layer), so we hook the real C++ method and append our recipe(s) to every return.
// GetAllRecipes returns TArray<UBaseRecipe*> by value -> MSVC x64 ABI passes a hidden
// return-storage ptr in RCX, this in RDX, bSort in R8, and returns the storage in RAX.
static std::vector<Unreal::UObject*> g_veincf_injected_recipes;
static Unreal::FArrayProperty* g_veincf_recipes_retprop = nullptr;
static uint64_t g_veincf_getallrecipes_tramp = 0;
static PLH::x64Detour* g_veincf_getallrecipes_detour = nullptr;
static volatile long g_veincf_getallrecipes_fires = 0;       // total detour fires
static volatile long g_veincf_getallrecipes_lastappended = 0; // entries appended on last fire

typedef void* (*veincf_getallrecipes_t)(void* retStorage, void* self, bool bSort);

static void* veincf_getallrecipes_hook(void* retStorage, void* self, bool bSort)
{
    auto orig = reinterpret_cast<veincf_getallrecipes_t>(g_veincf_getallrecipes_tramp);
    void* r = orig(retStorage, self, bSort); // fills retStorage with the base-game recipes
    g_veincf_getallrecipes_fires++;
    long appended = 0;
    __try
    {
        if (r && g_veincf_recipes_retprop && !g_veincf_injected_recipes.empty())
        {
            Unreal::FScriptArrayHelper helper(g_veincf_recipes_retprop, r);
            for (auto* rec : g_veincf_injected_recipes)
            {
                if (!rec) continue;
                int32_t idx = helper.AddValue();
                *reinterpret_cast<Unreal::UObject**>(helper.GetRawPtr(idx)) = rec;
                ++appended;
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    g_veincf_getallrecipes_lastappended = appended;
    return r;
}

namespace RC
{
    LuaMadeSimple::Lua* LuaStatics::console_executor{};
    bool LuaStatics::console_executor_enabled{};

    auto get_mod_ref(const LuaMadeSimple::Lua& lua) -> LuaMod*
    {
        if (lua_getglobal(lua.get_lua_state(), "ModRef") == LUA_TNIL)
        {
            lua.throw_error("[get_mod_ref] Tried retrieving 'ModRef' global variable but it was nil, please do not override this global");
        }

        // Explicitly using the top of the stack (-1) since that's where 'getglobal' puts stuff
        auto& lua_object = lua.get_userdata<LuaType::LuaModRef>(-1);
        return lua_object.get_remote_cpp_object();
    }

    static auto get_function_name_without_prefix(const StringType& function_full_name) -> StringType
    {
        static constexpr StringViewType function_prefix{STR("Function ")};
        if (auto prefix_pos = function_full_name.find(function_prefix); prefix_pos != function_full_name.npos)
        {
            return function_full_name.substr(prefix_pos + function_prefix.size());
        }
        else
        {
            return function_full_name;
        }
    }

    struct LuaUnrealScriptFunctionData
    {
        Unreal::CallbackId pre_callback_id;
        Unreal::CallbackId post_callback_id;
        Unreal::UFunction* unreal_function;
        const Mod* mod;
        const LuaMadeSimple::Lua& lua;
        const int lua_callback_ref;
        const int lua_post_callback_ref;
        const int lua_thread_ref;

        bool has_return_value{};
        // Will be non-nullptr if the UFunction has a return value
        Unreal::FProperty* return_property{};
        std::atomic<bool> scheduled_for_removal{false};

        LuaUnrealScriptFunctionData(Unreal::CallbackId pre_id,
                                    Unreal::CallbackId post_id,
                                    Unreal::UFunction* func,
                                    const Mod* m,
                                    const LuaMadeSimple::Lua& l,
                                    int cb_ref,
                                    int post_cb_ref,
                                    int thread_ref)
            : pre_callback_id(pre_id), post_callback_id(post_id), unreal_function(func), mod(m), lua(l),
              lua_callback_ref(cb_ref), lua_post_callback_ref(post_cb_ref), lua_thread_ref(thread_ref)
        {
        }
    };
    static std::vector<std::unique_ptr<LuaUnrealScriptFunctionData>> g_hooked_script_function_data{};

    static auto lua_unreal_script_function_hook_pre(Unreal::UnrealScriptFunctionCallableContext context, void* custom_data) -> void
    {
        // Fetch the data corresponding to this UFunction
        auto& lua_data = *static_cast<LuaUnrealScriptFunctionData*>(custom_data);

        // Check if this hook has been scheduled for removal (Lua state may be invalid)
        if (lua_data.scheduled_for_removal) return;

        // Use the stored registry index to put a Lua function on the Lua stack
        // This is the function that was provided by the Lua call to "RegisterHook"
        lua_data.lua.registry().get_function_ref(lua_data.lua_callback_ref);

        // Set up the first param (context / this-ptr)
        // TODO: Check what happens if a static UFunction is hooked since they don't have any context
        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
        LuaType::RemoteUnrealParam::construct(lua_data.lua, &context.Context, s_object_property_name);

        // Attempt at dynamically fetching the params
        const auto FunctionBeingExecuted = lua_data.unreal_function;
        uint16_t return_value_offset = FunctionBeingExecuted->GetReturnValueOffset();

        // 'ReturnValueOffset' is 0xFFFF if the UFunction return type is void
        lua_data.has_return_value = return_value_offset != 0xFFFF;

        uint8_t num_unreal_params = FunctionBeingExecuted->GetNumParms();
        if (lua_data.has_return_value)
        {
            // Subtract one from the number of params if there's a return value
            // This is because Unreal treats the return value as a param, and it's included in the 'NumParms' member variable
            --num_unreal_params;
        }

        bool has_properties_to_process = lua_data.has_return_value || num_unreal_params > 0;
        if (has_properties_to_process && (context.TheStack.Locals() || context.TheStack.OutParms()))
        {
            // int32_t current_param_offset{};

            for (Unreal::FProperty* func_prop : Unreal::TFieldRange<Unreal::FProperty>(FunctionBeingExecuted, Unreal::EFieldIterationFlags::IncludeDeprecated))
            {
                // Skip this property if it's not a parameter
                if (!func_prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_Parm))
                {
                    continue;
                }

                // Skip if this property corresponds to the return value
                if (lua_data.has_return_value && func_prop->GetOffset_Internal() == return_value_offset)
                {
                    lua_data.return_property = func_prop;
                    continue;
                }

                Unreal::FName property_type = func_prop->GetClass().GetFName();
                int32_t name_comparison_index = property_type.GetComparisonIndex();

                if (LuaType::StaticState::m_property_value_pushers.contains(name_comparison_index))
                {
                    // Non-typed pointer to the current parameter value
                    void* data{};
                    if (func_prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_OutParm))
                    {
                        data = Unreal::FindOutParamValueAddress(context.TheStack, func_prop);
                    }
                    else
                    {
                        data = func_prop->ContainerPtrToValuePtr<void>(context.TheStack.Locals());
                    }

                    // Keeping track of where in the 'Locals' array the next property is
                    // current_param_offset += func_prop->GetSize();

                    // Set up a call to a handler for this type of Unreal property (the param)
                    // The FName is being used as a key for an unordered_map which has the types & corresponding handlers filled right after the dll is injected
                    const LuaType::PusherParams pusher_params{.operation = LuaType::Operation::GetParam,
                                                              .lua = lua_data.lua,
                                                              .base = nullptr,
                                                              .data = data,
                                                              .property = func_prop};
                    LuaType::StaticState::m_property_value_pushers[name_comparison_index](pusher_params);
                }
                else
                {
                    lua_data.lua.throw_error(fmt::format(
                            "[unreal_script_function_hook] Tried accessing unreal property without a registered handler. Property type '{}' not supported.",
                            to_string(property_type.ToString())));
                }
            }
        }

        // Call the Lua function with the correct number of parameters & return values
        // Increasing the 'num_params' by one to account for the 'this / context' param
        lua_data.lua.call_function(num_unreal_params + 1, 1);

        // The params for the Lua script will be 'userdata' and they will have get/set functions
        // Use these functions in the Lua script to access & mutate the parameter values

        // After the Lua function has been executed you should call the original function
        // This will execute any internal UE4 scripting functions & native functions depending on the type of UFunction
        // The API will automatically call the original function
        // This function continues in 'lua_unreal_script_function_hook_post' which executes immediately after the original function gets called
    }

    static auto lua_unreal_script_function_hook_post(Unreal::UnrealScriptFunctionCallableContext context, void* custom_data) -> void
    {
        // Fetch the data corresponding to this UFunction
        auto& lua_data = *static_cast<LuaUnrealScriptFunctionData*>(custom_data);

        // Returns true if a hooks were removed.
        auto remove_if_scheduled = [&] -> bool {
            if (lua_data.scheduled_for_removal)
            {
                const auto function_name_no_prefix = get_function_name_without_prefix(lua_data.unreal_function->GetFullName());

                Output::send<LogLevel::Verbose>(STR("Unregistering native pre-hook ({}) for {}\n"), lua_data.pre_callback_id, function_name_no_prefix);
                lua_data.unreal_function->UnregisterHook(lua_data.pre_callback_id);
                luaL_unref(lua_data.lua.get_lua_state(), LUA_REGISTRYINDEX, lua_data.lua_callback_ref);

                Output::send<LogLevel::Verbose>(STR("Unregistering native post-hook ({}) for {}\n"), lua_data.post_callback_id, function_name_no_prefix);
                lua_data.unreal_function->UnregisterHook(lua_data.post_callback_id);
                if (lua_data.lua_post_callback_ref != -1)
                {
                    luaL_unref(lua_data.lua.get_lua_state(), LUA_REGISTRYINDEX, lua_data.lua_post_callback_ref);
                }

                const auto mod = get_mod_ref(lua_data.lua);
                luaL_unref(mod->lua().get_lua_state(), LUA_REGISTRYINDEX, lua_data.lua_thread_ref);
                std::erase_if(g_hooked_script_function_data, [&](const std::unique_ptr<LuaUnrealScriptFunctionData>& elem) {
                    return elem.get() == &lua_data;
                });

                return true;
            }
            else
            {
                return false;
            }
        };

        // Removes pre & post-hook callbacks if UnregisterHook was called in the pre-callback.
        if (remove_if_scheduled())
        {
            return;
        }

        auto process_return_value = [&]() {
            // If 'nil' exists on the Lua stack, that means that the UFunction expected a return value but the Lua script didn't return anything
            // So we can simply clean the stack and let the UFunction decide the return value on its own
            if (lua_data.lua.is_nil())
            {
                lua_data.lua.discard_value();
            }
            else if (lua_data.lua.get_stack_size() > 0 && lua_data.has_return_value && lua_data.return_property && context.RESULT_DECL)
            {
                // Fetch the return value from Lua if the UFunction expects one
                // If no return value exists then assume that the Lua script didn't want to override the original
                // Keep in mind that the if this was a Blueprint UFunction then the entire byte-code will already have executed
                // That means that changing the return value here won't affect the script itself
                // If this was a native UFunction then changing the return value here will have the desired effect

                Unreal::FName property_type_name = lua_data.return_property->GetClass().GetFName();
                int32_t name_comparison_index = property_type_name.GetComparisonIndex();

                if (LuaType::StaticState::m_property_value_pushers.contains(name_comparison_index))
                {
                    const LuaType::PusherParams pusher_params{.operation = LuaType::Operation::Set,
                                                              .lua = lua_data.lua,
                                                              .base = static_cast<Unreal::UObject*>(context.RESULT_DECL),
                                                              .data = context.RESULT_DECL,
                                                              .property = lua_data.return_property};
                    LuaType::StaticState::m_property_value_pushers[name_comparison_index](pusher_params);
                }
                else
                {
                    // If the type wasn't supported then we simply clean the Lua stack, output a warning and then do nothing
                    lua_data.lua.discard_value();

                    auto parameter_type_name = property_type_name.ToString();
                    auto parameter_name = lua_data.return_property->GetName();

                    Output::send(
                            STR("Tried altering return value of a hooked UFunction without a registered handler for return type Return property '{}' of type "
                                "'{}' not supported."),
                            parameter_name,
                            parameter_type_name);
                }
            }
        };

        if (lua_data.lua_post_callback_ref != -1)
        {
            // Use the stored registry index to put a Lua function on the Lua stack
            // This is the function that was provided by the Lua call to "RegisterHook"
            lua_data.lua.registry().get_function_ref(lua_data.lua_post_callback_ref);

            // Set up the first param (context / this-ptr)
            static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
            LuaType::RemoteUnrealParam::construct(lua_data.lua, &context.Context, s_object_property_name);

            // Attempt at dynamically fetching the params
            const auto FunctionBeingExecuted = lua_data.unreal_function;
            uint16_t return_value_offset = FunctionBeingExecuted->GetReturnValueOffset();

            // 'ReturnValueOffset' is 0xFFFF if the UFunction return type is void
            lua_data.has_return_value = return_value_offset != 0xFFFF;

            uint8_t num_unreal_params = FunctionBeingExecuted->GetNumParms();
            if (lua_data.has_return_value)
            {
                // Subtract one from the number of params if there's a return value
                // This is because Unreal treats the return value as a param, and it's included in the 'NumParms' member variable
                --num_unreal_params;

                // Set up the return value param so that Lua can access the original return value
                auto return_property = FunctionBeingExecuted->GetReturnProperty();
                auto return_property_type = return_property->GetClass().GetFName();
                int32_t name_comparison_index = return_property_type.GetComparisonIndex();
                if (LuaType::StaticState::m_property_value_pushers.contains(name_comparison_index))
                {
                    const LuaType::PusherParams pusher_params{.operation = LuaType::Operation::GetParam,
                                                              .lua = lua_data.lua,
                                                              .base = nullptr,
                                                              .data = context.RESULT_DECL,
                                                              .property = return_property};
                    LuaType::StaticState::m_property_value_pushers[name_comparison_index](pusher_params);
                }
            }

            bool has_properties_to_process = lua_data.has_return_value || num_unreal_params > 0;
            if (has_properties_to_process && context.TheStack.Locals())
            {
                for (Unreal::FProperty* func_prop : Unreal::TFieldRange<Unreal::FProperty>(FunctionBeingExecuted, Unreal::EFieldIterationFlags::IncludeDeprecated))
                {
                    // Skip this property if it's not a parameter
                    if (!func_prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_Parm))
                    {
                        continue;
                    }

                    // Skip if this property corresponds to the return value
                    if (lua_data.has_return_value && func_prop->GetOffset_Internal() == return_value_offset)
                    {
                        lua_data.return_property = func_prop;
                        continue;
                    }

                    Unreal::FName property_type = func_prop->GetClass().GetFName();
                    int32_t name_comparison_index = property_type.GetComparisonIndex();

                    if (LuaType::StaticState::m_property_value_pushers.contains(name_comparison_index))
                    {
                        // Non-typed pointer to the current parameter value
                        void* data{};
                        if (func_prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_OutParm))
                        {
                            // For out params (including ref params), get the modified value from OutParms
                            data = Unreal::FindOutParamValueAddress(context.TheStack, func_prop);
                        }
                        else
                        {
                            // For regular input params, read from Locals
                            data = func_prop->ContainerPtrToValuePtr<void>(context.TheStack.Locals());
                        }

                        // Keeping track of where in the 'Locals' array the next property is
                        // current_param_offset += func_prop->GetSize();

                        // Set up a call to a handler for this type of Unreal property (the param)
                        // The FName is being used as a key for an unordered_map which has the types & corresponding handlers filled right after the dll is injected
                        const LuaType::PusherParams pusher_params{.operation = LuaType::Operation::GetParam,
                                                                  .lua = lua_data.lua,
                                                                  .base = nullptr,
                                                                  .data = data,
                                                                  .property = func_prop};
                        LuaType::StaticState::m_property_value_pushers[name_comparison_index](pusher_params);
                    }
                    else
                    {
                        lua_data.lua.throw_error(fmt::format(
                                "[unreal_script_function_hook] Tried accessing unreal property without a registered handler. Property type '{}' not supported.",
                                to_string(property_type.ToString())));
                    }
                }
            }

            // Call the Lua function with the correct number of parameters & return values
            // Increasing the 'num_params' by one to account for the 'this / context' param
            // Increasing it again if there's a return value because we store that as the second param
            lua_data.lua.call_function(num_unreal_params + (lua_data.has_return_value ? 2 : 1), 1);
        }

        // Processing potential return values from both callbacks.
        // Stack pos 1: return value from callback 1 (nil if nothing returned)
        // Stack pos 2: return value from callback 2
        // We will always have at leaste two return values, either one can be nil, and we need to process both in case one isn't nil.
        process_return_value();
        process_return_value();

        // Removes pre & post-hook callbacks if UnregisterHook was called in the post-hook callback.
        remove_if_scheduled();
    }

    static auto register_input_globals(const LuaMadeSimple::Lua& lua) -> void
    {
        LuaMadeSimple::Lua::Table key_table = lua.prepare_new_table();
        key_table.add_pair("LEFT_MOUSE_BUTTON", static_cast<uint32_t>(Input::Key::LEFT_MOUSE_BUTTON));
        key_table.add_pair("RIGHT_MOUSE_BUTTON", static_cast<uint32_t>(Input::Key::RIGHT_MOUSE_BUTTON));
        key_table.add_pair("CANCEL", static_cast<uint32_t>(Input::Key::CANCEL));
        key_table.add_pair("MIDDLE_MOUSE_BUTTON", static_cast<uint32_t>(Input::Key::MIDDLE_MOUSE_BUTTON));
        key_table.add_pair("XBUTTON_ONE", static_cast<uint32_t>(Input::Key::XBUTTON_ONE));
        key_table.add_pair("XBUTTON_TWO", static_cast<uint32_t>(Input::Key::XBUTTON_TWO));
        key_table.add_pair("BACKSPACE", static_cast<uint32_t>(Input::Key::BACKSPACE));
        key_table.add_pair("TAB", static_cast<uint32_t>(Input::Key::TAB));
        key_table.add_pair("CLEAR", static_cast<uint32_t>(Input::Key::CLEAR));
        key_table.add_pair("RETURN", static_cast<uint32_t>(Input::Key::RETURN));
        key_table.add_pair("PAUSE", static_cast<uint32_t>(Input::Key::PAUSE));
        key_table.add_pair("CAPS_LOCK", static_cast<uint32_t>(Input::Key::CAPS_LOCK));
        key_table.add_pair("IME_KANA", static_cast<uint32_t>(Input::Key::IME_KANA));
        key_table.add_pair("IME_HANGUEL", static_cast<uint32_t>(Input::Key::IME_HANGUEL));
        key_table.add_pair("IME_HANGUL", static_cast<uint32_t>(Input::Key::IME_HANGUL));
        key_table.add_pair("IME_ON", static_cast<uint32_t>(Input::Key::IME_ON));
        key_table.add_pair("IME_JUNJA", static_cast<uint32_t>(Input::Key::IME_JUNJA));
        key_table.add_pair("IME_FINAL", static_cast<uint32_t>(Input::Key::IME_FINAL));
        key_table.add_pair("IME_HANJA", static_cast<uint32_t>(Input::Key::IME_HANJA));
        key_table.add_pair("IME_KANJI", static_cast<uint32_t>(Input::Key::IME_KANJI));
        key_table.add_pair("IME_OFF", static_cast<uint32_t>(Input::Key::IME_OFF));
        key_table.add_pair("ESCAPE", static_cast<uint32_t>(Input::Key::ESCAPE));
        key_table.add_pair("IME_CONVERT", static_cast<uint32_t>(Input::Key::IME_CONVERT));
        key_table.add_pair("IME_NONCONVERT", static_cast<uint32_t>(Input::Key::IME_NONCONVERT));
        key_table.add_pair("IME_ACCEPT", static_cast<uint32_t>(Input::Key::IME_ACCEPT));
        key_table.add_pair("IME_MODECHANGE", static_cast<uint32_t>(Input::Key::IME_MODECHANGE));
        key_table.add_pair("SPACE", static_cast<uint32_t>(Input::Key::SPACE));
        key_table.add_pair("PAGE_UP", static_cast<uint32_t>(Input::Key::PAGE_UP));
        key_table.add_pair("PAGE_DOWN", static_cast<uint32_t>(Input::Key::PAGE_DOWN));
        key_table.add_pair("END", static_cast<uint32_t>(Input::Key::END));
        key_table.add_pair("HOME", static_cast<uint32_t>(Input::Key::HOME));
        key_table.add_pair("LEFT_ARROW", static_cast<uint32_t>(Input::Key::LEFT_ARROW));
        key_table.add_pair("UP_ARROW", static_cast<uint32_t>(Input::Key::UP_ARROW));
        key_table.add_pair("RIGHT_ARROW", static_cast<uint32_t>(Input::Key::RIGHT_ARROW));
        key_table.add_pair("DOWN_ARROW", static_cast<uint32_t>(Input::Key::DOWN_ARROW));
        key_table.add_pair("SELECT", static_cast<uint32_t>(Input::Key::SELECT));
        key_table.add_pair("PRINT", static_cast<uint32_t>(Input::Key::PRINT));
        key_table.add_pair("EXECUTE", static_cast<uint32_t>(Input::Key::EXECUTE));
        key_table.add_pair("PRINT_SCREEN", static_cast<uint32_t>(Input::Key::PRINT_SCREEN));
        key_table.add_pair("INS", static_cast<uint32_t>(Input::Key::INS));
        key_table.add_pair("DEL", static_cast<uint32_t>(Input::Key::DEL));
        key_table.add_pair("HELP", static_cast<uint32_t>(Input::Key::HELP));
        key_table.add_pair("ZERO", static_cast<uint32_t>(Input::Key::ZERO));
        key_table.add_pair("ONE", static_cast<uint32_t>(Input::Key::ONE));
        key_table.add_pair("TWO", static_cast<uint32_t>(Input::Key::TWO));
        key_table.add_pair("THREE", static_cast<uint32_t>(Input::Key::THREE));
        key_table.add_pair("FOUR", static_cast<uint32_t>(Input::Key::FOUR));
        key_table.add_pair("FIVE", static_cast<uint32_t>(Input::Key::FIVE));
        key_table.add_pair("SIX", static_cast<uint32_t>(Input::Key::SIX));
        key_table.add_pair("SEVEN", static_cast<uint32_t>(Input::Key::SEVEN));
        key_table.add_pair("EIGHT", static_cast<uint32_t>(Input::Key::EIGHT));
        key_table.add_pair("NINE", static_cast<uint32_t>(Input::Key::NINE));
        key_table.add_pair("A", static_cast<uint32_t>(Input::Key::A));
        key_table.add_pair("B", static_cast<uint32_t>(Input::Key::B));
        key_table.add_pair("C", static_cast<uint32_t>(Input::Key::C));
        key_table.add_pair("D", static_cast<uint32_t>(Input::Key::D));
        key_table.add_pair("E", static_cast<uint32_t>(Input::Key::E));
        key_table.add_pair("F", static_cast<uint32_t>(Input::Key::F));
        key_table.add_pair("G", static_cast<uint32_t>(Input::Key::G));
        key_table.add_pair("H", static_cast<uint32_t>(Input::Key::H));
        key_table.add_pair("I", static_cast<uint32_t>(Input::Key::I));
        key_table.add_pair("J", static_cast<uint32_t>(Input::Key::J));
        key_table.add_pair("K", static_cast<uint32_t>(Input::Key::K));
        key_table.add_pair("L", static_cast<uint32_t>(Input::Key::L));
        key_table.add_pair("M", static_cast<uint32_t>(Input::Key::M));
        key_table.add_pair("N", static_cast<uint32_t>(Input::Key::N));
        key_table.add_pair("O", static_cast<uint32_t>(Input::Key::O));
        key_table.add_pair("P", static_cast<uint32_t>(Input::Key::P));
        key_table.add_pair("Q", static_cast<uint32_t>(Input::Key::Q));
        key_table.add_pair("R", static_cast<uint32_t>(Input::Key::R));
        key_table.add_pair("S", static_cast<uint32_t>(Input::Key::S));
        key_table.add_pair("T", static_cast<uint32_t>(Input::Key::T));
        key_table.add_pair("U", static_cast<uint32_t>(Input::Key::U));
        key_table.add_pair("V", static_cast<uint32_t>(Input::Key::V));
        key_table.add_pair("W", static_cast<uint32_t>(Input::Key::W));
        key_table.add_pair("X", static_cast<uint32_t>(Input::Key::X));
        key_table.add_pair("Y", static_cast<uint32_t>(Input::Key::Y));
        key_table.add_pair("Z", static_cast<uint32_t>(Input::Key::Z));
        key_table.add_pair("LEFT_WIN", static_cast<uint32_t>(Input::Key::LEFT_WIN));
        key_table.add_pair("RIGHT_WIN", static_cast<uint32_t>(Input::Key::RIGHT_WIN));
        key_table.add_pair("APPS", static_cast<uint32_t>(Input::Key::APPS));
        key_table.add_pair("SLEEP", static_cast<uint32_t>(Input::Key::SLEEP));
        key_table.add_pair("NUM_ZERO", static_cast<uint32_t>(Input::Key::NUM_ZERO));
        key_table.add_pair("NUM_ONE", static_cast<uint32_t>(Input::Key::NUM_ONE));
        key_table.add_pair("NUM_TWO", static_cast<uint32_t>(Input::Key::NUM_TWO));
        key_table.add_pair("NUM_THREE", static_cast<uint32_t>(Input::Key::NUM_THREE));
        key_table.add_pair("NUM_FOUR", static_cast<uint32_t>(Input::Key::NUM_FOUR));
        key_table.add_pair("NUM_FIVE", static_cast<uint32_t>(Input::Key::NUM_FIVE));
        key_table.add_pair("NUM_SIX", static_cast<uint32_t>(Input::Key::NUM_SIX));
        key_table.add_pair("NUM_SEVEN", static_cast<uint32_t>(Input::Key::NUM_SEVEN));
        key_table.add_pair("NUM_EIGHT", static_cast<uint32_t>(Input::Key::NUM_EIGHT));
        key_table.add_pair("NUM_NINE", static_cast<uint32_t>(Input::Key::NUM_NINE));
        key_table.add_pair("MULTIPLY", static_cast<uint32_t>(Input::Key::MULTIPLY));
        key_table.add_pair("ADD", static_cast<uint32_t>(Input::Key::ADD));
        key_table.add_pair("SEPARATOR", static_cast<uint32_t>(Input::Key::SEPARATOR));
        key_table.add_pair("SUBTRACT", static_cast<uint32_t>(Input::Key::SUBTRACT));
        key_table.add_pair("DECIMAL", static_cast<uint32_t>(Input::Key::DECIMAL));
        key_table.add_pair("DIVIDE", static_cast<uint32_t>(Input::Key::DIVIDE));
        key_table.add_pair("F1", static_cast<uint32_t>(Input::Key::F1));
        key_table.add_pair("F2", static_cast<uint32_t>(Input::Key::F2));
        key_table.add_pair("F3", static_cast<uint32_t>(Input::Key::F3));
        key_table.add_pair("F4", static_cast<uint32_t>(Input::Key::F4));
        key_table.add_pair("F5", static_cast<uint32_t>(Input::Key::F5));
        key_table.add_pair("F6", static_cast<uint32_t>(Input::Key::F6));
        key_table.add_pair("F7", static_cast<uint32_t>(Input::Key::F7));
        key_table.add_pair("F8", static_cast<uint32_t>(Input::Key::F8));
        key_table.add_pair("F9", static_cast<uint32_t>(Input::Key::F9));
        key_table.add_pair("F10", static_cast<uint32_t>(Input::Key::F10));
        key_table.add_pair("F11", static_cast<uint32_t>(Input::Key::F11));
        key_table.add_pair("F12", static_cast<uint32_t>(Input::Key::F12));
        key_table.add_pair("F13", static_cast<uint32_t>(Input::Key::F13));
        key_table.add_pair("F14", static_cast<uint32_t>(Input::Key::F14));
        key_table.add_pair("F15", static_cast<uint32_t>(Input::Key::F15));
        key_table.add_pair("F16", static_cast<uint32_t>(Input::Key::F16));
        key_table.add_pair("F17", static_cast<uint32_t>(Input::Key::F17));
        key_table.add_pair("F18", static_cast<uint32_t>(Input::Key::F18));
        key_table.add_pair("F19", static_cast<uint32_t>(Input::Key::F19));
        key_table.add_pair("F20", static_cast<uint32_t>(Input::Key::F20));
        key_table.add_pair("F21", static_cast<uint32_t>(Input::Key::F21));
        key_table.add_pair("F22", static_cast<uint32_t>(Input::Key::F22));
        key_table.add_pair("F23", static_cast<uint32_t>(Input::Key::F23));
        key_table.add_pair("F24", static_cast<uint32_t>(Input::Key::F24));
        key_table.add_pair("NUM_LOCK", static_cast<uint32_t>(Input::Key::NUM_LOCK));
        key_table.add_pair("SCROLL_LOCK", static_cast<uint32_t>(Input::Key::SCROLL_LOCK));
        key_table.add_pair("BROWSER_BACK", static_cast<uint32_t>(Input::Key::BROWSER_BACK));
        key_table.add_pair("BROWSER_FORWARD", static_cast<uint32_t>(Input::Key::BROWSER_FORWARD));
        key_table.add_pair("BROWSER_REFRESH", static_cast<uint32_t>(Input::Key::BROWSER_REFRESH));
        key_table.add_pair("BROWSER_STOP", static_cast<uint32_t>(Input::Key::BROWSER_STOP));
        key_table.add_pair("BROWSER_SEARCH", static_cast<uint32_t>(Input::Key::BROWSER_SEARCH));
        key_table.add_pair("BROWSER_FAVORITES", static_cast<uint32_t>(Input::Key::BROWSER_FAVORITES));
        key_table.add_pair("BROWSER_HOME", static_cast<uint32_t>(Input::Key::BROWSER_HOME));
        key_table.add_pair("VOLUME_MUTE", static_cast<uint32_t>(Input::Key::VOLUME_MUTE));
        key_table.add_pair("VOLUME_DOWN", static_cast<uint32_t>(Input::Key::VOLUME_DOWN));
        key_table.add_pair("VOLUME_UP", static_cast<uint32_t>(Input::Key::VOLUME_UP));
        key_table.add_pair("MEDIA_NEXT_TRACK", static_cast<uint32_t>(Input::Key::MEDIA_NEXT_TRACK));
        key_table.add_pair("MEDIA_PREV_TRACK", static_cast<uint32_t>(Input::Key::MEDIA_PREV_TRACK));
        key_table.add_pair("MEDIA_STOP", static_cast<uint32_t>(Input::Key::MEDIA_STOP));
        key_table.add_pair("MEDIA_PLAY_PAUSE", static_cast<uint32_t>(Input::Key::MEDIA_PLAY_PAUSE));
        key_table.add_pair("LAUNCH_MAIL", static_cast<uint32_t>(Input::Key::LAUNCH_MAIL));
        key_table.add_pair("LAUNCH_MEDIA_SELECT", static_cast<uint32_t>(Input::Key::LAUNCH_MEDIA_SELECT));
        key_table.add_pair("LAUNCH_APP1", static_cast<uint32_t>(Input::Key::LAUNCH_APP1));
        key_table.add_pair("LAUNCH_APP2", static_cast<uint32_t>(Input::Key::LAUNCH_APP2));
        key_table.add_pair("OEM_ONE", static_cast<uint32_t>(Input::Key::OEM_ONE));
        key_table.add_pair("OEM_PLUS", static_cast<uint32_t>(Input::Key::OEM_PLUS));
        key_table.add_pair("OEM_COMMA", static_cast<uint32_t>(Input::Key::OEM_COMMA));
        key_table.add_pair("OEM_MINUS", static_cast<uint32_t>(Input::Key::OEM_MINUS));
        key_table.add_pair("OEM_PERIOD", static_cast<uint32_t>(Input::Key::OEM_PERIOD));
        key_table.add_pair("OEM_TWO", static_cast<uint32_t>(Input::Key::OEM_TWO));
        key_table.add_pair("OEM_THREE", static_cast<uint32_t>(Input::Key::OEM_THREE));
        key_table.add_pair("OEM_FOUR", static_cast<uint32_t>(Input::Key::OEM_FOUR));
        key_table.add_pair("OEM_FIVE", static_cast<uint32_t>(Input::Key::OEM_FIVE));
        key_table.add_pair("OEM_SIX", static_cast<uint32_t>(Input::Key::OEM_SIX));
        key_table.add_pair("OEM_SEVEN", static_cast<uint32_t>(Input::Key::OEM_SEVEN));
        key_table.add_pair("OEM_EIGHT", static_cast<uint32_t>(Input::Key::OEM_EIGHT));
        key_table.add_pair("OEM_102", static_cast<uint32_t>(Input::Key::OEM_102));
        key_table.add_pair("IME_PROCESS", static_cast<uint32_t>(Input::Key::IME_PROCESS));
        key_table.add_pair("PACKET", static_cast<uint32_t>(Input::Key::PACKET));
        key_table.add_pair("ATTN", static_cast<uint32_t>(Input::Key::ATTN));
        key_table.add_pair("CRSEL", static_cast<uint32_t>(Input::Key::CRSEL));
        key_table.add_pair("EXSEL", static_cast<uint32_t>(Input::Key::EXSEL));
        key_table.add_pair("EREOF", static_cast<uint32_t>(Input::Key::EREOF));
        key_table.add_pair("PLAY", static_cast<uint32_t>(Input::Key::PLAY));
        key_table.add_pair("ZOOM", static_cast<uint32_t>(Input::Key::ZOOM));
        key_table.add_pair("PA1", static_cast<uint32_t>(Input::Key::PA1));
        key_table.add_pair("OEM_CLEAR", static_cast<uint32_t>(Input::Key::OEM_CLEAR));
        key_table.make_global("Key");

        LuaMadeSimple::Lua::Table modifier_key_table = lua.prepare_new_table();
        modifier_key_table.add_pair("SHIFT", 0x10);
        modifier_key_table.add_pair("CONTROL", 0x11);
        modifier_key_table.add_pair("ALT", 0x12);
        /*modifier_key_table.add_pair("LEFT_SHIFT", 0xA0);
        modifier_key_table.add_pair("RIGHT_SHIFT", 0xA1);
        modifier_key_table.add_pair("LEFT_CONTROL", 0xA2);
        modifier_key_table.add_pair("RIGHT_CONTROL", 0xA3);
        modifier_key_table.add_pair("LEFT_ALT", 0xA4);
        modifier_key_table.add_pair("RIGHT_ALT", 0xA5);*/
        modifier_key_table.make_global("ModifierKey");
    }

    static auto register_object_flags(const LuaMadeSimple::Lua& lua) -> void
    {
        LuaMadeSimple::Lua::Table object_flags_table = lua.prepare_new_table();
        object_flags_table.add_pair("RF_NoFlags", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NoFlags));
        object_flags_table.add_pair("RF_Public", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_Public));
        object_flags_table.add_pair("RF_Standalone", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_Standalone));
        object_flags_table.add_pair("RF_MarkAsNative", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_MarkAsNative));
        object_flags_table.add_pair("RF_Transactional", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_Transactional));
        object_flags_table.add_pair("RF_ClassDefaultObject",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_ClassDefaultObject));
        object_flags_table.add_pair("RF_ArchetypeObject", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_ArchetypeObject));
        object_flags_table.add_pair("RF_Transient", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_Transient));
        object_flags_table.add_pair("RF_MarkAsRootSet", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_MarkAsRootSet));
        object_flags_table.add_pair("RF_TagGarbageTemp", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_TagGarbageTemp));
        object_flags_table.add_pair("RF_NeedInitialization",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NeedInitialization));
        object_flags_table.add_pair("RF_NeedLoad", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NeedLoad));
        object_flags_table.add_pair("RF_KeepForCooker", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_KeepForCooker));
        object_flags_table.add_pair("RF_NeedPostLoad", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NeedPostLoad));
        object_flags_table.add_pair("RF_NeedPostLoadSubobjects",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NeedPostLoadSubobjects));
        object_flags_table.add_pair("RF_NewerVersionExists",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NewerVersionExists));
        object_flags_table.add_pair("RF_BeginDestroyed", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_BeginDestroyed));
        object_flags_table.add_pair("RF_FinishDestroyed", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_FinishDestroyed));
        object_flags_table.add_pair("RF_BeingRegenerated", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_BeingRegenerated));
        object_flags_table.add_pair("RF_DefaultSubObject", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_DefaultSubObject));
        object_flags_table.add_pair("RF_WasLoaded", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_WasLoaded));
        object_flags_table.add_pair("RF_TextExportTransient",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_TextExportTransient));
        object_flags_table.add_pair("RF_LoadCompleted", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_LoadCompleted));
        object_flags_table.add_pair("RF_InheritableComponentTemplate",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_InheritableComponentTemplate));
        object_flags_table.add_pair("RF_DuplicateTransient",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_DuplicateTransient));
        object_flags_table.add_pair("RF_StrongRefOnFrame", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_StrongRefOnFrame));
        object_flags_table.add_pair("RF_NonPIEDuplicateTransient",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_NonPIEDuplicateTransient));
        object_flags_table.add_pair("RF_Dynamic", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_Dynamic));
        object_flags_table.add_pair("RF_WillBeLoaded", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_WillBeLoaded));
        object_flags_table.add_pair("RF_HasExternalPackage",
                                    static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_HasExternalPackage));
        object_flags_table.add_pair("RF_AllFlags", static_cast<std::underlying_type_t<Unreal::EObjectFlags>>(Unreal::EObjectFlags::RF_AllFlags));
        object_flags_table.make_global("EObjectFlags");

        LuaMadeSimple::Lua::Table object_internal_flags_table = lua.prepare_new_table();
        object_internal_flags_table.add_pair("ReachableInCluster",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::ReachableInCluster));
        object_internal_flags_table.add_pair("ClusterRoot",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::ClusterRoot));
        object_internal_flags_table.add_pair("Native", static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::Native));
        object_internal_flags_table.add_pair("Async", static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::Async));
        object_internal_flags_table.add_pair("AsyncLoading",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::AsyncLoading));
        object_internal_flags_table.add_pair("Unreachable",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::Unreachable));
        object_internal_flags_table.add_pair("PendingKill",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::PendingKill));
        object_internal_flags_table.add_pair("RootSet", static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::RootSet));
        object_internal_flags_table.add_pair("GarbageCollectionKeepFlags",
                                             static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(
                                                     Unreal::EInternalObjectFlags::GarbageCollectionKeepFlags));
        object_internal_flags_table.add_pair("AllFlags", static_cast<std::underlying_type_t<Unreal::EInternalObjectFlags>>(Unreal::EInternalObjectFlags::AllFlags));
        object_internal_flags_table.make_global("EInternalObjectFlags");
    }

    static auto register_efindname(const LuaMadeSimple::Lua& lua) -> void
    {
        LuaMadeSimple::Lua::Table efindname_table = lua.prepare_new_table();
        efindname_table.add_pair("FNAME_Find", static_cast<std::underlying_type_t<Unreal::EFindName>>(Unreal::EFindName::FNAME_Find));
        efindname_table.add_pair("FNAME_Add", static_cast<std::underlying_type_t<Unreal::EFindName>>(Unreal::EFindName::FNAME_Add));
        efindname_table.add_pair("FNAME_Replace_Not_Safe_For_Threading",
                                 static_cast<std::underlying_type_t<Unreal::EFindName>>(Unreal::EFindName::FNAME_Replace_Not_Safe_For_Threading));
        efindname_table.make_global("EFindName");
    }

    LuaMod::LuaMod(UE4SSProgram& program, StringType&& mod_name, StringType&& mod_path)
        : Mod(program, std::move(mod_name), std::move(mod_path)), m_lua(LuaMadeSimple::new_state())
    {
        // First check for "Scripts" (capital S)
        std::filesystem::path scripts_path = m_mod_path / STR("Scripts");

        // If not found, try with lowercase "scripts"
        if (!std::filesystem::exists(scripts_path))
        {
            std::filesystem::path alt_scripts_path = m_mod_path / STR("scripts");
            if (std::filesystem::exists(alt_scripts_path))
            {
                scripts_path = alt_scripts_path;
            }
        }

        m_scripts_path = scripts_path;

        if (!std::filesystem::exists(m_scripts_path))
        {
            Output::send<LogLevel::Error>(STR("Mod path doesn't exist {}\n"), ensure_str(m_scripts_path));
            set_installable(false);
            return;
        }
    }

    auto LuaMod::global_uninstall() -> void
    {
    }

    template <typename PropertyType>
    auto add_property_type_table(const LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua::Table& property_types_table, std::string_view property_type_name) -> void
    {
        property_types_table.add_key(property_type_name.data());

        auto property_type_table = lua.prepare_new_table();
        property_type_table.add_pair("Name", property_type_name.data());

        if constexpr (Unreal::IsTProperty<PropertyType>)
        {
            // TODO: Update LuaMadeSimple to accept an unsigned long long, and do it with proper bounds checking
            property_type_table.add_pair("Size", static_cast<int64_t>(sizeof(typename PropertyType::TCppType)));
        }
        else
        {
            // Sizes for types are unknown and will only be known dynamically at runtime
            // TODO: The size is used in LuaTArray to calculate the address of an element (element index * size)
            //       Reimplement this by requiring a custom "Size" field in the Lua table
            property_type_table.add_pair("Size", 0);
        }

        // property_type_table.add_pair("Size", PropertyType::size);
        //  This should be a lightuserdata instead of a reinterpret_cast to int64_t
        //  This is not very safe at all, what if the pointer is too large for a signed 64-bit integer ?
        property_type_table.add_pair("FFieldClassPointer", static_cast<int64_t>(PropertyType::StaticClass().HashObject()));
        // TODO: Figure out if the static object pointer is needed
        property_type_table.add_pair("StaticPointer", 0);

        property_types_table.fuse_pair();

        property_type_table.make_local();
    }

    // Private helper: Ensures hook thread exists and returns the registry reference (or LUA_REFNIL if already exists)
    static int ensure_hook_thread_exists(LuaMod* mod)
    {
        if (mod->m_hook_lua == nullptr)
        {
            // First use - create new thread and anchor it in the registry
            mod->m_hook_lua = &mod->lua().new_thread();
            int thread_ref = luaL_ref(mod->lua().get_lua_state(), LUA_REGISTRYINDEX);
            return thread_ref;
        }

        // Thread already exists and is already anchored
        return LUA_REFNIL;
    }

    // Returns the hook lua thread for immediate use (doesn't need registry reference management)
    auto static get_hook_lua(LuaMod* mod) -> LuaMadeSimple::Lua*
    {
        ensure_hook_thread_exists(mod);
        return mod->m_hook_lua;
    }

    // Returns hook state with registry reference (for persistent hooks that need cleanup)
    // auto static make_hook_state(Mod* mod, const LuaMadeSimple::Lua& lua)->std::shared_ptr<LuaMadeSimple::Lua>
    auto static make_hook_state(LuaMod* mod) -> std::pair<LuaMadeSimple::Lua*, int>
    {
        int thread_ref = ensure_hook_thread_exists(mod);
        return {mod->m_hook_lua, thread_ref};

        // Make the hook thread (which is just a separate Lua stack) be a global in its parent.
        // This is needed because otherwise it will be GCd when we don't want it to.
        // lua_setglobal(lua.get_lua_state(), "HookThread");

        // Commenting out until we switch to lua_newstate instead of lua_newthread.
        // For the switch to happen, we need to be able to move or copy Lua types across lua_states which we can't do yet.
        /*
        mod->m_hook_lua->register_function("RegisterHook", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.throw_error("'RegisterHook' is not allowed from the game thread");
            return 0;
        });

        mod->m_hook_lua->register_function("NotifyOnNewObject", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.throw_error("'NotifyOnNewObject' is not allowed in the game thread");
            return 0;
        });
        //*/
        //}
    }

    auto static make_async_state(LuaMod* mod, const LuaMadeSimple::Lua& lua) -> void
    {
        if (!mod->m_async_lua)
        {
            mod->m_async_lua = &lua.new_thread();

            // Make the hook thread (which is just a separate Lua stack) be a global in its parent.
            // This is needed because otherwise it will be GCd when we don't want it to.
            lua_setglobal(lua.get_lua_state(), "AsyncThread");

            // Commenting out until we switch to lua_newstate instead of lua_newthread.
            // For the switch to happen, we need to be able to move or copy Lua types across lua_states which we can't do yet.
            /*
            mod->m_hook_lua->register_function("RegisterHook", [](const LuaMadeSimple::Lua& lua) -> int {
                lua.throw_error("'RegisterHook' is not allowed from the game thread");
                return 0;
            });

            mod->m_hook_lua->register_function("NotifyOnNewObject", [](const LuaMadeSimple::Lua& lua) -> int {
                lua.throw_error("'NotifyOnNewObject' is not allowed in the game thread");
                return 0;
            });
            //*/
        }
    }

    auto static make_main_state(LuaMod* mod, const LuaMadeSimple::Lua& lua) -> void
    {
        if (!mod->m_main_lua)
        {
            mod->m_main_lua = &lua.new_thread();

            // Make the hook thread (which is just a separate Lua stack) be a global in its parent.
            // This is needed because otherwise it will be GCd when we don't want it to.
            lua_setglobal(lua.get_lua_state(), "MainThread");

            // Commenting out until we switch to lua_newstate instead of lua_newthread.
            // For the switch to happen, we need to be able to move or copy Lua types across lua_states which we can't do yet.
            /*
            mod->m_hook_lua->register_function("RegisterHook", [](const LuaMadeSimple::Lua& lua) -> int {
                lua.throw_error("'RegisterHook' is not allowed from the game thread");
                return 0;
            });

            mod->m_hook_lua->register_function("NotifyOnNewObject", [](const LuaMadeSimple::Lua& lua) -> int {
                lua.throw_error("'NotifyOnNewObject' is not allowed in the game thread");
                return 0;
            });
            //*/
        }
    }
    auto static register_all_property_types(const LuaMadeSimple::Lua& lua) -> void
    {
        auto property_types_table = lua.prepare_new_table();

        add_property_type_table<Unreal::FObjectProperty>(lua, property_types_table, "ObjectProperty");
        add_property_type_table<Unreal::FObjectPtrProperty>(lua, property_types_table, "ObjectPtrProperty");
        add_property_type_table<Unreal::FInt8Property>(lua, property_types_table, "Int8Property");
        add_property_type_table<Unreal::FInt16Property>(lua, property_types_table, "Int16Property");
        add_property_type_table<Unreal::FIntProperty>(lua, property_types_table, "IntProperty");
        add_property_type_table<Unreal::FInt64Property>(lua, property_types_table, "Int64Property");
        add_property_type_table<Unreal::FByteProperty>(lua, property_types_table, "ByteProperty");
        add_property_type_table<Unreal::FUInt16Property>(lua, property_types_table, "UInt16Property");
        add_property_type_table<Unreal::FUInt32Property>(lua, property_types_table, "UInt32Property");
        add_property_type_table<Unreal::FUInt64Property>(lua, property_types_table, "UInt64Property");
        add_property_type_table<Unreal::FNameProperty>(lua, property_types_table, "NameProperty");
        add_property_type_table<Unreal::FFloatProperty>(lua, property_types_table, "FloatProperty");
        // add_property_type_table<Unreal::FStrProperty>(lua, property_types_table, "StrProperty");
        add_property_type_table<Unreal::FBoolProperty>(lua, property_types_table, "BoolProperty");
        add_property_type_table<Unreal::FArrayProperty>(lua, property_types_table, "ArrayProperty");
        add_property_type_table<Unreal::FSetProperty>(lua, property_types_table, "SetProperty");
        add_property_type_table<Unreal::FMapProperty>(lua, property_types_table, "MapProperty");
        add_property_type_table<Unreal::FStructProperty>(lua, property_types_table, "StructProperty");
        add_property_type_table<Unreal::FClassProperty>(lua, property_types_table, "ClassProperty");
        add_property_type_table<Unreal::FWeakObjectProperty>(lua, property_types_table, "WeakObjectProperty");
        if (Unreal::Version::IsAtLeast(4, 15))
        {
            add_property_type_table<Unreal::FEnumProperty>(lua, property_types_table, "EnumProperty");
        }
        add_property_type_table<Unreal::FTextProperty>(lua, property_types_table, "TextProperty");
        add_property_type_table<Unreal::FStrProperty>(lua, property_types_table, "StrProperty");
        if (Unreal::Version::IsAtLeast(5, 6))
        {
            add_property_type_table<Unreal::FUtf8StrProperty>(lua, property_types_table, "Utf8StrProperty");
        }

        property_types_table.make_global("PropertyTypes");
    }

    auto LuaMod::setup_custom_module_loader(const LuaMadeSimple::Lua* lua_state) -> void
    {
        lua_State* L = lua_state->get_lua_state();
    
        // Initialize ue4ss_loaded_modules table
        lua_newtable(L);
        lua_setglobal(L, "ue4ss_loaded_modules");
    
        // Get package.searchers table
        lua_getglobal(L, "package");
        if (!lua_istable(L, -1))
        {
            Output::send<LogLevel::Error>(STR("package table not found\n"));
            lua_pop(L, 1);
            return;
        }
    
        lua_getfield(L, -1, "searchers");
        if (!lua_istable(L, -1))
        {
            Output::send<LogLevel::Error>(STR("package.searchers table not found\n"));
            lua_pop(L, 2);
            return;
        }
    
        // Insert our searcher at position 1
        // First, shift existing searchers up
        lua_Integer len = luaL_len(L, -1);
        for (lua_Integer i = len; i >= 1; i--)
        {
            lua_geti(L, -1, i);     // Get searchers[i]
            lua_seti(L, -2, i + 1); // Set searchers[i+1] = searchers[i]
        }
    
        // Push the LuaMod instance as a light userdata (our upvalue)
        lua_pushlightuserdata(L, this);
    
        // Create the C closure with one upvalue
        lua_pushcclosure(L, custom_module_searcher, 1);
    
        // Set searchers[1] = our new closure
        lua_seti(L, -2, 1);
    
        lua_pop(L, 2); // Clean up stack: searchers, package
    }

    // Static C function for the module searcher
    int LuaMod::custom_module_searcher(lua_State* L)
    {
        const char* module_name = luaL_checkstring(L, 1);
        if (!module_name)
        {
            lua_pushstring(L, "module name is required");
            return 1;
        }
        
        // Get the LuaMod* from the upvalue at index 1
        auto* lua_mod = static_cast<LuaMod*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (!lua_mod)
        {
            lua_pushstring(L, "custom searcher is missing its C++ context");
            return 1;
        }
        
        // Get paths directly from the C++ object and convert to UTF-8 strings for Lua
        const auto& mods_dir = lua_mod->get_program().get_mods_directory();
        const auto& mod_name = lua_mod->get_name();
        const auto& scripts_path = lua_mod->get_scripts_path();
        
        std::string mods_path_str = normalize_path_for_lua(mods_dir);
        std::string mod_name_str = to_utf8_string(mod_name);
        std::string scripts_path_str = normalize_path_for_lua(scripts_path);
        
        // Try different path combinations
        std::vector<std::string> paths_to_try = {
            scripts_path_str + "/" + module_name + ".lua",
            mods_path_str + "/shared/" + module_name + ".lua",
            mods_path_str + "/shared/" + module_name + "/" + module_name + ".lua"
        };
        
        // Try each path
        std::string attempted_paths_str;
        for (const auto& path : paths_to_try)
        {
            // Convert to wide string for Windows filesystem operations
            std::wstring wide_path;
            try
            {
                wide_path = utf8_to_wpath(path);
            }
            catch (const std::exception&)
            {
                attempted_paths_str += "\n\t" + path + " (encoding error)";
                continue;
            }
            
            if (!std::filesystem::exists(wide_path))
            {
                attempted_paths_str += "\n\t" + path;
                continue;
            }
            
            // Check if already loaded
            lua_getglobal(L, "ue4ss_loaded_modules");
            lua_pushstring(L, path.c_str());
            lua_gettable(L, -2);
            
            if (!lua_isnil(L, -1))
            {
                // Already loaded, return the cached function
                lua_remove(L, -2); // Remove ue4ss_loaded_modules table
                return 1;
            }
            
            lua_pop(L, 2); // Pop nil and ue4ss_loaded_modules
            
            // Try to load the file
            std::ifstream file(wide_path, std::ios::binary);
            if (!file.is_open())
            {
                attempted_paths_str += "\n\t" + path + " (cannot open)";
                continue;
            }
            
            // Get file size and read content
            file.seekg(0, std::ios::end);
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            std::vector<char> buffer(size);
            if (!file.read(buffer.data(), size))
            {
                attempted_paths_str += "\n\t" + path + " (read error)";
                continue;
            }
            file.close();
            
            // Create chunk name for debugging
            std::string chunk_name = "@" + path;
            
            // Load the script as a function that returns the module
            std::string module_wrapper = "return function()\n" + std::string(buffer.data(), buffer.size()) + "\nend";
            
            if (luaL_loadbuffer(L, module_wrapper.c_str(), module_wrapper.size(), chunk_name.c_str()) != LUA_OK)
            {
                attempted_paths_str += "\n\t" + path + " (syntax error: " + lua_tostring(L, -1) + ")";
                lua_pop(L, 1); // Pop error message
                continue;
            }
            
            // Execute to get the loader function
            if (lua_pcall(L, 0, 1, 0) != LUA_OK)
            {
                attempted_paths_str += "\n\t" + path + " (execution error: " + lua_tostring(L, -1) + ")";
                lua_pop(L, 1); // Pop error message
                continue;
            }
            
            // Cache the loaded module
            lua_getglobal(L, "ue4ss_loaded_modules");
            lua_pushstring(L, path.c_str());
            lua_pushvalue(L, -3); // Copy the function
            lua_settable(L, -3);
            lua_pop(L, 1); // Pop ue4ss_loaded_modules
            
            // Return the loader function
            return 1;
        }
        
        // Module not found
        std::string error_msg = "module '" + std::string(module_name) + "' not found:" + attempted_paths_str;
        lua_pushstring(L, error_msg.c_str());
        return 1;
    }

    auto LuaMod::setup_lua_require_paths(const LuaMadeSimple::Lua& lua) const -> void
    {
        try
        {
            auto* lua_state = m_lua.get_lua_state();
            lua_getglobal(lua_state, "package");

            // Get current paths
            lua_getfield(lua_state, -1, "path");
            std::string current_paths = lua_tostring(lua_state, -1);
            lua_pop(lua_state, 1);

            auto mods_dir = m_program.get_mods_directory();
            auto mod_name = get_name();

            // Create normalized UTF-8 paths with forward slashes
            std::string mods_dir_utf8 = normalize_path_for_lua(mods_dir);
            std::string mod_name_utf8 = to_utf8_string(mod_name);
            std::string scripts_path_utf8 = normalize_path_for_lua(m_scripts_path);

            // Create path strings with forward slashes for Lua
            std::string script_path = fmt::format(";{}/?.lua", scripts_path_utf8);
            std::string shared_path = fmt::format(";{}/shared/?.lua", mods_dir_utf8);
            std::string shared_nested_path = fmt::format(";{}/shared/?/?.lua", mods_dir_utf8);

            current_paths.append(script_path);
            current_paths.append(shared_path);
            current_paths.append(shared_nested_path);

            lua_pushstring(lua_state, current_paths.c_str());
            lua_setfield(lua_state, -2, "path");

            // Now set up cpath similarly
            lua_getfield(lua_state, -1, "cpath");
            std::string current_cpaths = lua_tostring(lua_state, -1);
            lua_pop(lua_state, 1);

            // Create cpath strings
            std::string script_dll_path = fmt::format(";{}/?.dll", scripts_path_utf8);
            std::string mod_dll_path = fmt::format(";{}/{}/?/?.dll", mods_dir_utf8, mod_name_utf8);

            current_cpaths.append(script_dll_path);
            current_cpaths.append(mod_dll_path);

            lua_pushstring(lua_state, current_cpaths.c_str());
            lua_setfield(lua_state, -2, "cpath");

            lua_pop(lua_state, 1); // Pop package table
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("Exception in setup_lua_require_paths: {}\n"), ensure_str(e.what()));
            throw;
        }
    }

    auto LuaMod::get_object_names(const Unreal::UObject* object) -> std::vector<Unreal::FName>
    {
        std::vector<Unreal::FName> names{};
        for (auto ptr = object; ptr; ptr = ptr->GetOuterPrivate())
        {
            names.emplace_back(ptr->GetNamePrivate());
        }
        return names;
    }

    auto LuaMod::find_function_hook_data(std::vector<FunctionHookData>& container, Unreal::FName in_name) -> FunctionHookData*
    {
        for (auto it = container.begin(); it != container.end(); ++it)
        {
            if (it->names.size() >= 1 && in_name.Equals(it->names[0]))
            {
                return &*it;
            }
        }
        return nullptr;
    }

    auto LuaMod::find_function_hook_data(std::vector<FunctionHookData>& container, const Unreal::UObject* object) -> FunctionHookData*
    {
        return find_function_hook_data(container, get_object_names(object));
    }

    auto LuaMod::find_function_hook_data(std::vector<FunctionHookData>& container, const std::vector<Unreal::FName>& in_name) -> FunctionHookData*
    {
        for (auto it = container.begin(); it != container.end(); ++it)
        {
            auto found = true;
            if (in_name.size() != it->names.size())
            {
                continue;
            }
            for (const auto& [index, name] : std::ranges::enumerate_view(it->names))
            {
                if (!name.Equals(in_name[index]))
                {
                    found = false;
                }
            }
            if (found)
            {
                return &*it;
            }
        }
        return nullptr;
    }

    auto LuaMod::remove_function_hook_data(std::vector<FunctionHookData>& container, StringViewType in_name) -> void
    {
        remove_function_hook_data(container, Unreal::FName(in_name, Unreal::FNAME_Add));
    }

    auto LuaMod::remove_function_hook_data(std::vector<FunctionHookData>& container, Unreal::FName in_name) -> void
    {
        for (auto it = container.begin(); it != container.end(); ++it)
        {
            if (it->names.size() >= 1 && it->names[0] == in_name)
            {
                container.erase(it);
                break;
            }
        }
    }

    auto LuaMod::remove_function_hook_data(std::vector<FunctionHookData>& container, const Unreal::UObject* object) -> void
    {
        remove_function_hook_data(container, get_object_names(object));
    }

    auto LuaMod::remove_function_hook_data(std::vector<FunctionHookData>& container, const std::vector<Unreal::FName>& in_name) -> void
    {
        for (auto it = container.begin(); it != container.end(); ++it)
        {
            auto found = true;
            if (in_name.size() != it->names.size())
            {
                continue;
            }
            for (const auto& [index, name] : std::ranges::enumerate_view(it->names))
            {
                if (name != in_name[index])
                {
                    found = false;
                }
            }
            if (found)
            {
                container.erase(it);
            }
        }
    }

    auto static setup_lua_global_functions_internal(const LuaMadeSimple::Lua& lua, Mod::IsTrueMod is_true_mod) -> void
    {
        lua.register_function("print", LuaLibrary::global_print);
        lua.register_function("LoadExport", LuaLibrary::load_export);

        lua.register_function("CreateInvalidObject", [](const LuaMadeSimple::Lua& lua) -> int {
            LuaType::auto_construct_object(lua, nullptr);
            return 1;
        });

        lua.register_function("StaticFindObject", [](const LuaMadeSimple::Lua& lua) -> int {
            // Stack size @ the start of the function is the same as the number of params
            int32_t stack_size = lua.get_stack_size();

            if (stack_size <= 0)
            {
                lua.throw_error("Function 'StaticFindObject' cannot be called with 0 parameters.");
            }

            std::string error_overload_not_found{R"(
No overload found for function 'StaticFindObject'.
Overloads:
#1: StaticFindObject(string name)
#2: StaticFindObject(UClass* Class, UObject* InOuter, string name, bool ExactClass = false))"};

            // Overload #1
            // P1: string name
            // Ignores any params after P1
            if (lua.is_string())
            {
                Unreal::UObject* object = Unreal::UObjectGlobals::StaticFindObject(nullptr, nullptr, ensure_str(lua.get_string()));

                // Construct a Lua object of type 'UObject'
                // Auto constructing is nullptr safe
                LuaType::auto_construct_object(lua, object);

                return 1;
            }

            // Overload #2
            // P1: UClass* Class
            // P2: UObject* InOuter
            // P3: string Name
            // P4: bool ExactClass = false
            // Full definition of StaticFindObject, including default values
            // Ignores any params after P4
            if (stack_size < 3)
            {
                // No overload found for function 'StaticFindObject'. Overloads are:
                lua.throw_error(error_overload_not_found);
            }

            Unreal::UClass* param_class{};
            Unreal::UObject* param_in_outer{};
            StringType param_name{};
            bool param_exact_class{};

            // P1 (Class), userdata
            if (lua.is_userdata())
            {
                auto& lua_object = lua.get_userdata<LuaType::UClass>();
                param_class = lua_object.get_remote_cpp_object();
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }
            else
            {
                lua.throw_error(error_overload_not_found);
            }

            // P2 (InOuter), userdata
            if (lua.is_userdata())
            {
                auto& lua_object = lua.get_userdata<LuaType::UObject>();
                param_in_outer = lua_object.get_remote_cpp_object();
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }
            else
            {
                lua.throw_error(error_overload_not_found);
            }

            // P3 (Name), string
            if (lua.is_string())
            {
                param_name = ensure_str(lua.get_string());
            }
            else
            {
                lua.throw_error(error_overload_not_found);
            }

            // P4 (ExactClass), bool = false
            if (lua.is_bool())
            {
                param_exact_class = lua.get_bool();
            }
            // There's no error if P4 isn't a bool, simply ignore all parameters after P3

            Unreal::UObject* object = Unreal::UObjectGlobals::StaticFindObject(param_class, param_in_outer, param_name, param_exact_class);

            // Construct a Lua object of type 'UObject'
            // Auto constructing is nullptr safe
            LuaType::auto_construct_object(lua, object);

            return 1;
        });

        lua.register_function("FindFirstOf", [](const LuaMadeSimple::Lua& lua) -> int {
            // Stack size @ the start of the function is the same as the number of params
            int32_t stack_size = lua.get_stack_size();

            if (stack_size <= 0)
            {
                lua.throw_error("Function 'FindFirstOf' cannot be called with 0 parameters.");
            }

            std::string error_overload_not_found{R"(
No overload found for function 'FindFirstOf'.
Overloads:
#1: FindFirstOf(string short_class_name))"};

            // Overload #1
            // P1: string short_name
            // Ignores any params after P1
            if (lua.is_string())
            {
                Unreal::UObject* object = Unreal::UObjectGlobals::FindFirstOf(ensure_str(lua.get_string()));

                // Construct a Lua object of type 'UObject'
                // Auto constructing is nullptr safe
                LuaType::auto_construct_object(lua, object);

                return 1;
            }
            else
            {
                lua.throw_error(error_overload_not_found);
            }

            return 0;
        });

        lua.register_function("FindAllOf", [](const LuaMadeSimple::Lua& lua) -> int {
            // Stack size @ the start of the function is the same as the number of params
            int32_t stack_size = lua.get_stack_size();

            if (stack_size <= 0)
            {
                lua.throw_error("Function 'FindAllOf' cannot be called with 0 parameters.");
            }

            std::string error_overload_not_found{R"(
No overload found for function 'FindAllOf'.
Overloads:
#1: FindAllOf(string short_class_name))"};

            // Overload #1
            // P1: string short_name
            // Ignores any params after P1
            if (lua.is_string())
            {
                constexpr int32_t elements_to_reserve = 40;

                std::vector<Unreal::UObject*> found_unreal_objects;

                // Reserving some space because FindAllOf is likely to find lots of objects
                found_unreal_objects.reserve(elements_to_reserve);

                Unreal::UObjectGlobals::FindAllOf(lua.get_string(), found_unreal_objects);

                if (!found_unreal_objects.empty())
                {
                    LuaMadeSimple::Lua::Table table = lua.prepare_new_table(elements_to_reserve);

                    for (size_t count{}; const auto& unreal_object : found_unreal_objects)
                    {
                        // Increasing the count first, this is to accommodate the one-index based tables of Lua
                        ++count;

                        table.add_key(count);

                        // Construct a Lua version of a UObject
                        // It will be at the top of the Lua stack and can act as the value of a key/value pair if fuse_pair() is called
                        LuaType::auto_construct_object(lua, unreal_object);
                        table.fuse_pair();
                    }

                    table.make_local();
                }
                else
                {
                    lua.set_nil();
                }

                return 1;
            }
            else
            {
                lua.throw_error(error_overload_not_found);
            }

            // This code isn't executed
            // Lua will error out in the else statement above
            // This is purely to shut the compiler up
            lua.set_nil();
            return 1;
        });

        if (is_true_mod == Mod::IsTrueMod::Yes)
        {
            lua.register_function("IsKeyBindRegistered", [](const LuaMadeSimple::Lua& lua) -> int {
                std::string error_overload_not_found{R"(
No overload found for function 'IsKeyBindRegistered'.
Overloads:
#1: IsKeyBindRegistered(integer key)
#2: IsKeyBindRegistered(integer key, table modifier_key_integers))"};

                if (!lua.is_integer())
                {
                    lua.throw_error(error_overload_not_found);
                }

                auto key_from_lua = lua.get_integer();
                if (key_from_lua < std::numeric_limits<uint8_t>::min() || key_from_lua > std::numeric_limits<uint8_t>::max())
                {
                    lua.throw_error("Parameter #1 for function 'IsKeyBindRegistered' must be an integer between 0 and 255");
                }
                auto key_to_check = static_cast<Input::Key>(key_from_lua);

                const auto mod = get_mod_ref(lua);

                if (lua.is_table())
                {
                    Input::Handler::ModifierKeyArray modifier_keys{};

                    uint8_t table_counter{};
                    lua.for_each_in_table([&](LuaMadeSimple::LuaTableReference table) -> bool {
                        if (!table.value.is_integer())
                        {
                            lua.throw_error(
                                    "Lua function 'IsKeyBindRegistered', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        int64_t full_integer = table.value.get_integer();
                        if (full_integer < std::numeric_limits<uint8_t>::min() || full_integer > std::numeric_limits<uint8_t>::max())
                        {
                            lua.throw_error(
                                    "Lua function 'IsKeyBindRegistered', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        modifier_keys[table_counter++] = static_cast<Input::ModifierKey>(table.value.get_integer());

                        return false;
                    });

                    if (table_counter > 0)
                    {
                        lua.set_bool(mod->m_program.is_keydown_event_registered(key_to_check, modifier_keys));
                    }
                    else
                    {
                        lua.set_bool(mod->m_program.is_keydown_event_registered(key_to_check));
                    }
                }
                else
                {
                    lua.set_bool(mod->m_program.is_keydown_event_registered(key_to_check));
                }

                return 1;
            });

            lua.register_function("RegisterKeyBindAsync", [](const LuaMadeSimple::Lua& lua) -> int {
                std::string error_overload_not_found{R"(
No overload found for function 'RegisterKeyBindAsync'.
Overloads:
#1: RegisterKeyBindAsync(integer key)
#2: RegisterKeyBindAsync(integer key, table modifier_key_integers))"};

                Mod* mod = get_mod_ref(lua);

                if (!lua.is_integer())
                {
                    lua.throw_error(error_overload_not_found);
                }

                int64_t key_from_lua = lua.get_integer();
                if (key_from_lua < std::numeric_limits<uint8_t>::min() || key_from_lua > std::numeric_limits<uint8_t>::max())
                {
                    lua.throw_error("Parameter #1 for function 'RegisterKeyBindAsync' must be an integer between 0 and 255");
                }

                Input::Key key_to_register = static_cast<Input::Key>(key_from_lua);

                const auto lua_keybind_callback_lambda = [](const LuaMadeSimple::Lua& lua, const int callback_register_index) -> void {
                    try
                    {
                        lua.registry().get_function_ref(callback_register_index);
                        lua.call_function(0, 0);
                    }
                    catch (std::runtime_error& e)
                    {
                        Output::send<LogLevel::Error>(STR("{}\n"), ensure_str(lua.handle_error(e.what())));
                    }
                };

                if (lua.is_function())
                {
                    // Overload #1
                    // P1: Key to register
                    // P2: Callback

                    // Duplicate the Lua function to the top of the stack for luaL_ref
                    lua_pushvalue(lua.get_lua_state(), 1);

                    // Take a reference to the Lua function (it also pops it of the stack)
                    const int32_t lua_callback_registry_index = lua.registry().make_ref();

                    // Taking 'lua_callback_registry_index' by copy here to ensure its survival
                    // Using a 'custom_data' of 1 to signify that this keydown event was created by a mod
                    mod->m_program.register_keydown_event(
                            key_to_register,
                            [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                            },
                            1, mod);
                }
                else if (lua.is_table())
                {
                    // Overload #2
                    // P1: Key to register
                    // P2: Table of modifier keys
                    // P3: Callback

                    Input::Handler::ModifierKeyArray modifier_keys{};

                    uint8_t table_counter{};
                    lua.for_each_in_table([&](LuaMadeSimple::LuaTableReference table) -> bool {
                        if (!table.value.is_integer())
                        {
                            lua.throw_error(
                                    "Lua function 'RegisterKeyBindAsync', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        int64_t full_integer = table.value.get_integer();
                        if (full_integer < std::numeric_limits<uint8_t>::min() || full_integer > std::numeric_limits<uint8_t>::max())
                        {
                            lua.throw_error(
                                    "Lua function 'RegisterKeyBindAsync', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        modifier_keys[table_counter++] = static_cast<Input::ModifierKey>(table.value.get_integer());

                        return false;
                    });

                    // Duplicate the Lua function to the top of the stack for luaL_ref
                    lua_pushvalue(lua.get_lua_state(), 1);

                    // Take a reference to the Lua function (it also pops it of the stack)
                    const auto lua_callback_registry_index = lua.registry().make_ref();

                    if (table_counter > 0)
                    {
                        mod->m_program.register_keydown_event(
                                key_to_register,
                                modifier_keys,
                                [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                    lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                                },
                                1, mod);
                    }
                    else
                    {
                        mod->m_program.register_keydown_event(
                                key_to_register,
                                [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                    lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                                },
                                1, mod);
                    }
                }
                else
                {
                    lua.throw_error(error_overload_not_found);
                }

                return 0;
            });

            lua.register_function("RegisterKeyBind", [](const LuaMadeSimple::Lua& lua) -> int {
                std::string error_overload_not_found{R"(
No overload found for function 'RegisterKeyBind'.
Overloads:
#1: RegisterKeyBind(integer key)
#2: RegisterKeyBind(integer key, table modifier_key_integers))"};

                Mod* mod = get_mod_ref(lua);

                if (!lua.is_integer())
                {
                    lua.throw_error(error_overload_not_found);
                }

                int64_t key_from_lua = lua.get_integer();
                if (key_from_lua < std::numeric_limits<uint8_t>::min() || key_from_lua > std::numeric_limits<uint8_t>::max())
                {
                    lua.throw_error("Parameter #1 for function 'RegisterKeyBind' must be an integer between 0 and 255");
                }

                Input::Key key_to_register = static_cast<Input::Key>(key_from_lua);

                const auto lua_keybind_callback_lambda = [](const LuaMadeSimple::Lua& lua, const int callback_register_index) -> void {
                    std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                    try
                    {
                        lua.registry().get_function_ref(callback_register_index);
                        lua.call_function(0, 0);
                    }
                    catch (std::runtime_error& e)
                    {
                        Output::send<LogLevel::Error>(STR("{}\n"), ensure_str(lua.handle_error(e.what())));
                    }
                };

                if (lua.is_function())
                {
                    // Overload #1
                    // P1: Key to register
                    // P2: Callback

                    // Duplicate the Lua function to the top of the stack for luaL_ref
                    lua_pushvalue(lua.get_lua_state(), 1);

                    // Take a reference to the Lua function (it also pops it of the stack)
                    const int32_t lua_callback_registry_index = lua.registry().make_ref();

                    // Taking 'lua_callback_registry_index' by copy here to ensure its survival
                    // Using a 'custom_data' of 1 to signify that this keydown event was created by a mod
                    mod->m_program.register_keydown_event(
                            key_to_register,
                            [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                            },
                            1, mod);
                }
                else if (lua.is_table())
                {
                    // Overload #2
                    // P1: Key to register
                    // P2: Table of modifier keys
                    // P3: Callback

                    Input::Handler::ModifierKeyArray modifier_keys{};

                    uint8_t table_counter{};
                    lua.for_each_in_table([&](LuaMadeSimple::LuaTableReference table) -> bool {
                        if (!table.value.is_integer())
                        {
                            lua.throw_error("Lua function 'RegisterKeyBind', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        int64_t full_integer = table.value.get_integer();
                        if (full_integer < std::numeric_limits<uint8_t>::min() || full_integer > std::numeric_limits<uint8_t>::max())
                        {
                            lua.throw_error("Lua function 'RegisterKeyBind', overload #2, requires a table of 1-byte large integers as the second parameter");
                        }

                        modifier_keys[table_counter++] = static_cast<Input::ModifierKey>(table.value.get_integer());

                        return false;
                    });

                    // Duplicate the Lua function to the top of the stack for luaL_ref
                    lua_pushvalue(lua.get_lua_state(), 1);

                    // Take a reference to the Lua function (it also pops it of the stack)
                    const auto lua_callback_registry_index = lua.registry().make_ref();

                    if (table_counter > 0)
                    {
                        mod->m_program.register_keydown_event(
                                key_to_register,
                                modifier_keys,
                                [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                    lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                                },
                                1, mod);
                    }
                    else
                    {
                        mod->m_program.register_keydown_event(
                                key_to_register,
                                [&lua, lua_callback_registry_index, &lua_keybind_callback_lambda]() {
                                    lua_keybind_callback_lambda(lua, lua_callback_registry_index);
                                },
                                1, mod);
                    }
                }
                else
                {
                    lua.throw_error(error_overload_not_found);
                }

                return 0;
            });

            lua.register_function("UnregisterHook", [](const LuaMadeSimple::Lua& lua) -> int {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

                std::string error_overload_not_found{R"(
No overload found for function 'UnregisterHook'.
Overloads:
#1: UnregisterHook(string UFunction_Name, integer PreCallbackId, integer PostCallbackId))"};

                if (!lua.is_string())
                {
                    lua.throw_error(error_overload_not_found);
                }

                auto function_name_no_prefix = get_function_name_without_prefix(ensure_str(lua.get_string()));

                Unreal::UFunction* unreal_function = Unreal::UObjectGlobals::StaticFindObject<Unreal::UFunction*>(nullptr, nullptr, function_name_no_prefix);
                if (!unreal_function)
                {
                    lua.throw_error(std::format("Tried to unregister a hook with Lua function 'UnregisterHook' but no UFunction with the specified name "
                                                "was found.\n>FunctionName: {}",
                                                to_string(function_name_no_prefix)));
                }

                if (!lua.is_integer())
                {
                    lua.throw_error(error_overload_not_found);
                }
                const auto pre_id = lua.get_integer();

                if (!lua.is_integer())
                {
                    lua.throw_error(error_overload_not_found);
                }
                const auto post_id = lua.get_integer();

                if (pre_id > std::numeric_limits<int32_t>::max())
                {
                    lua.throw_error(std::format("Tried to unregister a hook with Lua function 'UnregisterHook' but the PreCallbackId supplied was too "
                                                "large (>int32)\n>FunctionName: {}",
                                                to_string(function_name_no_prefix)));
                }

                if (post_id > std::numeric_limits<int32_t>::max())
                {
                    lua.throw_error(std::format("Tried to unregister a hook with Lua function 'UnregisterHook' but the PostCallbackId supplied was too "
                                                "large (>int32)\n>FunctionName: {}",
                                                to_string(function_name_no_prefix)));
                }


                auto func_ptr = unreal_function->GetFunc();
                if (func_ptr && func_ptr != Unreal::UObject::ProcessInternalInternal.get_function_address() &&
                    unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))
                {
                    const auto hook_data = std::ranges::find_if(g_hooked_script_function_data, [&](const std::unique_ptr<LuaUnrealScriptFunctionData>& elem) {
                        return elem->post_callback_id == post_id && elem->pre_callback_id == pre_id;
                    });
                    if (hook_data != g_hooked_script_function_data.end())
                    {
                        hook_data->get()->scheduled_for_removal = true;
                    }
                }
                else if (func_ptr && func_ptr == Unreal::UObject::ProcessInternalInternal.get_function_address() &&
                         !unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))
                {
                    if (auto data_ptr = LuaMod::find_function_hook_data(LuaMod::m_script_hook_callbacks, unreal_function); data_ptr)
                    {
                        Output::send<LogLevel::Verbose>(STR("Unregistering script hook with id: {}, FunctionName: {}\n"), post_id, function_name_no_prefix);
                        auto& registry_indexes = data_ptr->callback_data.registry_indexes;
                        for (auto& registry_index : registry_indexes)
                        {
                            if (post_id == registry_index.second.identifier)
                            {
                                registry_index.second.lua_index = -1;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    std::string error_message{"Was unable to unregister a hook with Lua function 'UnregisterHook', information:\n"};
                    error_message.append(fmt::format("FunctionName: {}\n", to_string(function_name_no_prefix)));
                    error_message.append(fmt::format("UFunction::Func: {}\n", std::bit_cast<void*>(func_ptr)));
                    error_message.append(fmt::format("ProcessInternal: {}\n", Unreal::UObject::ProcessInternalInternal.get_function_address()));
                    error_message.append(
                            fmt::format("FUNC_Native: {}\n", static_cast<uint32_t>(unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))));
                    lua.throw_error(error_message);
                }

                return 0;
            });

            lua.register_function("DumpAllObjects", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                const Mod* mod = get_mod_ref(lua);
                if (!mod)
                {
                    lua.throw_error("Couldn't dump objects and properties because the pointer to 'Mod' was nullptr");
                }
                UE4SSProgram::dump_all_objects_and_properties(mod->m_program.get_object_dumper_output_directory() + STR("\\") +
                                                              UE4SSProgram::m_object_dumper_file_name);
                return 0;
            });

            lua.register_function("GenerateSDK", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                const Mod* mod = get_mod_ref(lua);
                if (!mod)
                {
                    lua.throw_error("Couldn't generate SDK because the pointer to 'Mod' was nullptr");
                }
                File::StringType working_dir{mod->m_program.get_working_directory()};
                mod->m_program.generate_cxx_headers(working_dir + STR("\\CXXHeaderDump"));
                return 0;
            });

            lua.register_function("GenerateLuaTypes", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                const Mod* mod = get_mod_ref(lua);
                if (!mod)
                {
                    lua.throw_error("Couldn't generate lua types because the pointer to 'Mod' was nullptr");
                }
                File::StringType working_dir{mod->m_program.get_working_directory()};
                UE4SSProgram::get_program().generate_lua_types(working_dir + STR("\\Mods\\shared\\types"));
                return 0;
            });

            lua.register_function("GenerateUHTCompatibleHeaders", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                const Mod* mod = get_mod_ref(lua);
                mod->m_program.generate_uht_compatible_headers();
                return 0;
            });

            lua.register_function("DumpStaticMeshes", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                GUI::Dumpers::call_generate_static_mesh_file();
                return 0;
            });

            lua.register_function("DumpAllActors", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                GUI::Dumpers::call_generate_all_actor_file();
                return 0;
            });

            lua.register_function("DumpUSMAP", []([[maybe_unused]] const LuaMadeSimple::Lua& lua) -> int {
                OutTheShade::generate_usmap();
                return 0;
            });
        }

        lua.register_function("StaticConstructObject", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'StaticConstructObject'.
Overloads:
#1: StaticConstructObject(
                            UClass Class,
                            UObject Outer,
                            FName Name, #Optional
                            EObjectFlags Flags, #Optional
                            EInternalObjectFlags InternalSetFlags, #Optional
                            bool CopyTransientsFromClassDefaults, #Optional
                            bool AssumeTemplateIsArchetype, #Optional
                            UObject Template, #Optional
                            FObjectInstancingGraph InstanceGraph, #Optional
                            UPackage ExternalPackage, #Optional
                            void SubobjectOverrides #Optional

))"};

            // For now, we're assuming that if there's userdata, that userdata is of the correct underlying type
            if (!lua.is_userdata())
            {
                lua.throw_error(error_overload_not_found);
            }
            Unreal::UClass* param_class = lua.get_userdata<LuaType::UClass>().get_remote_cpp_object();

            if (!lua.is_userdata())
            {
                lua.throw_error(error_overload_not_found);
            }
            Unreal::UObject* param_outer = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            Unreal::FName param_name;
            if (lua.is_userdata())
            {
                param_name = lua.get_userdata<LuaType::FName>().get_local_cpp_object();
            }
            else if (lua.is_string())
            {
                // Support passing a string directly as the Name parameter
                auto name_str = lua.get_string();
                std::wstring wide_name(name_str.begin(), name_str.end());
                param_name = Unreal::FName(wide_name.c_str());
            }
            else if (lua.is_integer())
            {
                param_name = Unreal::FName(lua.get_integer());
            }
            else
            {
                param_name = Unreal::FName(static_cast<int64_t>(0));
            }

            Unreal::EObjectFlags param_set_flags{};
            if (lua.is_integer())
            {
                param_set_flags = static_cast<Unreal::EObjectFlags>(lua.get_integer());
            }

            Unreal::EInternalObjectFlags param_internal_set_flags{};
            if (lua.is_integer())
            {
                param_internal_set_flags = static_cast<Unreal::EInternalObjectFlags>(lua.get_integer());
            }

            // The rest are all optional parameters
            bool param_copy_transients_from_class_defaults{};
            if (lua.is_bool())
            {
                param_copy_transients_from_class_defaults = lua.get_bool();
            }

            bool param_assume_template_is_archetype{};
            if (lua.is_bool())
            {
                param_assume_template_is_archetype = lua.get_bool();
            }

            Unreal::UObject* param_template{};
            if (lua.is_userdata())
            {
                param_template = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            }

            // Change this to userdata if support for 'FObjectInstancingGraph' is ever added
            void* param_instance_graph{};
            if (lua.is_integer())
            {
                param_instance_graph = reinterpret_cast<void*>(static_cast<uintptr_t>(lua.get_integer()));
            }

            // Change this to userdata if support for 'UPackage' is ever added
            void* param_external_package{};
            if (lua.is_integer())
            {
                param_external_package = reinterpret_cast<void*>(static_cast<uintptr_t>(lua.get_integer()));
            }

            // void* param_subobject_overrides{};
            // if (lua.is_integer()) { param_subobject_overrides = reinterpret_cast<void*>(static_cast<uintptr_t>(lua.get_integer())); }

            Unreal::FStaticConstructObjectParameters params{param_class, param_outer};
            params.Name = param_name;
            params.SetFlags = param_set_flags;
            params.InternalSetFlags = param_internal_set_flags;
            params.bCopyTransientsFromClassDefaults = param_copy_transients_from_class_defaults;
            params.bAssumeTemplateIsArchetype = param_assume_template_is_archetype;
            params.Template = param_template;
            params.InstanceGraph = static_cast<struct RC::Unreal::FObjectInstancingGraph*>(param_instance_graph);
            params.ExternalPackage = static_cast<class RC::Unreal::UPackage*>(param_external_package);
            Unreal::UObject* created_object = Unreal::UObjectGlobals::StaticConstructObject(params);

            LuaType::UObject::construct(lua, created_object);

            return 1;
        });

        // VeinCF: Register a loaded UObject into VeinAssetManager's PreloadedAssets TMap
        // Usage: RegisterPreloadedAsset(assetTypeName, assetName, uobject)
        //   e.g. RegisterPreloadedAsset("BaseRecipe", "CR_QuickKindling", recipeObj)
        lua.register_function("RegisterPreloadedAsset", [](const LuaMadeSimple::Lua& lua) -> int {
            int32_t stack_size = lua.get_stack_size();
            if (stack_size < 3)
            {
                lua.throw_error("RegisterPreloadedAsset requires 3 args: assetTypeName (string), assetName (string), object (UObject)");
            }

            // P1: asset type name (e.g. "BaseRecipe")
            if (!lua.is_string()) lua.throw_error("RegisterPreloadedAsset P1 must be a string (asset type name)");
            auto asset_type_str = ensure_str(lua.get_string());

            // P2: asset name (e.g. "CR_QuickKindling")
            if (!lua.is_string()) lua.throw_error("RegisterPreloadedAsset P2 must be a string (asset name)");
            auto asset_name_str = ensure_str(lua.get_string());

            // P3: UObject*
            if (!lua.is_userdata()) lua.throw_error("RegisterPreloadedAsset P3 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!object)
            {
                lua.throw_error("RegisterPreloadedAsset: object is null");
            }

            // Find VeinAssetManager singleton
            Unreal::UObject* manager = Unreal::UObjectGlobals::FindFirstOf(STR("VeinAssetManager"));

            if (!manager)
            {
                Output::send(STR("[VeinCF] RegisterPreloadedAsset: VeinAssetManager not found\n"));
                lua.set_bool(false);
                return 1;
            }

            // Find the PreloadedAssets FMapProperty via reflection
            Unreal::FProperty* preloaded_prop = nullptr;
            for (auto* prop : manager->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == STR("PreloadedAssets"))
                {
                    preloaded_prop = prop;
                    break;
                }
            }

            if (!preloaded_prop)
            {
                Output::send(STR("[VeinCF] RegisterPreloadedAsset: PreloadedAssets property not found\n"));
                lua.set_bool(false);
                return 1;
            }

            // Get raw pointer to the TMap in the manager instance
            void* map_ptr = preloaded_prop->ContainerPtrToValuePtr<void>(manager);

            // Cast to typed TMap — layout matches TMap<FPrimaryAssetId, UObject*>
            auto* typed_map = reinterpret_cast<Unreal::TMap<Unreal::FPrimaryAssetId, Unreal::UObject*>*>(map_ptr);

            // Build the key
            Unreal::FPrimaryAssetId key;
            key.PrimaryAssetType.Name = Unreal::FName(asset_type_str.c_str());
            key.PrimaryAssetName = Unreal::FName(asset_name_str.c_str());

            // Add to map (replaces if key already exists)
            typed_map->Add(key, object);

            Output::send(STR("[VeinCF] RegisterPreloadedAsset: Added {}:{} -> {} (map now has {} entries)\n"),
                asset_type_str, asset_name_str, object->GetFullName(), typed_map->Num());

            lua.set_bool(true);
            return 1;
        });

        // VeinCF: Dump all keys from VeinAssetManager's PreloadedAssets TMap
        // Usage: local keys = GetPreloadedAssetKeys() -- returns array of "Type:Name" strings
        lua.register_function("GetPreloadedAssetKeys", [](const LuaMadeSimple::Lua& lua) -> int {
            Unreal::UObject* manager = Unreal::UObjectGlobals::FindFirstOf(STR("VeinAssetManager"));
            if (!manager)
            {
                Output::send(STR("[VeinCF] GetPreloadedAssetKeys: VeinAssetManager not found\n"));
                lua.set_nil();
                return 1;
            }

            Unreal::FProperty* preloaded_prop = nullptr;
            for (auto* prop : manager->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == STR("PreloadedAssets"))
                {
                    preloaded_prop = prop;
                    break;
                }
            }

            if (!preloaded_prop)
            {
                Output::send(STR("[VeinCF] GetPreloadedAssetKeys: PreloadedAssets not found\n"));
                lua.set_nil();
                return 1;
            }

            void* map_ptr = preloaded_prop->ContainerPtrToValuePtr<void>(manager);
            auto* typed_map = reinterpret_cast<Unreal::TMap<Unreal::FPrimaryAssetId, Unreal::UObject*>*>(map_ptr);

            auto* L = lua.get_lua_state();
            lua_newtable(L);
            int idx = 1;
            for (auto it = typed_map->begin(); it != typed_map->end(); ++it)
            {
                auto type_str = it->Key.PrimaryAssetType.Name.ToString();
                auto name_str = it->Key.PrimaryAssetName.ToString();
                auto combined = to_string(type_str) + ":" + to_string(name_str);
                lua_pushinteger(L, idx++);
                lua_pushstring(L, combined.c_str());
                lua_settable(L, -3);
            }

            Output::send(STR("[VeinCF] GetPreloadedAssetKeys: returned {} keys\n"), idx - 1);
            return 1;
        });

        // VeinCF: Add a FPrimaryAssetId to a TArray<FPrimaryAssetId> property on any UObject
        // Usage: AddPrimaryAssetIdToArray(uobject, "PropertyName", "TypeName", "AssetName")
        // Used for: PlayerCharacterData.Recipes, or any Array<Struct<PrimaryAssetId>>
        lua.register_function("AddPrimaryAssetIdToArray", [](const LuaMadeSimple::Lua& lua) -> int {
            int32_t stack_size = lua.get_stack_size();
            if (stack_size < 4)
            {
                lua.throw_error("AddPrimaryAssetIdToArray requires 4 args: uobject, propertyName, typeName, assetName");
            }

            // P1: UObject*
            if (!lua.is_userdata()) lua.throw_error("AddPrimaryAssetIdToArray P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("AddPrimaryAssetIdToArray: object is null");

            // P2: property name
            if (!lua.is_string()) lua.throw_error("AddPrimaryAssetIdToArray P2 must be a string (property name)");
            auto prop_name_str = ensure_str(lua.get_string());

            // P3: type name (e.g. "BaseRecipe")
            if (!lua.is_string()) lua.throw_error("AddPrimaryAssetIdToArray P3 must be a string (type name)");
            auto type_name_str = ensure_str(lua.get_string());

            // P4: asset name (e.g. "CR_QuickKindling")
            if (!lua.is_string()) lua.throw_error("AddPrimaryAssetIdToArray P4 must be a string (asset name)");
            auto asset_name_str = ensure_str(lua.get_string());

            // Find the property
            Unreal::FProperty* target_prop = nullptr;
            for (auto* prop : object->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == prop_name_str)
                {
                    target_prop = prop;
                    break;
                }
            }

            if (!target_prop)
            {
                Output::send(STR("[VeinCF] AddPrimaryAssetIdToArray: property '{}' not found\n"), prop_name_str);
                lua.set_bool(false);
                return 1;
            }

            // Get raw pointer to the TArray in the object
            void* array_ptr = target_prop->ContainerPtrToValuePtr<void>(object);
            auto* typed_array = reinterpret_cast<Unreal::TArray<Unreal::FPrimaryAssetId>*>(array_ptr);

            // Build the new entry
            Unreal::FPrimaryAssetId new_id;
            new_id.PrimaryAssetType.Name = Unreal::FName(type_name_str.c_str());
            new_id.PrimaryAssetName = Unreal::FName(asset_name_str.c_str());

            // Check for duplicates
            bool already_exists = false;
            for (int32_t i = 0; i < typed_array->Num(); i++)
            {
                if ((*typed_array)[i] == new_id)
                {
                    already_exists = true;
                    break;
                }
            }

            if (already_exists)
            {
                Output::send(STR("[VeinCF] AddPrimaryAssetIdToArray: {}:{} already in array '{}'\n"),
                    type_name_str, asset_name_str, prop_name_str);
                lua.set_bool(true);
                return 1;
            }

            typed_array->Add(new_id);

            Output::send(STR("[VeinCF] AddPrimaryAssetIdToArray: added {}:{} to '{}' (now {} entries)\n"),
                type_name_str, asset_name_str, prop_name_str, typed_array->Num());

            lua.set_bool(true);
            return 1;
        });

        // VeinCF: Remove a FPrimaryAssetId from a TArray<FPrimaryAssetId> property
        // Usage: RemovePrimaryAssetIdFromArray(uobject, "PropertyName", "TypeName", "AssetName")
        lua.register_function("RemovePrimaryAssetIdFromArray", [](const LuaMadeSimple::Lua& lua) -> int {
            int32_t stack_size = lua.get_stack_size();
            if (stack_size < 4)
            {
                lua.throw_error("RemovePrimaryAssetIdFromArray requires 4 args: uobject, propertyName, typeName, assetName");
            }

            if (!lua.is_userdata()) lua.throw_error("RemovePrimaryAssetIdFromArray P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("RemovePrimaryAssetIdFromArray: object is null");

            if (!lua.is_string()) lua.throw_error("RemovePrimaryAssetIdFromArray P2 must be a string");
            auto prop_name_str = ensure_str(lua.get_string());

            if (!lua.is_string()) lua.throw_error("RemovePrimaryAssetIdFromArray P3 must be a string");
            auto type_name_str = ensure_str(lua.get_string());

            if (!lua.is_string()) lua.throw_error("RemovePrimaryAssetIdFromArray P4 must be a string");
            auto asset_name_str = ensure_str(lua.get_string());

            Unreal::FProperty* target_prop = nullptr;
            for (auto* prop : object->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == prop_name_str)
                {
                    target_prop = prop;
                    break;
                }
            }

            if (!target_prop)
            {
                Output::send(STR("[VeinCF] RemovePrimaryAssetIdFromArray: property '{}' not found\n"), prop_name_str);
                lua.set_bool(false);
                return 1;
            }

            void* array_ptr = target_prop->ContainerPtrToValuePtr<void>(object);
            auto* typed_array = reinterpret_cast<Unreal::TArray<Unreal::FPrimaryAssetId>*>(array_ptr);

            Unreal::FPrimaryAssetId target_id;
            target_id.PrimaryAssetType.Name = Unreal::FName(type_name_str.c_str());
            target_id.PrimaryAssetName = Unreal::FName(asset_name_str.c_str());

            bool removed = false;
            for (int32_t i = typed_array->Num() - 1; i >= 0; i--)
            {
                if ((*typed_array)[i] == target_id)
                {
                    typed_array->RemoveAt(i);
                    removed = true;
                    break;
                }
            }

            Output::send(STR("[VeinCF] RemovePrimaryAssetIdFromArray: {} {}:{} from '{}'\n"),
                removed ? STR("removed") : STR("not found"), type_name_str, asset_name_str, prop_name_str);

            lua.set_bool(removed);
            return 1;
        });

        // VeinCF: Read all FPrimaryAssetId entries from a TArray<FPrimaryAssetId> property
        // Usage: local entries = GetPrimaryAssetIdArray(uobject, "PropertyName")
        //   returns array of {Type="BaseRecipe", Name="CR_Kindling"} tables
        lua.register_function("GetPrimaryAssetIdArray", [](const LuaMadeSimple::Lua& lua) -> int {
            int32_t stack_size = lua.get_stack_size();
            if (stack_size < 2)
            {
                lua.throw_error("GetPrimaryAssetIdArray requires 2 args: uobject, propertyName");
            }

            if (!lua.is_userdata()) lua.throw_error("GetPrimaryAssetIdArray P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("GetPrimaryAssetIdArray: object is null");

            if (!lua.is_string()) lua.throw_error("GetPrimaryAssetIdArray P2 must be a string");
            auto prop_name_str = ensure_str(lua.get_string());

            Unreal::FProperty* target_prop = nullptr;
            for (auto* prop : object->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == prop_name_str)
                {
                    target_prop = prop;
                    break;
                }
            }

            if (!target_prop)
            {
                Output::send(STR("[VeinCF] GetPrimaryAssetIdArray: property '{}' not found\n"), prop_name_str);
                lua.set_nil();
                return 1;
            }

            void* array_ptr = target_prop->ContainerPtrToValuePtr<void>(object);
            auto* typed_array = reinterpret_cast<Unreal::TArray<Unreal::FPrimaryAssetId>*>(array_ptr);

            auto* L = lua.get_lua_state();
            lua_newtable(L);
            for (int32_t i = 0; i < typed_array->Num(); i++)
            {
                auto& entry = (*typed_array)[i];
                lua_pushinteger(L, i + 1);
                lua_newtable(L);

                lua_pushstring(L, to_string(entry.PrimaryAssetType.Name.ToString()).c_str());
                lua_setfield(L, -2, "Type");

                lua_pushstring(L, to_string(entry.PrimaryAssetName.ToString()).c_str());
                lua_setfield(L, -2, "Name");

                lua_settable(L, -3);
            }

            return 1;
        });

        // VeinCF: Look up a UObject* from PreloadedAssets by type+name
        // Usage: local obj = GetPreloadedAsset("BaseRecipe", "CR_Kindling")
        lua.register_function("GetPreloadedAsset", [](const LuaMadeSimple::Lua& lua) -> int {
            int32_t stack_size = lua.get_stack_size();
            if (stack_size < 2)
            {
                lua.throw_error("GetPreloadedAsset requires 2 args: typeName, assetName");
            }

            if (!lua.is_string()) lua.throw_error("GetPreloadedAsset P1 must be a string");
            auto type_str = ensure_str(lua.get_string());

            if (!lua.is_string()) lua.throw_error("GetPreloadedAsset P2 must be a string");
            auto name_str = ensure_str(lua.get_string());

            Unreal::UObject* manager = Unreal::UObjectGlobals::FindFirstOf(STR("VeinAssetManager"));
            if (!manager)
            {
                lua.set_nil();
                return 1;
            }

            Unreal::FProperty* preloaded_prop = nullptr;
            for (auto* prop : manager->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop->GetFName().ToString() == STR("PreloadedAssets"))
                {
                    preloaded_prop = prop;
                    break;
                }
            }

            if (!preloaded_prop)
            {
                lua.set_nil();
                return 1;
            }

            void* map_ptr = preloaded_prop->ContainerPtrToValuePtr<void>(manager);
            auto* typed_map = reinterpret_cast<Unreal::TMap<Unreal::FPrimaryAssetId, Unreal::UObject*>*>(map_ptr);

            Unreal::FPrimaryAssetId key;
            key.PrimaryAssetType.Name = Unreal::FName(type_str.c_str());
            key.PrimaryAssetName = Unreal::FName(name_str.c_str());

            auto* found = typed_map->Find(key);
            if (found && *found)
            {
                LuaType::auto_construct_object(lua, *found);
            }
            else
            {
                lua.set_nil();
            }

            return 1;
        });

        // VeinCF: Dump all FProperty and UFunction names on a UObject's class chain
        // Usage: DumpClassProperties(uobject) -> returns table of {name=propName, type=propType, class=className}
        lua.register_function("DumpClassProperties", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1 || !lua.is_userdata())
            {
                lua.throw_error("DumpClassProperties requires 1 arg: UObject");
            }

            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object)
            {
                lua.throw_error("DumpClassProperties: object is null");
            }

            Unreal::UClass* cls = object->GetClassPrivate();
            lua.set_nil(); // clear stack

            // Create results table
            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            // Walk the class hierarchy
            Unreal::UClass* current = cls;
            while (current)
            {
                auto class_name = current->GetFName().ToString();

                // Walk FProperty chain
                for (auto* prop : current->ForEachPropertyInChain())
                {
                    if (!prop) continue;

                    auto prop_name = to_string(prop->GetFName().ToString());
                    auto prop_class_variant = prop->GetClass();
                    auto prop_type = to_string(prop_class_variant.GetFName().ToString());

                    lua_newtable(lua.get_lua_state());

                    lua_pushstring(lua.get_lua_state(), prop_name.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "name");

                    lua_pushstring(lua.get_lua_state(), prop_type.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "type");

                    lua_pushstring(lua.get_lua_state(), to_string(class_name).c_str());
                    lua_setfield(lua.get_lua_state(), -2, "class");

                    lua_rawseti(lua.get_lua_state(), table_idx, entry++);
                }

                // Walk UFunctions via ForEachFunction
                for (auto* func : current->ForEachFunction())
                {
                    if (!func) continue;

                    auto func_name = to_string(func->GetFName().ToString());

                    lua_newtable(lua.get_lua_state());

                    lua_pushstring(lua.get_lua_state(), func_name.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "name");

                    lua_pushstring(lua.get_lua_state(), "Function");
                    lua_setfield(lua.get_lua_state(), -2, "type");

                    lua_pushstring(lua.get_lua_state(), to_string(class_name).c_str());
                    lua_setfield(lua.get_lua_state(), -2, "class");

                    lua_rawseti(lua.get_lua_state(), table_idx, entry++);
                }

                // Stop at UObject
                auto super = current->GetSuperClass();
                if (!super || super->GetFName().ToString() == STR("Object")) break;
                current = super;
            }

            Output::send(STR("[VeinCF] DumpClassProperties: {} entries for {}\n"), entry - 1, cls->GetFName().ToString());
            return 1;
        });

        // VeinCF: Authoritative reflected-field offsets, read straight from the UClass.
        // Offsets are a property of the class, identical for every instance — this needs
        // NO live instance state and never touches game-owned memory. Use it to anchor
        // reflected fields, then separate them from native (non-reflected) fields found
        // empirically via DumpObjectMemoryDeep.
        // Usage: DumpClassPropertyOffsets(uobject)
        //   -> table of {name, type, class, offset, size}
        lua.register_function("DumpClassPropertyOffsets", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1 || !lua.is_userdata())
            {
                lua.throw_error("DumpClassPropertyOffsets requires 1 arg: UObject");
            }

            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("DumpClassPropertyOffsets: object is null");

            Unreal::UClass* cls = object->GetClassPrivate();
            lua.set_nil();

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            Unreal::UClass* current = cls;
            while (current)
            {
                auto class_name = to_string(current->GetFName().ToString());

                for (auto* prop : current->ForEachPropertyInChain())
                {
                    if (!prop) continue;

                    auto prop_name = to_string(prop->GetFName().ToString());
                    auto prop_type = to_string(prop->GetClass().GetFName().ToString());
                    int32_t offset = prop->GetOffset_Internal();
                    int32_t size = prop->GetSize();

                    lua_newtable(lua.get_lua_state());

                    lua_pushstring(lua.get_lua_state(), prop_name.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "name");

                    lua_pushstring(lua.get_lua_state(), prop_type.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "type");

                    lua_pushstring(lua.get_lua_state(), class_name.c_str());
                    lua_setfield(lua.get_lua_state(), -2, "class");

                    lua_pushinteger(lua.get_lua_state(), offset);
                    lua_setfield(lua.get_lua_state(), -2, "offset");

                    lua_pushinteger(lua.get_lua_state(), size);
                    lua_setfield(lua.get_lua_state(), -2, "size");

                    lua_rawseti(lua.get_lua_state(), table_idx, entry++);
                }

                auto super = current->GetSuperClass();
                if (!super || super->GetFName().ToString() == STR("Object")) break;
                current = super;
            }

            // Also report total instance size of the most-derived class — native fields
            // live between the last reflected offset and (roughly) this boundary.
            int32_t props_size = cls->GetPropertiesSize();
            Output::send(STR("[VeinCF] DumpClassPropertyOffsets: {} props for {}, PropertiesSize={}\n"),
                         entry - 1, cls->GetFName().ToString(), props_size);

            lua_pushinteger(lua.get_lua_state(), props_size);
            return 2; // table, propertiesSize
        });

        // VeinCF: Master class index — walk all of GUObjectArray and count instances
        // per class. The starting map of every system that exists at runtime.
        // Usage: EnumerateClasses() -> table of {class=name, count=N}, totalObjects
        lua.register_function("EnumerateClasses", [](const LuaMadeSimple::Lua& lua) -> int {
            std::unordered_map<Unreal::UClass*, int> counts;
            int64_t total = 0;

            Unreal::UObjectGlobals::ForEachUObject([&](void* raw, int32_t, int32_t) {
                Unreal::UObject* obj = static_cast<Unreal::UObject*>(raw);
                if (!obj) return RC::LoopAction::Continue;
                Unreal::UClass* cls = nullptr;
                // valid live objects from GUObjectArray — ClassPrivate is safe, but guard anyway
                __try { cls = obj->GetClassPrivate(); } __except(EXCEPTION_EXECUTE_HANDLER) { cls = nullptr; }
                if (cls)
                {
                    ++counts[cls];
                    ++total;
                }
                return RC::LoopAction::Continue;
            });

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;
            for (auto& [cls, count] : counts)
            {
                std::wstring cname;
                if (!veincf_safe_object_name(cls, cname)) continue;

                lua_newtable(lua.get_lua_state());
                auto u8 = to_string(cname);
                lua_pushstring(lua.get_lua_state(), u8.c_str());
                lua_setfield(lua.get_lua_state(), -2, "class");
                lua_pushinteger(lua.get_lua_state(), count);
                lua_setfield(lua.get_lua_state(), -2, "count");
                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] EnumerateClasses: {} classes, {} total objects\n"), entry - 1, total);
            lua_pushinteger(lua.get_lua_state(), static_cast<lua_Integer>(total));
            return 2; // table, totalObjects
        });

        // VeinCF: Recursive object-graph mapper. From a root UObject, follow every
        // strong AND weak reference up to `depth` hops, deduped by pointer, bounded by
        // `maxNodes`. Emits the EDGE LIST of how systems link — the tool that auto-finds
        // SHARED components between systems (the whole point of mass datamining).
        // Each node is scanned up to `scanBytes`, SEH-guarded per slot so varied-size
        // objects never fault. Returns a flat edge table for the Lua side to serialize.
        // Usage: DeepMapGraph(rootObj, depth, maxNodes, scanBytes)
        //   -> table of {fromClass, fromPtr, offset, kind, toClass, toPtr, depth}
        lua.register_function("DeepMapGraph", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 4)
            {
                lua.throw_error("DeepMapGraph requires 4 args: rootObj, depth, maxNodes, scanBytes");
            }

            // Read args by ABSOLUTE index via the raw C API (no pop ambiguity):
            //   1 = rootObj (userdata), 2 = depth, 3 = maxNodes, 4 = scanBytes
            lua_State* L = lua.get_lua_state();
            int64_t max_depth = static_cast<int64_t>(lua_tointeger(L, 2));
            int64_t max_nodes = static_cast<int64_t>(lua_tointeger(L, 3));
            int64_t scan_bytes = static_cast<int64_t>(lua_tointeger(L, 4));
            if (!lua.is_userdata(1)) lua.throw_error("DeepMapGraph P1 must be a UObject");
            Unreal::UObject* root = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            if (!root) lua.throw_error("DeepMapGraph: root is null");

            if (scan_bytes < 8) scan_bytes = 8;
            if (scan_bytes > 8192) scan_bytes = 8192;
            if (max_depth < 1) max_depth = 1;
            if (max_depth > 8) max_depth = 8;
            if (max_nodes < 1) max_nodes = 1;
            if (max_nodes > 200000) max_nodes = 200000;

            // Build the whole edge list as ONE TSV string (no giant Lua table — that
            // exhausts Lua memory at scale). Lua writes the returned string to a file.
            std::string out;
            out.reserve(1u << 22); // 4 MB to start
            out += "fromClass\tfromPtr\toffset\tkind\ttoClass\ttoPtr\tdepth\n";
            int64_t edge_count = 0;

            std::unordered_set<uintptr_t> visited;
            std::vector<std::pair<Unreal::UObject*, int>> queue; // (node, depth)
            queue.push_back({root, 0});
            visited.insert(reinterpret_cast<uintptr_t>(root));
            size_t qi = 0;

            char linebuf[64];

            while (qi < queue.size() && static_cast<int64_t>(visited.size()) <= max_nodes)
            {
                Unreal::UObject* node = queue[qi].first;
                int node_depth = queue[qi].second;
                ++qi;

                std::wstring from_class;
                veincf_safe_class_name(node, from_class);
                auto from_u8 = to_string(from_class);
                snprintf(linebuf, sizeof(linebuf), "0x%012llX", (unsigned long long)reinterpret_cast<uintptr_t>(node));
                std::string from_hex = linebuf;

                uint8_t* base = reinterpret_cast<uint8_t*>(node);
                for (int64_t offset = 0; offset <= scan_bytes - 8; offset += 8)
                {
                    uintptr_t val = 0;
                    if (!veincf_safe_read_u64(base + offset, &val))
                        break; // walked off the end of this object's mapped memory

                    Unreal::UObject* child = nullptr;
                    const char* kind = nullptr;

                    Unreal::UObject* strong = nullptr;
                    if (veincf_probe_uobject(val, &strong) && strong)
                    {
                        child = strong; kind = "strong";
                    }
                    else
                    {
                        Unreal::UObject* weak = veincf_resolve_weak(base + offset);
                        Unreal::UObject* wp = nullptr;
                        if (weak && veincf_probe_uobject(reinterpret_cast<uintptr_t>(weak), &wp) && wp)
                        {
                            child = wp; kind = "weak";
                        }
                    }

                    if (!child) continue;

                    std::wstring to_class;
                    veincf_safe_class_name(child, to_class);
                    auto to_u8 = to_string(to_class);

                    // Append a TSV line
                    out += from_u8;
                    out += '\t';
                    out += from_hex;
                    snprintf(linebuf, sizeof(linebuf), "\t%lld\t", (long long)offset);
                    out += linebuf;
                    out += kind;
                    out += '\t';
                    out += to_u8;
                    out += '\t';
                    snprintf(linebuf, sizeof(linebuf), "0x%012llX\t%d\n",
                             (unsigned long long)reinterpret_cast<uintptr_t>(child), node_depth);
                    out += linebuf;
                    ++edge_count;

                    // Enqueue child if unseen and within depth
                    uintptr_t cp = reinterpret_cast<uintptr_t>(child);
                    if (node_depth + 1 < max_depth && visited.find(cp) == visited.end()
                        && static_cast<int64_t>(visited.size()) < max_nodes)
                    {
                        visited.insert(cp);
                        queue.push_back({child, node_depth + 1});
                    }
                }
            }

            Output::send(STR("[VeinCF] DeepMapGraph: {} nodes visited, {} edges\n"), visited.size(), edge_count);
            lua_pushlstring(lua.get_lua_state(), out.data(), out.size());
            lua_pushinteger(lua.get_lua_state(), static_cast<lua_Integer>(visited.size()));
            lua_pushinteger(lua.get_lua_state(), static_cast<lua_Integer>(edge_count));
            return 3; // tsvString, nodeCount, edgeCount
        });

        // VeinCF: Resolve the object referenced at obj+offset (strong OR weak handle)
        // into a usable Lua UObject we can call methods on. Probe-validated before
        // construction, so non-object slots return nil instead of crashing. This is how
        // we pull a MAPPED native handle (e.g. workbench +1016 CraftingComponent) into
        // Lua so it can be passed to functions like SetRecipe.
        // Usage: GetObjectAt(uobject, offset) -> UObject or nil
        lua.register_function("GetObjectAt", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2) lua.throw_error("GetObjectAt requires 2 args: UObject, offset");
            lua_State* L = lua.get_lua_state();
            int64_t offset = static_cast<int64_t>(lua_tointeger(L, 2));
            if (!lua.is_userdata(1)) lua.throw_error("GetObjectAt P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            if (!object) lua.throw_error("GetObjectAt: object is null");
            if (offset < 0 || offset > 8192) lua.throw_error("GetObjectAt: offset out of range");

            uint8_t* slot = reinterpret_cast<uint8_t*>(object) + offset;
            uintptr_t val = 0;
            Unreal::UObject* resolved = nullptr;
            if (veincf_safe_read_u64(slot, &val))
            {
                Unreal::UObject* probed = nullptr;
                if (veincf_probe_uobject(val, &probed) && probed)
                {
                    resolved = probed; // strong pointer
                }
                else
                {
                    Unreal::UObject* weak = veincf_resolve_weak(slot);
                    Unreal::UObject* wp = nullptr;
                    if (weak && veincf_probe_uobject(reinterpret_cast<uintptr_t>(weak), &wp) && wp)
                        resolved = wp; // weak handle
                }
            }

            LuaType::auto_construct_object(lua, resolved); // nullptr -> nil
            return 1;
        });

        // VeinCF: Deep-copy every reflected property from src -> dest (same/compatible
        // class). Effectively StaticDuplicateObject for data objects: arrays and nested
        // structs are deep-copied (CopyCompleteValue), object refs stay shared. Lets us
        // make a genuinely NEW recipe = construct fresh BaseRecipe + CopyAllProperties
        // from a template, then modify, register, rebuild.
        // Usage: CopyAllProperties(destObj, srcObj) -> count of properties copied
        lua.register_function("CopyAllProperties", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2) lua.throw_error("CopyAllProperties requires 2 args: dest, src");
            if (!lua.is_userdata(1)) lua.throw_error("CopyAllProperties P1 (dest) must be a UObject");
            if (!lua.is_userdata(2)) lua.throw_error("CopyAllProperties P2 (src) must be a UObject");
            Unreal::UObject* dest = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            Unreal::UObject* src  = lua.get_userdata<LuaType::UObject>(2, true).get_remote_cpp_object();
            if (!dest || !src) lua.throw_error("CopyAllProperties: null object");

            Unreal::UClass* cls = dest->GetClassPrivate();
            if (!cls) lua.throw_error("CopyAllProperties: dest has no class");

            int copied = 0;
            for (auto* prop : cls->ForEachPropertyInChain())
            {
                if (!prop) continue;
                // Skip the UberGraphFrame and similar transient runtime props
                auto pname = prop->GetFName().ToString();
                if (pname == STR("UberGraphFrame")) continue;
                void* destPtr = prop->ContainerPtrToValuePtr<void>(dest);
                const void* srcPtr = prop->ContainerPtrToValuePtr<void>(src);
                if (!destPtr || !srcPtr) continue;
                prop->CopyCompleteValue(destPtr, srcPtr);
                ++copied;
            }

            Output::send(STR("[VeinCF] CopyAllProperties: copied {} properties\n"), copied);
            lua_pushinteger(lua.get_lua_state(), copied);
            return 1;
        });

        // VeinCF: Dump a UFunction's parameter signature (name, type, return/out flags)
        // plus NumParms / ParmsSize / ReturnValueOffset. Lets us learn how to call any
        // game function correctly. Read-only reflection, safe.
        // Usage: DumpFunctionParams(obj, "FuncName") -> table {name,type,isReturn,isOut}, numParms, returnOffset
        lua.register_function("DumpFunctionParams", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2 || !lua.is_userdata(1)) lua.throw_error("DumpFunctionParams requires (obj, funcName)");
            lua_State* L = lua.get_lua_state();
            Unreal::UObject* obj = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            const char* fn = lua_tostring(L, 2);
            if (!obj || !fn) lua.throw_error("DumpFunctionParams: null arg");
            std::string s(fn); std::wstring wname(s.begin(), s.end());

            Unreal::UFunction* func = nullptr;
            for (auto* f : obj->GetClassPrivate()->ForEachFunction())
            {
                if (f && f->GetFName().ToString() == wname) { func = f; break; }
            }
            if (!func) lua.throw_error("DumpFunctionParams: function not found");

            lua_newtable(L);
            int tbl = lua_gettop(L);
            int entry = 1;
            for (auto* prop : func->ForEachProperty())
            {
                if (!prop) continue;
                if (!prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_Parm)) continue;
                auto pname = to_string(prop->GetFName().ToString());
                auto ptype = to_string(prop->GetClass().GetFName().ToString());
                bool isReturn = prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_ReturnParm);
                bool isOut = prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_OutParm);

                lua_newtable(L);
                lua_pushstring(L, pname.c_str()); lua_setfield(L, -2, "name");
                lua_pushstring(L, ptype.c_str()); lua_setfield(L, -2, "type");
                lua_pushboolean(L, isReturn ? 1 : 0); lua_setfield(L, -2, "isReturn");
                lua_pushboolean(L, isOut ? 1 : 0); lua_setfield(L, -2, "isOut");
                lua_rawseti(L, tbl, entry++);
            }

            lua_pushinteger(L, func->GetNumParms());
            lua_pushinteger(L, func->GetReturnValueOffset());
            Output::send(STR("[VeinCF] DumpFunctionParams: {} params for {}\n"), entry - 1, wname);
            return 3; // table, numParms, returnValueOffset
        });

        // VeinCF: Find the real native method that a UFunction's exec-thunk calls.
        // The thunk (UFunction->Func) reads params then CALLs the actual C++ method;
        // native callers (e.g. the workbench) invoke that method directly, bypassing
        // the thunk -> to hook them we need the real method's address. This scans the
        // thunk for E8 call-rel32 instructions and returns their absolute targets.
        // Usage: ScanThunkCalls(obj, "FuncName") -> thunkAddr(hex), table of {offset, target(hex)}
        lua.register_function("ScanThunkCalls", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2 || !lua.is_userdata(1)) lua.throw_error("ScanThunkCalls requires (obj, funcName)");
            lua_State* L = lua.get_lua_state();
            Unreal::UObject* obj = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            const char* fn = lua_tostring(L, 2);
            if (!obj || !fn) lua.throw_error("ScanThunkCalls: null arg");
            std::string s(fn); std::wstring wname(s.begin(), s.end());

            Unreal::UFunction* func = nullptr;
            for (auto* f : obj->GetClassPrivate()->ForEachFunction())
            {
                if (f && f->GetFName().ToString() == wname) { func = f; break; }
            }
            if (!func) lua.throw_error("ScanThunkCalls: function not found");

            void* thunk = reinterpret_cast<void*>(func->GetFuncPtr());
            char hexbuf[32];
            snprintf(hexbuf, sizeof(hexbuf), "0x%016llX", (unsigned long long)reinterpret_cast<uintptr_t>(thunk));
            lua_pushstring(L, hexbuf);

            int32_t offsets[64];
            uintptr_t targets[64];
            int count = veincf_scan_calls(reinterpret_cast<uint8_t*>(thunk), 256, offsets, targets, 64);

            lua_newtable(L);
            int tbl = lua_gettop(L);
            int entry = 1;
            for (int i = 0; i < count; ++i)
            {
                lua_newtable(L);
                lua_pushinteger(L, offsets[i]); lua_setfield(L, -2, "offset");
                char tb[32]; snprintf(tb, sizeof(tb), "0x%016llX", (unsigned long long)targets[i]);
                lua_pushstring(L, tb); lua_setfield(L, -2, "target");
                lua_rawseti(L, tbl, entry++);
            }

            Output::send(STR("[VeinCF] ScanThunkCalls: {} calls in thunk for {}\n"), count, wname);
            return 2; // thunkAddr(hex), table of calls
        });

        // VeinCF: Native disassembler. Decode `count` instructions starting at a
        // hex address (e.g. the real method addr from ScanThunkCalls). Each entry
        // = {addr, offset, len, text} where text is the formatted Intel asm
        // ("mov rax, [rcx+0x4F0]", "call 0x00007FF..."). Reading a function's body
        // reveals the EXACT member offsets it accesses + the functions it calls —
        // no more guessing native structure from memory dumps. SEH-guarded per
        // instruction (stops cleanly if it runs off into unmapped code).
        // Usage: DisasmFunction(addrHex, count) -> table of {addr, offset, len, text}
        lua.register_function("DisasmFunction", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2) lua.throw_error("DisasmFunction requires (addrHex, count)");
            lua_State* L = lua.get_lua_state();
            const char* addr_str = lua_tostring(L, 1);
            if (!addr_str) lua.throw_error("DisasmFunction: P1 must be a hex address string");
            int count = (int)lua_tointeger(L, 2);
            if (count < 1) count = 1;
            if (count > 512) count = 512;

            uintptr_t start = (uintptr_t)strtoull(addr_str, nullptr, 16);
            if (!start) lua.throw_error("DisasmFunction: address parsed to 0");

            lua_newtable(L);
            int tbl = lua_gettop(L);
            int entry = 1;
            uintptr_t cur = start;
            for (int i = 0; i < count; ++i)
            {
                VeincfDisasmOut out;
                if (!veincf_disasm_one(cur, &out) || !out.ok || out.length == 0) break;

                lua_newtable(L);
                char ab[32]; snprintf(ab, sizeof(ab), "0x%016llX", (unsigned long long)cur);
                lua_pushstring(L, ab); lua_setfield(L, -2, "addr");
                lua_pushinteger(L, (lua_Integer)(cur - start)); lua_setfield(L, -2, "offset");
                lua_pushinteger(L, (lua_Integer)out.length); lua_setfield(L, -2, "len");
                lua_pushstring(L, out.text); lua_setfield(L, -2, "text");
                lua_rawseti(L, tbl, entry++);

                cur += out.length;
                // stop at a return (ret / retn) — end of the function body
                if (out.text[0] == 'r' && out.text[1] == 'e' && out.text[2] == 't') break;
            }
            Output::send(STR("[VeinCF] DisasmFunction: decoded {} instrs from 0x{:X}\n"), entry - 1, (unsigned long long)start);
            return 1;
        });

        // VeinCF: SEH-safe read of 8 bytes at any address (hex string in, hex
        // string out, or nil on fault). Lets Lua follow raw pointer chains —
        // read [obj] -> vtable, read [vtable+0xNNN] -> virtual fn addr, etc. —
        // which combined with DisasmFunction lets us walk + read any native code.
        // Usage: ReadU64(addrHex) -> valueHex(string) | nil
        lua.register_function("ReadU64", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1) lua.throw_error("ReadU64 requires (addrHex)");
            lua_State* L = lua.get_lua_state();
            const char* s = lua_tostring(L, 1);
            if (!s) lua.throw_error("ReadU64: P1 must be a hex address string");
            uintptr_t addr = (uintptr_t)strtoull(s, nullptr, 16);
            uintptr_t val = 0;
            if (addr && veincf_safe_read_u64(reinterpret_cast<void*>(addr), &val))
            {
                char hb[32]; snprintf(hb, sizeof(hb), "0x%016llX", (unsigned long long)val);
                lua_pushstring(L, hb);
            }
            else
            {
                lua_pushnil(L);
            }
            return 1;
        });

        // VeinCF: Resolve a raw FName ComparisonIndex -> display string. The
        // universal "name a raw FName" verb: combined with ReadU64 it labels any
        // FName-keyed structure read from memory (e.g. AssetTypeMap keys) without
        // reflection or the unreliable Lua FName(int) ctor. SEH-safe.
        // Usage: ResolveFName(index | "0xhex") -> string | nil
        lua.register_function("ResolveFName", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 1) lua.throw_error("ResolveFName requires (comparisonIndex)");
            int32_t idx;
            if (lua_isstring(L, 1) && !lua_isnumber(L, 1))
                idx = (int32_t)strtoull(lua_tostring(L, 1), nullptr, 16);
            else
                idx = (int32_t)lua_tointeger(L, 1);
            std::wstring s;
            if (veincf_safe_fname_string(idx, s))
                lua_pushstring(L, to_string(s).c_str());
            else
                lua_pushnil(L);
            return 1;
        });

        // VeinCF: Read an FName at a memory address {int32 ComparisonIndex, int32
        // Number} and resolve it to its display string. SEH-safe read + resolve in
        // one hop — the keystone for walking FName-keyed memory (TMap keys, struct
        // members). Returns the base name (Number suffix omitted; 0 for type keys).
        // Usage: ReadFNameAt(addrHex) -> string | nil
        lua.register_function("ReadFNameAt", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 1) lua.throw_error("ReadFNameAt requires (addrHex)");
            const char* sa = lua_tostring(L, 1);
            if (!sa) lua.throw_error("ReadFNameAt: P1 must be a hex address string");
            uintptr_t addr = (uintptr_t)strtoull(sa, nullptr, 16);
            uintptr_t raw = 0;
            if (addr && veincf_safe_read_u64(reinterpret_cast<void*>(addr), &raw))
            {
                int32_t comp = (int32_t)(raw & 0xFFFFFFFF);
                std::wstring s;
                if (veincf_safe_fname_string(comp, s))
                {
                    lua_pushstring(L, to_string(s).c_str());
                    return 1;
                }
            }
            lua_pushnil(L);
            return 1;
        });

        // VeinCF: Call a native function by address (MS x64 ABI, up to 6 int/ptr
        // args). Each arg may be a hex string ("0x...") or a number (e.g. a
        // UObject's GetAddress(), or a constructed struct pointer). Returns the
        // result (rax) as a hex string, or nil if the call faulted. MUST be
        // invoked on the game thread for anything touching game state.
        // Usage: CallNative(fnAddrHex, arg1, arg2, ...) -> resultHex(string) | nil
        lua.register_function("CallNative", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            int n = lua.get_stack_size();
            if (n < 1) lua.throw_error("CallNative requires (addrHex, [args...])");

            auto as_uptr = [L](int idx) -> uintptr_t {
                if (lua_isstring(L, idx) && !lua_isnumber(L, idx))
                    return (uintptr_t)strtoull(lua_tostring(L, idx), nullptr, 16);
                return (uintptr_t)(unsigned long long)lua_tointeger(L, idx);
            };

            uintptr_t fn = as_uptr(1);
            if (!fn) lua.throw_error("CallNative: function address is 0");

            int argc = n - 1;
            if (argc < 0) argc = 0;
            if (argc > 6) argc = 6;
            uintptr_t a[6] = {0,0,0,0,0,0};
            for (int i = 0; i < argc; ++i) a[i] = as_uptr(i + 2);

            uintptr_t out = 0;
            bool ok = veincf_call_native(fn, argc, a, &out);
            if (ok)
            {
                char hb[32]; snprintf(hb, sizeof(hb), "0x%016llX", (unsigned long long)out);
                lua_pushstring(L, hb);
            }
            else
            {
                lua_pushnil(L);
            }
            return 1;
        });

        // VeinCF: FRONT-DOOR CONTENT MERGE. Load a cooked AssetRegistry.bin via
        // FAssetRegistryState::LoadFromDisk, then IAssetRegistry::AppendState it into the
        // live registry -> our pak's assets become ON-DISK registry entries (which the
        // primary-asset scan accepts; in-memory ones are filtered by bIncludeOnlyOnDiskAssets).
        // loadFromDiskHex = resolved static fn; ifaceThisHex = ar+0x28 (IAssetRegistry sub-object);
        // appendStateFnHex = the AppendState fn read from the interface vtable. Buffers are zeroed
        // static (one-shot startup merge; intentionally not destructed).
        // Usage: MergeRegistryFromDisk(loadFromDiskHex, ifaceThisHex, appendStateFnHex, binPath) -> bool
        lua.register_function("MergeRegistryFromDisk", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 4)
                lua.throw_error("MergeRegistryFromDisk(loadFromDiskHex, ifaceThisHex, appendStateFnHex, binPath)");
            uintptr_t loadFn   = (uintptr_t)strtoull(lua_tostring(L, 1), nullptr, 16);
            uintptr_t iface    = (uintptr_t)strtoull(lua_tostring(L, 2), nullptr, 16);
            uintptr_t appendFn = (uintptr_t)strtoull(lua_tostring(L, 3), nullptr, 16);
            const char* binPath = lua_tostring(L, 4);
            if (!loadFn || !iface || !appendFn || !binPath) lua.throw_error("MergeRegistryFromDisk: bad arg");

            // generous zeroed buffers (empty TMaps/TArrays = valid empty state/options)
            static uint8_t s_state[0x1000];
            static uint8_t s_options[0x100];
            memset(s_state, 0, sizeof(s_state));
            memset(s_options, 0, sizeof(s_options));

            std::string p(binPath);
            std::wstring wp(p.begin(), p.end());

            bool loaded = veincf_call_loadfromdisk(loadFn, wp.c_str(), s_options, s_state);
            Output::send(STR("[VeinCF] LoadFromDisk('{}') -> loaded={}\n"), std::wstring(wp), (int)loaded);

            bool appended = false;
            if (loaded)
            {
                uintptr_t a[2] = { iface, reinterpret_cast<uintptr_t>(s_state) };
                uintptr_t out = 0;
                appended = veincf_call_native(appendFn, 2, a, &out);
                Output::send(STR("[VeinCF] AppendState -> ok={}\n"), (int)appended);
            }
            lua.set_bool(loaded && appended);
            return 1;
        });

        // VeinCF: Call a VIRTUAL method by vtable slot. vtable = *(void**)obj;
        // fn = *(void**)(vtable + slotOff); then fn(obj, args...). The universal verb for
        // engine INTERFACE methods (IAssetRegistry etc.) that aren't reflected or string-
        // resolvable but ARE virtual -> reachable via the vtable (NO string-wall). slotOff =
        // BYTE offset into the vtable. SEH-safe. MUST run on the game thread for game state.
        // Usage: CallVirtual(objHex|num, slotOffHex|num, [args...]) -> resultHex(string) | nil
        lua.register_function("CallVirtual", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            int n = lua.get_stack_size();
            if (n < 2) lua.throw_error("CallVirtual requires (objHex, slotOffHex, [args...])");
            auto as_uptr = [L](int idx) -> uintptr_t {
                if (lua_isstring(L, idx) && !lua_isnumber(L, idx))
                    return (uintptr_t)strtoull(lua_tostring(L, idx), nullptr, 16);
                return (uintptr_t)(unsigned long long)lua_tointeger(L, idx);
            };
            uintptr_t obj  = as_uptr(1);
            uintptr_t slot = as_uptr(2);
            if (!obj) lua.throw_error("CallVirtual: obj is 0");
            uintptr_t vtable = 0, fn = 0;
            if (!veincf_safe_read_u64(reinterpret_cast<void*>(obj), &vtable) || !vtable) { lua_pushnil(L); return 1; }
            if (!veincf_safe_read_u64(reinterpret_cast<void*>(vtable + slot), &fn) || !fn) { lua_pushnil(L); return 1; }
            int extra = n - 2; if (extra < 0) extra = 0; if (extra > 5) extra = 5;
            uintptr_t a[6] = {0,0,0,0,0,0}; a[0] = obj;                 // a[0] = this
            for (int i = 0; i < extra; ++i) a[i+1] = as_uptr(i + 3);
            uintptr_t out = 0;
            bool ok = veincf_call_native(fn, extra + 1, a, &out);
            if (ok) { char hb[32]; snprintf(hb, sizeof(hb), "0x%016llX", (unsigned long long)out); lua_pushstring(L, hb); }
            else lua_pushnil(L);
            return 1;
        });

        // VeinCF: FRONT-DOOR loader unlock. Call an IAssetRegistry scan virtual
        // (ScanPathsSynchronous / ScanFilesSynchronous) by vtable slot to force-rescan +
        // INDEX a content path — e.g. a freshly-mounted mod pak folder — so the path
        // becomes enumerable and the AssetManager scan then picks it up. Builds
        // TArray<FString>{path}, resolves vtable[slotOff], calls
        //   void (this, const TArray<FString>&, bool bForceRescan, bool bIgnoreDenyList).
        // Usage: ScanRegistryPaths(arHex, slotOffHex, pathStr, [bForceRescan=true]) -> bool
        lua.register_function("ScanRegistryPaths", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 3)
                lua.throw_error("ScanRegistryPaths(arHex, slotOffHex, pathStr, [bForceRescan])");
            uintptr_t ar   = (uintptr_t)strtoull(lua_tostring(L, 1), nullptr, 16);
            uintptr_t slot = (uintptr_t)strtoull(lua_tostring(L, 2), nullptr, 16);
            const char* ps = lua_tostring(L, 3);
            bool force = true;
            if (lua.get_stack_size() >= 4) force = (lua_toboolean(L, 4) != 0);
            if (!ar || !ps) lua.throw_error("ScanRegistryPaths: bad arg");

            uintptr_t vtable = 0, fn = 0;
            if (!veincf_safe_read_u64(reinterpret_cast<void*>(ar), &vtable) || !vtable) { lua.set_bool(false); return 1; }
            if (!veincf_safe_read_u64(reinterpret_cast<void*>(vtable + slot), &fn) || !fn) { lua.set_bool(false); return 1; }

            std::string pathStr(ps);
            std::wstring wpath(pathStr.begin(), pathStr.end());
            Unreal::TArray<Unreal::FString> paths;
            paths.Add(Unreal::FString(wpath.c_str()));

            bool ok = veincf_call_scan_vtbl(fn, reinterpret_cast<void*>(ar), &paths, force, false);
            Output::send(STR("[VeinCF] ScanRegistryPaths path='{}' force={} -> ok={}\n"),
                         std::wstring(wpath), (int)force, (int)ok);
            lua.set_bool(ok);
            return 1;
        });

        // VeinCF: Register a primary asset DIRECTLY into UAssetManager's per-type
        // AssetMap by calling the resolved TryUpdateCachedAssetData inserter with a
        // hand-built FPrimaryAssetId + minimal FAssetData. Only the reflected FName
        // head (PackageName@0x00, PackagePath@0x08, AssetName@0x10) + AssetClassPath
        // are written; everything from TagsAndValues onward stays ZERO (the engine
        // null-checks TaggedAssetBundles etc., so zeros are safe). classPathOff is the
        // byte offset of AssetClassPath within FAssetData (5.6=0x18, 5.0/5.1=0x20) —
        // passed from Lua so we can TUNE without a rebuild. allowDuplicates=true.
        // Usage: RegisterPrimaryAssetNative(inserterHex, amAddr, type, name, pkgName, pkgPath, classPkg, className, classPathOff) -> bool
        lua.register_function("RegisterPrimaryAssetNative", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 9)
                lua.throw_error("RegisterPrimaryAssetNative(inserterHex, amAddr, type, name, pkgName, pkgPath, classPkg, className, classPathOff)");

            uintptr_t fn = (uintptr_t)strtoull(lua_tostring(L, 1), nullptr, 16);
            uintptr_t am = (uintptr_t)(unsigned long long)lua_tointeger(L, 2);
            const char* typeName  = lua_tostring(L, 3);
            const char* assetName = lua_tostring(L, 4);
            const char* pkgName   = lua_tostring(L, 5);
            const char* pkgPath   = lua_tostring(L, 6);
            const char* classPkg  = lua_tostring(L, 7);
            const char* className = lua_tostring(L, 8);
            int classPathOff = (int)lua_tointeger(L, 9);
            if (!fn || !am || !typeName || !assetName || !pkgName) lua.throw_error("RegisterPrimaryAssetNative: bad arg");

            // FPrimaryAssetId key
            Unreal::FPrimaryAssetId id;
            id.PrimaryAssetType.Name = Unreal::FName(ensure_str(typeName).c_str(), Unreal::FNAME_Add);
            id.PrimaryAssetName      = Unreal::FName(ensure_str(assetName).c_str(), Unreal::FNAME_Add);

            // Minimal FAssetData on the stack (generously oversized + zeroed).
            alignas(8) uint8_t assetData[0xA0] = {0};
            uint8_t* p = assetData;
            *reinterpret_cast<Unreal::FName*>(p + 0x00) = Unreal::FName(ensure_str(pkgName).c_str(),   Unreal::FNAME_Add); // PackageName
            *reinterpret_cast<Unreal::FName*>(p + 0x08) = Unreal::FName(ensure_str(pkgPath).c_str(),   Unreal::FNAME_Add); // PackagePath
            *reinterpret_cast<Unreal::FName*>(p + 0x10) = Unreal::FName(ensure_str(assetName).c_str(), Unreal::FNAME_Add); // AssetName
            if (classPathOff > 0 && classPathOff + 16 <= (int)sizeof(assetData) && classPkg && className)
            {
                *reinterpret_cast<Unreal::FName*>(p + classPathOff + 0) = Unreal::FName(ensure_str(classPkg).c_str(),  Unreal::FNAME_Add); // AssetClassPath.PackageName
                *reinterpret_cast<Unreal::FName*>(p + classPathOff + 8) = Unreal::FName(ensure_str(className).c_str(), Unreal::FNAME_Add); // AssetClassPath.AssetName
            }

            bool result = false;
            bool ok = veincf_call_inserter(fn, reinterpret_cast<void*>(am), &id, assetData, true, &result);
            Output::send(STR("[VeinCF] RegisterPrimaryAssetNative {}:{} -> call_ok={} result={}\n"),
                ensure_str(typeName), ensure_str(assetName), (int)ok, (int)result);
            lua.set_bool(ok && result);
            return 1;
        });

        // VeinCF: Find code that references a string (ANSI or UTF-16) in the main
        // game module -> resolve a function by a unique log/ensure string it uses,
        // with no symbols. Returns {strHits=N, refs={{ref=hex, func=hex}, ...}}.
        // 'func' = best-guess function start (nearest preceding int3 pad). The
        // universal "locate function" verb; pairs with DisasmFunction + CallNative.
        // Usage: FindStringRefs("some unique string") -> table
        lua.register_function("FindStringRefs", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1) lua.throw_error("FindStringRefs requires (string)");
            lua_State* L = lua.get_lua_state();
            const char* s = lua_tostring(L, 1);
            if (!s) lua.throw_error("FindStringRefs: P1 must be a string");

            static uintptr_t refs[256]; static uintptr_t funcs[256];
            int strHits = 0;
            int n = veincf_find_string_refs(s, refs, funcs, 256, &strHits);

            lua_newtable(L);
            lua_pushinteger(L, (lua_Integer)strHits); lua_setfield(L, -2, "strHits");
            lua_newtable(L);
            int tbl = lua_gettop(L);
            for (int i = 0; i < n; ++i)
            {
                lua_newtable(L);
                char rb[32]; snprintf(rb, sizeof(rb), "0x%016llX", (unsigned long long)refs[i]);
                lua_pushstring(L, rb); lua_setfield(L, -2, "ref");
                char fb[32]; snprintf(fb, sizeof(fb), "0x%016llX", (unsigned long long)funcs[i]);
                lua_pushstring(L, fb); lua_setfield(L, -2, "func");
                lua_rawseti(L, tbl, i + 1);
            }
            lua_setfield(L, -2, "refs");
            Output::send(STR("[VeinCF] FindStringRefs: {} string hits, {} code refs\n"), strHits, n);
            return 1;
        });

        // VeinCF: find all CALL sites targeting a known function address (E8 rel32). The
        // by-address xref verb: resolve a function that has no string anchor by finding its
        // CALLERS from a callee we CAN resolve. Cracks the codeRefs=0 wide-string wall.
        // Usage: FindCallersOf(addrHex) -> {refs={{ref=hex, func=hex}, ...}}
        lua.register_function("FindCallersOf", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1) lua.throw_error("FindCallersOf requires (addrHex)");
            lua_State* L = lua.get_lua_state();
            const char* s = lua_tostring(L, 1);
            if (!s) lua.throw_error("FindCallersOf: P1 must be a hex address string");
            uintptr_t target = (uintptr_t)strtoull(s, nullptr, 16);
            if (!target) lua.throw_error("FindCallersOf: target address is 0");

            static uintptr_t refs[512]; static uintptr_t funcs[512];
            int n = veincf_find_callers(target, refs, funcs, 512);

            lua_newtable(L);
            lua_newtable(L);
            int tbl = lua_gettop(L);
            for (int i = 0; i < n; ++i)
            {
                lua_newtable(L);
                char rb[32]; snprintf(rb, sizeof(rb), "0x%016llX", (unsigned long long)refs[i]);
                lua_pushstring(L, rb); lua_setfield(L, -2, "ref");
                char fb[32]; snprintf(fb, sizeof(fb), "0x%016llX", (unsigned long long)funcs[i]);
                lua_pushstring(L, fb); lua_setfield(L, -2, "func");
                lua_rawseti(L, tbl, i + 1);
            }
            lua_setfield(L, -2, "refs");
            Output::send(STR("[VeinCF] FindCallersOf: {} call sites\n"), n);
            return 1;
        });

        // VeinCF: Call UAssetManager::ScanPathsForPrimaryAssets natively — the
        // engine's "scan a content path and register its assets as primary assets
        // of a type" verb. Builds FPrimaryAssetType(FName(typeName)) + a
        // TArray<FString>{pathStr} here (native, easy) and invokes the resolved
        // function. Returns the int32 result (# assets scanned), or -999 on fault.
        // Usage: ScanPathsForType(scanAddrHex, amAddr, typeName, pathStr, baseClassAddr)
        lua.register_function("ScanPathsForType", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            if (lua.get_stack_size() < 5)
                lua.throw_error("ScanPathsForType(scanAddrHex, amAddr, typeName, pathStr, baseClassAddr)");

            uintptr_t scanAddr  = (uintptr_t)strtoull(lua_tostring(L, 1), nullptr, 16);
            uintptr_t am        = (uintptr_t)(unsigned long long)lua_tointeger(L, 2);
            const char* tn      = lua_tostring(L, 3);
            const char* ps      = lua_tostring(L, 4);
            uintptr_t baseClass = (uintptr_t)(unsigned long long)lua_tointeger(L, 5);
            if (!scanAddr || !am || !tn || !ps) lua.throw_error("ScanPathsForType: bad arg");

            std::string typeName(tn), pathStr(ps);
            std::wstring wtype(typeName.begin(), typeName.end());
            std::wstring wpath(pathStr.begin(), pathStr.end());

            Unreal::FName typeFName(wtype.c_str(), Unreal::FNAME_Add);
            Unreal::TArray<Unreal::FString> paths;
            paths.Add(Unreal::FString(wpath.c_str()));

            // FPrimaryAssetType is just the FName, passed by value (low 8 bytes)
            uint64_t typeVal = *reinterpret_cast<uint64_t*>(&typeFName);

            int32_t result = veincf_call_scanpaths(reinterpret_cast<void*>(scanAddr),
                                                   reinterpret_cast<void*>(am), typeVal,
                                                   &paths, reinterpret_cast<void*>(baseClass));
            Output::send(STR("[VeinCF] ScanPathsForType('{}'): result={}\n"),
                         std::wstring(wtype), result);
            lua_pushinteger(L, result);
            return 1;
        });

        // VeinCF: dump every string constant (ANSI/UTF-16) in the module that
        // CONTAINS a substring -> read the REAL strings the binary uses (so we
        // anchor FindStringRefs on reality, not a guess). Returns {addr,text,wide}.
        // Usage: DumpStringsContaining("PrimaryAsset") -> table
        lua.register_function("DumpStringsContaining", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 1) lua.throw_error("DumpStringsContaining requires (substr)");
            lua_State* L = lua.get_lua_state();
            const char* sub = lua_tostring(L, 1);
            if (!sub) lua.throw_error("DumpStringsContaining: P1 must be a string");

            static VeincfStrHit hits[1024];
            int n = veincf_strings_containing(sub, hits, 1024);

            lua_newtable(L);
            int tbl = lua_gettop(L);
            int e = 1;
            for (int i = 0; i < n; ++i)
            {
                char buf[256];
                veincf_copy_str(reinterpret_cast<uint8_t*>(hits[i].addr), hits[i].len, hits[i].wide != 0, buf, sizeof(buf));
                lua_newtable(L);
                char ab[32]; snprintf(ab, sizeof(ab), "0x%016llX", (unsigned long long)hits[i].addr);
                lua_pushstring(L, ab);   lua_setfield(L, -2, "addr");
                lua_pushstring(L, buf);  lua_setfield(L, -2, "text");
                lua_pushinteger(L, hits[i].wide); lua_setfield(L, -2, "wide");
                lua_rawseti(L, tbl, e++);
            }
            Output::send(STR("[VeinCF] DumpStringsContaining: {} strings\n"), n);
            return 1;
        });

        // VeinCF: Binary-hook UVeinAssetManager::GetAllRecipes and inject a recipe into
        // every return. The workbench calls this natively, so this is the only way to
        // make a genuinely NEW recipe discoverable (no override, no registry surgery).
        // Resolves the real method addr (call nearest the thunk), installs a PolyHook
        // detour that appends our recipe to the returned TArray. Opt-in + idempotent.
        // Usage: HookGetAllRecipes(assetManager, recipeObj) -> ok(bool), realAddr(hex)
        lua.register_function("HookGetAllRecipes", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2 || !lua.is_userdata(1) || !lua.is_userdata(2))
                lua.throw_error("HookGetAllRecipes requires (assetManager, recipeObj)");
            lua_State* L = lua.get_lua_state();
            Unreal::UObject* am = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            Unreal::UObject* recipe = lua.get_userdata<LuaType::UObject>(2, true).get_remote_cpp_object();
            if (!am || !recipe) lua.throw_error("HookGetAllRecipes: null arg");

            Unreal::UFunction* func = nullptr;
            for (auto* f : am->GetClassPrivate()->ForEachFunction())
            {
                if (f && f->GetFName().ToString() == STR("GetAllRecipes")) { func = f; break; }
            }
            if (!func) lua.throw_error("HookGetAllRecipes: GetAllRecipes not found");

            // resolve the ReturnValue array property (once)
            if (!g_veincf_recipes_retprop)
            {
                for (auto* prop : func->ForEachProperty())
                {
                    if (prop && prop->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_ReturnParm))
                    {
                        if (prop->GetClass().GetFName().ToString() == STR("ArrayProperty"))
                            g_veincf_recipes_retprop = static_cast<Unreal::FArrayProperty*>(prop);
                        break;
                    }
                }
            }
            if (!g_veincf_recipes_retprop) lua.throw_error("HookGetAllRecipes: ReturnValue is not an ArrayProperty");

            // add recipe to the inject list (dedupe)
            bool already = false;
            for (auto* r : g_veincf_injected_recipes) if (r == recipe) already = true;
            if (!already) g_veincf_injected_recipes.push_back(recipe);

            if (g_veincf_getallrecipes_detour)
            {
                Output::send(STR("[VeinCF] HookGetAllRecipes: already hooked, inject list now {}\n"), g_veincf_injected_recipes.size());
                lua_pushboolean(L, 1);
                lua_pushstring(L, "already-installed");
                return 2;
            }

            // resolve the real native method = call target nearest the thunk
            uint8_t* thunk = reinterpret_cast<uint8_t*>(func->GetFuncPtr());
            int32_t offsets[64]; uintptr_t targets[64];
            int n = veincf_scan_calls(thunk, 256, offsets, targets, 64);
            if (n <= 0) lua.throw_error("HookGetAllRecipes: couldn't scan thunk");
            uintptr_t thunkAddr = reinterpret_cast<uintptr_t>(thunk);
            uintptr_t best = 0, bestDist = UINTPTR_MAX;
            for (int i = 0; i < n; ++i)
            {
                uintptr_t d = (targets[i] > thunkAddr) ? (targets[i] - thunkAddr) : (thunkAddr - targets[i]);
                if (d < bestDist) { bestDist = d; best = targets[i]; }
            }
            if (!best) lua.throw_error("HookGetAllRecipes: no candidate address");
            Output::send(STR("[VeinCF] HookGetAllRecipes: real method @ 0x{:X} (thunk 0x{:X}, dist 0x{:X})\n"), best, thunkAddr, bestDist);

            g_veincf_getallrecipes_detour = new PLH::x64Detour(
                static_cast<uint64_t>(best),
                reinterpret_cast<uint64_t>(&veincf_getallrecipes_hook),
                &g_veincf_getallrecipes_tramp);
            bool ok = false;
            try { ok = g_veincf_getallrecipes_detour->hook(); }
            catch (...) { ok = false; }
            Output::send(STR("[VeinCF] HookGetAllRecipes: hook installed = {}\n"), ok);

            char hb[32]; snprintf(hb, sizeof(hb), "0x%016llX", (unsigned long long)best);
            lua_pushboolean(L, ok ? 1 : 0);
            lua_pushstring(L, hb);
            return 2;
        });

        // VeinCF: how many times the GetAllRecipes detour has fired + how many entries
        // it appended on its last fire. Tells us if the WORKBENCH actually calls
        // GetAllRecipes (fires increase on SetWorkbench) or uses a cached list.
        // Usage: GetHookFireCount() -> fires, lastAppended
        lua.register_function("GetHookFireCount", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_pushinteger(lua.get_lua_state(), (lua_Integer)g_veincf_getallrecipes_fires);
            lua_pushinteger(lua.get_lua_state(), (lua_Integer)g_veincf_getallrecipes_lastappended);
            return 2;
        });

        // VeinCF: clear the recipe inject list (the detour stays installed).
        // Usage: ResetInjectList()
        lua.register_function("ResetInjectList", [](const LuaMadeSimple::Lua& lua) -> int {
            g_veincf_injected_recipes.clear();
            Output::send(STR("[VeinCF] ResetInjectList: cleared\n"));
            return 0;
        });

        // VeinCF: dump the current inject list (names + validity), DLL-side so it's
        // reliable regardless of Lua array-read quirks.
        // Usage: DumpInjectList() -> table of {name, valid}
        lua.register_function("DumpInjectList", [](const LuaMadeSimple::Lua& lua) -> int {
            lua_State* L = lua.get_lua_state();
            lua_newtable(L);
            int tbl = lua_gettop(L);
            int entry = 1;
            for (auto* rec : g_veincf_injected_recipes)
            {
                lua_newtable(L);
                Unreal::UObject* probed = nullptr;
                bool valid = (rec && veincf_probe_uobject(reinterpret_cast<uintptr_t>(rec), &probed) && probed);
                std::wstring nm;
                if (valid && veincf_safe_object_name(rec, nm))
                {
                    auto u8 = to_string(nm);
                    lua_pushstring(L, u8.c_str());
                }
                else lua_pushstring(L, "<invalid>");
                lua_setfield(L, -2, "name");
                lua_pushboolean(L, valid ? 1 : 0); lua_setfield(L, -2, "valid");
                lua_rawseti(L, tbl, entry++);
            }
            return 1;
        });

        // VeinCF: Append a UObject* to a TArray<Object> property (Lua can't grow arrays).
        // Used to inject into the game's cached recipe list once located.
        // Usage: ArrayAddObject(obj, "PropName", objToAdd) -> new element count
        lua.register_function("ArrayAddObject", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3) lua.throw_error("ArrayAddObject requires 3 args: obj, propName, objToAdd");
            lua_State* L = lua.get_lua_state();
            if (!lua.is_userdata(1)) lua.throw_error("ArrayAddObject P1 must be a UObject");
            Unreal::UObject* obj = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            const char* pn = lua_tostring(L, 2);
            if (!lua.is_userdata(3)) lua.throw_error("ArrayAddObject P3 must be a UObject");
            Unreal::UObject* toAdd = lua.get_userdata<LuaType::UObject>(3, true).get_remote_cpp_object();
            if (!obj || !toAdd || !pn) lua.throw_error("ArrayAddObject: null arg");
            std::string s(pn);
            std::wstring wname(s.begin(), s.end());

            Unreal::FProperty* found = nullptr;
            for (auto* prop : obj->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop && prop->GetFName().ToString() == wname) { found = prop; break; }
            }
            if (!found) lua.throw_error("ArrayAddObject: property not found");
            if (found->GetClass().GetFName().ToString() != STR("ArrayProperty"))
                lua.throw_error("ArrayAddObject: property is not an ArrayProperty");

            auto* arrProp = static_cast<Unreal::FArrayProperty*>(found);
            void* arrPtr = found->ContainerPtrToValuePtr<void>(obj);
            Unreal::FScriptArrayHelper helper(arrProp, arrPtr);
            int32_t newIdx = helper.AddValue();
            *reinterpret_cast<Unreal::UObject**>(helper.GetRawPtr(newIdx)) = toAdd;
            int32_t cnt = helper.Num();

            Output::send(STR("[VeinCF] ArrayAddObject: {} now {} elements\n"), wname, cnt);
            lua_pushinteger(L, cnt);
            return 1;
        });

        // VeinCF: Append a UObject to a TArray<FWeakObjectPtr> property (writes a weak
        // handle, not a raw pointer). Companion to ArrayAddObject for weak caches.
        // Usage: WeakArrayAddObject(obj, "PropName", objToAdd) -> new element count
        lua.register_function("WeakArrayAddObject", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3) lua.throw_error("WeakArrayAddObject requires 3 args: obj, propName, objToAdd");
            lua_State* L = lua.get_lua_state();
            if (!lua.is_userdata(1)) lua.throw_error("WeakArrayAddObject P1 must be a UObject");
            Unreal::UObject* obj = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            const char* pn = lua_tostring(L, 2);
            if (!lua.is_userdata(3)) lua.throw_error("WeakArrayAddObject P3 must be a UObject");
            Unreal::UObject* toAdd = lua.get_userdata<LuaType::UObject>(3, true).get_remote_cpp_object();
            if (!obj || !toAdd || !pn) lua.throw_error("WeakArrayAddObject: null arg");
            std::string s(pn);
            std::wstring wname(s.begin(), s.end());

            Unreal::FProperty* found = nullptr;
            for (auto* prop : obj->GetClassPrivate()->ForEachPropertyInChain())
            {
                if (prop && prop->GetFName().ToString() == wname) { found = prop; break; }
            }
            if (!found) lua.throw_error("WeakArrayAddObject: property not found");
            if (found->GetClass().GetFName().ToString() != STR("ArrayProperty"))
                lua.throw_error("WeakArrayAddObject: property is not an ArrayProperty");

            auto* arrProp = static_cast<Unreal::FArrayProperty*>(found);
            void* arrPtr = found->ContainerPtrToValuePtr<void>(obj);
            Unreal::FScriptArrayHelper helper(arrProp, arrPtr);
            int32_t newIdx = helper.AddValue();
            // construct a weak handle to toAdd and write it into the new slot
            Unreal::FWeakObjectPtr wp(toAdd);
            *reinterpret_cast<Unreal::FWeakObjectPtr*>(helper.GetRawPtr(newIdx)) = wp;
            int32_t cnt = helper.Num();

            Output::send(STR("[VeinCF] WeakArrayAddObject: {} now {} elements\n"), wname, cnt);
            lua_pushinteger(L, cnt);
            return 1;
        });

        // VeinCF: Sweep ALL of GUObjectArray and report which object-typed array
        // properties contain `target` (STRONG and WEAK arrays). Locates hidden
        // containers like the workbench's cached recipe list (find the array holding
        // a known recipe -> that's the cache).
        // SEH-guarded raw array reads; per-class property cache for speed; caps at 100 hits.
        // Usage: FindArraysContaining(targetObj)
        //   -> table of {ownerClass, ownerName, prop, count, ownerObj}
        lua.register_function("FindArraysContaining", [](const LuaMadeSimple::Lua& lua) -> int {
            if (!lua.is_userdata(1)) lua.throw_error("FindArraysContaining P1 must be a UObject");
            Unreal::UObject* target = lua.get_userdata<LuaType::UObject>(1, true).get_remote_cpp_object();
            if (!target) lua.throw_error("FindArraysContaining: null target");
            lua_State* L = lua.get_lua_state();

            lua_newtable(L);
            int tbl = lua_gettop(L);
            int entry = 1;
            int matches = 0;

            // per-class cache: each object-array property + whether it's a weak array
            struct ArrInfo { Unreal::FArrayProperty* prop; bool isWeak; };
            std::unordered_map<Unreal::UClass*, std::vector<ArrInfo>> cache;

            Unreal::UObjectGlobals::ForEachUObject([&](void* raw, [[maybe_unused]] int32_t a, [[maybe_unused]] int32_t b) -> RC::LoopAction {
                if (matches >= 100) return RC::LoopAction::Break;
                Unreal::UObject* o = static_cast<Unreal::UObject*>(raw);
                Unreal::UClass* cls = veincf_safe_getclass(o);
                if (!cls) return RC::LoopAction::Continue;

                auto it = cache.find(cls);
                if (it == cache.end())
                {
                    std::vector<ArrInfo> props;
                    for (auto* prop : cls->ForEachPropertyInChain())
                    {
                        if (!prop) continue;
                        if (prop->GetClass().GetFName().ToString() != STR("ArrayProperty")) continue;
                        auto* ap = static_cast<Unreal::FArrayProperty*>(prop);
                        auto* inner = ap->GetInner();
                        if (!inner) continue;
                        auto innerName = inner->GetClass().GetFName().ToString();
                        if (innerName == STR("ObjectProperty") || innerName == STR("ClassProperty"))
                            props.push_back({ap, false});
                        else if (innerName == STR("WeakObjectProperty"))
                            props.push_back({ap, true});
                    }
                    it = cache.emplace(cls, std::move(props)).first;
                }
                if (it->second.empty()) return RC::LoopAction::Continue;

                for (auto& ai : it->second)
                {
                    void* arrPtr = ai.prop->ContainerPtrToValuePtr<void>(o);
                    int32_t cnt = 0;
                    bool hit = ai.isWeak ? veincf_raw_weakarray_contains(arrPtr, target, &cnt)
                                         : veincf_raw_array_contains(arrPtr, target, &cnt);
                    if (hit)
                    {
                        std::wstring ownerClass, ownerName;
                        veincf_safe_class_name(o, ownerClass);
                        veincf_safe_object_name(o, ownerName);
                        auto propName = to_string(ai.prop->GetFName().ToString());

                        lua_newtable(L);
                        auto oc = to_string(ownerClass); lua_pushstring(L, oc.c_str()); lua_setfield(L, -2, "ownerClass");
                        auto on = to_string(ownerName);  lua_pushstring(L, on.c_str()); lua_setfield(L, -2, "ownerName");
                        lua_pushstring(L, propName.c_str()); lua_setfield(L, -2, "prop");
                        lua_pushinteger(L, cnt); lua_setfield(L, -2, "count");
                        lua_pushstring(L, ai.isWeak ? "weak" : "strong"); lua_setfield(L, -2, "kind");
                        LuaType::auto_construct_object(lua, o); lua_setfield(L, -2, "ownerObj");
                        lua_rawseti(L, tbl, entry++);
                        ++matches;
                    }
                }
                return RC::LoopAction::Continue;
            });

            Output::send(STR("[VeinCF] FindArraysContaining: {} matches\n"), matches);
            return 1;
        });

        // VeinCF: Scan raw object memory for UObject pointers
        // Usage: DumpObjectMemory(uobject, numBytes) -> table of {offset=N, pointer=hexstring, name=string|nil, class=string|nil}
        // Scans the raw memory of a UObject looking for valid UObject pointers.
        // Used to discover hidden (non-FProperty) member offsets.
        lua.register_function("DumpObjectMemory", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2)
            {
                lua.throw_error("DumpObjectMemory requires 2 args: UObject, numBytes");
            }

            // Get numBytes (arg 2, top of stack)
            if (!lua.is_number(-1)) lua.throw_error("DumpObjectMemory P2 must be a number (numBytes)");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            // Get object (arg 1)
            if (!lua.is_userdata()) lua.throw_error("DumpObjectMemory P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object)
            {
                lua.throw_error("DumpObjectMemory: object is null");
            }

            // Clamp scan range
            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 4096) num_bytes = 4096;

            uint8_t* base = reinterpret_cast<uint8_t*>(object);

            // Build the GUObjectArray range for validity checks
            // We'll use a simple heuristic: check if pointer looks like a valid UObject
            // by verifying its ClassPrivate is non-null and its name index is reasonable

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uintptr_t val = 0;
                if (!veincf_safe_read_u64(base + offset, &val)) break;

                Unreal::UObject* found = nullptr;
                if (!veincf_probe_uobject(val, &found) || !found)
                    continue;

                // Output offset + hex pointer only — no UObject push (crashes on edge cases)
                // Use ReadObjectAtOffset from Lua to safely fetch specific pointers
                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "pointer");

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] DumpObjectMemory: {} UObject pointers found in {} bytes\n"), entry - 1, num_bytes);
            return 1;
        });

        // VeinCF: Dump ALL raw 8-byte values from object memory (no UObject filtering)
        // Usage: DumpRawMemory(uobject, numBytes) -> table of {offset=N, hex=string}
        lua.register_function("DumpRawMemory", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2)
            {
                lua.throw_error("DumpRawMemory requires 2 args: UObject, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("DumpRawMemory P2 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("DumpRawMemory P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object)
            {
                lua.throw_error("DumpRawMemory: object is null");
            }

            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 4096) num_bytes = 4096;

            uint8_t* base = reinterpret_cast<uint8_t*>(object);

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uintptr_t val = 0;
                // SEH-safe: stop cleanly if we walk off the allocation into an unmapped page
                if (!veincf_safe_read_u64(base + offset, &val)) break;

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "hex");

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            return 1;
        });

        // VeinCF: Labeled memory map — every 8-byte slot with its offset, hex value,
        // and (if the slot holds a valid UObject) the resolved class name.
        // This is the primary tool for mapping NATIVE (non-FProperty) member fields.
        // Usage: DumpObjectMemoryLabeled(uobject, numBytes) -> table of {offset, hex, class?}
        lua.register_function("DumpObjectMemoryLabeled", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2)
            {
                lua.throw_error("DumpObjectMemoryLabeled requires 2 args: UObject, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("DumpObjectMemoryLabeled P2 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("DumpObjectMemoryLabeled P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("DumpObjectMemoryLabeled: object is null");

            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 4096) num_bytes = 4096;

            uint8_t* base = reinterpret_cast<uint8_t*>(object);

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uintptr_t val = 0;
                if (!veincf_safe_read_u64(base + offset, &val)) break;

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "hex");

                Unreal::UObject* found = nullptr;
                if (veincf_probe_uobject(val, &found) && found)
                {
                    std::wstring cname;
                    if (veincf_safe_class_name(found, cname))
                    {
                        auto u8 = to_string(cname);
                        lua_pushstring(lua.get_lua_state(), u8.c_str());
                        lua_setfield(lua.get_lua_state(), -2, "class");
                    }
                }

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] DumpObjectMemoryLabeled: mapped {} bytes\n"), num_bytes);
            return 1;
        });

        // VeinCF: Follow one pointer hop, then dump the target's labeled memory.
        // Reads the pointer at base+offset, validates it as a UObject, and maps it.
        // Lets us chain-walk native fields (workbench -> component -> inventory)
        // without ever constructing a Lua userdata (which can crash on bad pointers).
        // Usage: DumpPointerMemoryLabeled(uobject, offset, numBytes)
        //        -> targetClass (string or nil), table of {offset, hex, class?}
        lua.register_function("DumpPointerMemoryLabeled", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3)
            {
                lua.throw_error("DumpPointerMemoryLabeled requires 3 args: UObject, offset, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("DumpPointerMemoryLabeled P3 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_number(-1)) lua.throw_error("DumpPointerMemoryLabeled P2 must be a number");
            int64_t hop_offset = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("DumpPointerMemoryLabeled P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("DumpPointerMemoryLabeled: object is null");

            if (hop_offset < 0 || hop_offset > 4096) lua.throw_error("DumpPointerMemoryLabeled: offset out of range");
            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 4096) num_bytes = 4096;

            uintptr_t target_val = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(object) + hop_offset);

            Unreal::UObject* target = nullptr;
            if (!veincf_probe_uobject(target_val, &target) || !target)
            {
                // Not a valid UObject at that offset — return nil + empty table
                lua_pushnil(lua.get_lua_state());
                lua_newtable(lua.get_lua_state());
                return 2;
            }

            // Push the target's class name
            std::wstring tcname;
            if (veincf_safe_class_name(target, tcname))
            {
                auto u8 = to_string(tcname);
                lua_pushstring(lua.get_lua_state(), u8.c_str());
            }
            else
            {
                lua_pushnil(lua.get_lua_state());
            }

            // Now dump the target's labeled memory
            uint8_t* base = reinterpret_cast<uint8_t*>(target);
            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uintptr_t val = 0;
                if (!veincf_safe_read_u64(base + offset, &val)) break;

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "hex");

                Unreal::UObject* found = nullptr;
                if (veincf_probe_uobject(val, &found) && found)
                {
                    std::wstring cname;
                    if (veincf_safe_class_name(found, cname))
                    {
                        auto u8c = to_string(cname);
                        lua_pushstring(lua.get_lua_state(), u8c.c_str());
                        lua_setfield(lua.get_lua_state(), -2, "class");
                    }
                }

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] DumpPointerMemoryLabeled: followed +{}, mapped {} bytes\n"), hop_offset, num_bytes);
            return 2;
        });

        // VeinCF: Deep labeled map — tries each 8-byte slot as BOTH a strong
        // UObject* AND a TWeakObjectPtr {int32 index, int32 serial}. Weak/soft
        // references (common for components, owners, cached refs) are invisible to
        // a raw pointer scan because they look like small integers. This is the
        // master mapping tool: nothing referenced by the object stays hidden.
        // Usage: DumpObjectMemoryDeep(uobject, numBytes)
        //   -> table of {offset, hex, class? (strong), weak? (class via weakptr)}
        lua.register_function("DumpObjectMemoryDeep", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2)
            {
                lua.throw_error("DumpObjectMemoryDeep requires 2 args: UObject, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("DumpObjectMemoryDeep P2 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("DumpObjectMemoryDeep P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("DumpObjectMemoryDeep: object is null");

            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 8192) num_bytes = 8192;

            uint8_t* base = reinterpret_cast<uint8_t*>(object);

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uint8_t* slot = base + offset;
                uintptr_t val = 0;
                // SEH-safe: stop cleanly if we walk off the allocation into an unmapped page
                if (!veincf_safe_read_u64(slot, &val)) break;

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "hex");

                // Strong pointer attempt
                Unreal::UObject* strong = nullptr;
                if (veincf_probe_uobject(val, &strong) && strong)
                {
                    std::wstring cname;
                    if (veincf_safe_class_name(strong, cname))
                    {
                        auto u8 = to_string(cname);
                        lua_pushstring(lua.get_lua_state(), u8.c_str());
                        lua_setfield(lua.get_lua_state(), -2, "class");
                    }
                }
                else
                {
                    // Weak pointer attempt (only if it wasn't a strong pointer)
                    Unreal::UObject* weak = veincf_resolve_weak(slot);
                    Unreal::UObject* probed = nullptr;
                    if (weak && veincf_probe_uobject(reinterpret_cast<uintptr_t>(weak), &probed) && probed)
                    {
                        std::wstring wname;
                        if (veincf_safe_class_name(probed, wname))
                        {
                            auto u8 = to_string(wname);
                            lua_pushstring(lua.get_lua_state(), u8.c_str());
                            lua_setfield(lua.get_lua_state(), -2, "weak");
                        }
                    }
                }

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] DumpObjectMemoryDeep: mapped {} bytes (strong+weak)\n"), num_bytes);
            return 1;
        });

        // VeinCF: Follow a WEAK pointer at base+offset, then deep-map the target.
        // Companion to DumpPointerMemoryLabeled (which follows STRONG pointers).
        // Needed to walk into TWeakObjectPtr fields like CraftingComponent (+1016)
        // and WorkbenchInventoryComponent (+1864), which a strong-follow can't reach.
        // Usage: DumpWeakPointerMemoryDeep(uobject, offset, numBytes)
        //   -> targetClass (string|nil), table of {offset, hex, class?, weak?}
        lua.register_function("DumpWeakPointerMemoryDeep", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3)
            {
                lua.throw_error("DumpWeakPointerMemoryDeep requires 3 args: UObject, offset, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("DumpWeakPointerMemoryDeep P3 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_number(-1)) lua.throw_error("DumpWeakPointerMemoryDeep P2 must be a number");
            int64_t hop_offset = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("DumpWeakPointerMemoryDeep P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object) lua.throw_error("DumpWeakPointerMemoryDeep: object is null");

            if (hop_offset < 0 || hop_offset > 8192) lua.throw_error("DumpWeakPointerMemoryDeep: offset out of range");
            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 8192) num_bytes = 8192;

            uint8_t* slot = reinterpret_cast<uint8_t*>(object) + hop_offset;
            Unreal::UObject* target = veincf_resolve_weak(slot);
            Unreal::UObject* probed = nullptr;
            if (!target || !veincf_probe_uobject(reinterpret_cast<uintptr_t>(target), &probed) || !probed)
            {
                lua_pushnil(lua.get_lua_state());
                lua_newtable(lua.get_lua_state());
                return 2;
            }

            std::wstring tcname;
            if (veincf_safe_class_name(probed, tcname))
            {
                auto u8 = to_string(tcname);
                lua_pushstring(lua.get_lua_state(), u8.c_str());
            }
            else
            {
                lua_pushnil(lua.get_lua_state());
            }

            uint8_t* base = reinterpret_cast<uint8_t*>(probed);
            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uint8_t* s = base + offset;
                uintptr_t val = 0;
                if (!veincf_safe_read_u64(s, &val)) break;

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hex_buf[32];
                snprintf(hex_buf, sizeof(hex_buf), "0x%016llX", (unsigned long long)val);
                lua_pushstring(lua.get_lua_state(), hex_buf);
                lua_setfield(lua.get_lua_state(), -2, "hex");

                Unreal::UObject* strong = nullptr;
                if (veincf_probe_uobject(val, &strong) && strong)
                {
                    std::wstring cname;
                    if (veincf_safe_class_name(strong, cname))
                    {
                        auto u8c = to_string(cname);
                        lua_pushstring(lua.get_lua_state(), u8c.c_str());
                        lua_setfield(lua.get_lua_state(), -2, "class");
                    }
                }
                else
                {
                    Unreal::UObject* weak = veincf_resolve_weak(s);
                    Unreal::UObject* wp = nullptr;
                    if (weak && veincf_probe_uobject(reinterpret_cast<uintptr_t>(weak), &wp) && wp)
                    {
                        std::wstring wname;
                        if (veincf_safe_class_name(wp, wname))
                        {
                            auto u8w = to_string(wname);
                            lua_pushstring(lua.get_lua_state(), u8w.c_str());
                            lua_setfield(lua.get_lua_state(), -2, "weak");
                        }
                    }
                }

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] DumpWeakPointerMemoryDeep: followed weak +{}, mapped {} bytes\n"), hop_offset, num_bytes);
            return 2;
        });

        // VeinCF: Copy raw bytes from one UObject to another
        // Usage: CopyRawMemory(src, dst, startOffset, numBytes)
        // Copies raw memory from src+startOffset to dst+startOffset.
        // Use startOffset >= 48 to skip UObject header (vtable, name, outer, class, flags).
        lua.register_function("CopyRawMemory", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 4)
            {
                lua.throw_error("CopyRawMemory requires 4 args: src, dst, startOffset, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("CopyRawMemory P4 (numBytes) must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_number(-1)) lua.throw_error("CopyRawMemory P3 (startOffset) must be a number");
            int64_t start_offset = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("CopyRawMemory P2 (dst) must be a UObject");
            Unreal::UObject* dst = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!lua.is_userdata()) lua.throw_error("CopyRawMemory P1 (src) must be a UObject");
            Unreal::UObject* src = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!src || !dst)
            {
                lua.throw_error("CopyRawMemory: src and dst must be non-null");
            }
            if (start_offset < 0 || start_offset > 4096 || num_bytes < 0 || num_bytes > 4096)
            {
                lua.throw_error("CopyRawMemory: offsets out of range (0-4096)");
            }

            uint8_t* src_base = reinterpret_cast<uint8_t*>(src);
            uint8_t* dst_base = reinterpret_cast<uint8_t*>(dst);
            memcpy(dst_base + start_offset, src_base + start_offset, num_bytes);

            Output::send(STR("[VeinCF] CopyRawMemory: copied {} bytes at offset {}\n"), num_bytes, start_offset);
            lua_pushboolean(lua.get_lua_state(), 1);
            return 1;
        });

        // VeinCF: Write a raw 8-byte integer value at a byte offset in a UObject
        // Usage: WriteRawValueAtOffset(uobject, byteOffset, uint64Value) -> true
        lua.register_function("WriteRawValueAtOffset", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3)
            {
                lua.throw_error("WriteRawValueAtOffset requires 3 args: UObject, byteOffset, value");
            }

            if (!lua.is_number(-1)) lua.throw_error("WriteRawValueAtOffset P3 (value) must be a number");
            uint64_t value = static_cast<uint64_t>(lua.get_number(-1));

            if (!lua.is_number(-1)) lua.throw_error("WriteRawValueAtOffset P2 (byteOffset) must be a number");
            int64_t offset = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("WriteRawValueAtOffset P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!object)
            {
                lua.throw_error("WriteRawValueAtOffset: null object");
            }
            if (offset < 0 || offset > 4096)
            {
                lua.throw_error("WriteRawValueAtOffset: offset out of range (0-4096)");
            }

            uintptr_t base = reinterpret_cast<uintptr_t>(object);
            *reinterpret_cast<uint64_t*>(base + offset) = value;

            Output::send(STR("[VeinCF] WriteRawValueAtOffset: wrote 0x{:X} at offset {}\n"), value, offset);
            lua_pushboolean(lua.get_lua_state(), 1);
            return 1;
        });

        // VeinCF: Read a UObject* at a raw byte offset from another UObject
        // Usage: ReadObjectAtOffset(uobject, byteOffset) -> UObject or nil
        lua.register_function("ReadObjectAtOffset", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 2)
            {
                lua.throw_error("ReadObjectAtOffset requires 2 args: UObject, byteOffset");
            }

            if (!lua.is_number(-1)) lua.throw_error("ReadObjectAtOffset P2 must be a number");
            int64_t offset = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("ReadObjectAtOffset P1 must be a UObject");
            Unreal::UObject* object = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!object)
            {
                lua.throw_error("ReadObjectAtOffset: object is null");
            }

            if (offset < 0 || offset > 4096)
            {
                lua.throw_error("ReadObjectAtOffset: offset out of range (0-4096)");
            }

            uint8_t* base = reinterpret_cast<uint8_t*>(object);
            uintptr_t val = *reinterpret_cast<uintptr_t*>(base + offset);

            Unreal::UObject* result = nullptr;
            if (!veincf_probe_uobject(val, &result) || !result)
            {
                lua.set_nil();
                return 1;
            }

            LuaType::auto_construct_object(lua, result);
            return 1;
        });

        // VeinCF: Write a UObject* at a raw byte offset on another UObject
        // Usage: WriteObjectAtOffset(targetObj, byteOffset, valueObj)
        // Pass nil as valueObj to write null.
        lua.register_function("WriteObjectAtOffset", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3)
            {
                lua.throw_error("WriteObjectAtOffset requires 3 args: targetObj, byteOffset, valueObj");
            }

            // Arg 3: value (UObject or nil)
            Unreal::UObject* value = nullptr;
            if (lua.is_nil())
            {
                lua.discard_value();
            }
            else if (lua.is_userdata())
            {
                value = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            }
            else
            {
                lua.throw_error("WriteObjectAtOffset P3 must be a UObject or nil");
            }

            // Arg 2: offset
            if (!lua.is_number(-1)) lua.throw_error("WriteObjectAtOffset P2 must be a number");
            int64_t offset = static_cast<int64_t>(lua.get_number(-1));

            // Arg 1: target
            if (!lua.is_userdata()) lua.throw_error("WriteObjectAtOffset P1 must be a UObject");
            Unreal::UObject* target = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
            if (!target)
            {
                lua.throw_error("WriteObjectAtOffset: target is null");
            }

            if (offset < 0 || offset > 4096)
            {
                lua.throw_error("WriteObjectAtOffset: offset out of range (0-4096)");
            }

            uint8_t* base = reinterpret_cast<uint8_t*>(target);
            *reinterpret_cast<uintptr_t*>(base + offset) = reinterpret_cast<uintptr_t>(value);

            Output::send(STR("[VeinCF] WriteObjectAtOffset: wrote {} at offset {} on {}\n"),
                value ? to_wstring(value->GetFName().ToString()) : STR("null"),
                offset,
                to_wstring(target->GetFName().ToString()));

            lua.set_bool(true);
            return 1;
        });

        // VeinCF: Compare two UObjects' memory to find differing pointer-sized values
        // Usage: CompareObjectMemory(objA, objB, numBytes) -> table of {offset, valA, valB, nameA, nameB}
        // Useful for finding which offsets differ between a working and broken object of the same class.
        lua.register_function("CompareObjectMemory", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.get_stack_size() < 3)
            {
                lua.throw_error("CompareObjectMemory requires 3 args: objA, objB, numBytes");
            }

            if (!lua.is_number(-1)) lua.throw_error("CompareObjectMemory P3 must be a number");
            int64_t num_bytes = static_cast<int64_t>(lua.get_number(-1));

            if (!lua.is_userdata()) lua.throw_error("CompareObjectMemory P2 must be a UObject");
            Unreal::UObject* objB = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!lua.is_userdata()) lua.throw_error("CompareObjectMemory P1 must be a UObject");
            Unreal::UObject* objA = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();

            if (!objA) lua.throw_error("CompareObjectMemory: objA is null");
            if (!objB) lua.throw_error("CompareObjectMemory: objB is null");

            if (num_bytes < 8) num_bytes = 8;
            if (num_bytes > 4096) num_bytes = 4096;

            uint8_t* baseA = reinterpret_cast<uint8_t*>(objA);
            uint8_t* baseB = reinterpret_cast<uint8_t*>(objB);

            lua_newtable(lua.get_lua_state());
            int table_idx = lua_gettop(lua.get_lua_state());
            int entry = 1;

            for (int64_t offset = 0; offset <= num_bytes - 8; offset += 8)
            {
                uintptr_t valA = 0, valB = 0;
                if (!veincf_safe_read_u64(baseA + offset, &valA)) break;
                if (!veincf_safe_read_u64(baseB + offset, &valB)) break;

                if (valA == valB) continue; // Same value, skip

                lua_newtable(lua.get_lua_state());

                lua_pushinteger(lua.get_lua_state(), offset);
                lua_setfield(lua.get_lua_state(), -2, "offset");

                char hexA[32], hexB[32];
                snprintf(hexA, sizeof(hexA), "0x%016llX", (unsigned long long)valA);
                snprintf(hexB, sizeof(hexB), "0x%016llX", (unsigned long long)valB);
                lua_pushstring(lua.get_lua_state(), hexA);
                lua_setfield(lua.get_lua_state(), -2, "valA");
                lua_pushstring(lua.get_lua_state(), hexB);
                lua_setfield(lua.get_lua_state(), -2, "valB");

                // Push UObject refs for pointer-like values (Lua resolves names safely)
                // Mark whether each value looks like a UObject (for Lua to fetch via ReadObjectAtOffset)
                auto label_val = [&](uintptr_t val, const char* label_field) {
                    Unreal::UObject* found = nullptr;
                    if (val != 0 && val >= 0x10000 && val != 0xFFFFFFFFFFFFFFFF && veincf_probe_uobject(val, &found) && found)
                        lua_pushstring(lua.get_lua_state(), "uobject");
                    else
                        lua_pushstring(lua.get_lua_state(), val == 0 ? "null" : "not-uobject");
                    lua_setfield(lua.get_lua_state(), -2, label_field);
                };

                label_val(valA, "nameA");
                label_val(valB, "nameB");

                lua_rawseti(lua.get_lua_state(), table_idx, entry++);
            }

            Output::send(STR("[VeinCF] CompareObjectMemory: {} differences in {} bytes\n"), entry - 1, num_bytes);
            return 1;
        });

        lua.register_function("RegisterCustomProperty", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterCustomProperty'.
Overloads:
#1: RegisterCustomProperty(table PropertyInfo))"};

            if (!lua.is_table())
            {
                lua.throw_error(error_overload_not_found);
            }

            struct PropertyTypeInfo
            {
                std::string_view name{};
                int32_t size{-1};
                void* ffieldclass_pointer{};
                void* static_pointer{};

                auto is_valid() -> bool
                {
                    if (size < 0)
                    {
                        return false;
                    }
                    if (!ffieldclass_pointer)
                    {
                        return false;
                    }
                    // if (!static_pointer) { return false; }

                    return true;
                }
            };

            struct PropertyInfo
            {
                StringType name{};
                PropertyTypeInfo type{}; // Figure out what to do here, it shouldn't be just a string
                StringType belongs_to_class{};
                int32_t offset_internal{-1};
                int32_t element_size{-1}; // Is this required for trivial types like integers and floats ?

                // ArrayProperty
                PropertyTypeInfo array_inner{};

                bool offset_internal_is_table{};

                // Only one of these booleans can be true
                bool is_array_property{};

                auto set_is_array_property() -> void
                {
                    // Check here if any incompatible booleans have been set, and throw error if so
                    is_array_property = true;
                }

                auto is_missing_values() -> bool
                {
                    if (name.empty())
                    {
                        return true;
                    }
                    if (!type.is_valid())
                    {
                        return true;
                    }
                    if (belongs_to_class.empty())
                    {
                        return true;
                    }
                    if (!offset_internal_is_table && offset_internal < 0)
                    {
                        return true;
                    }
                    // if (element_size < 0) { return true; }

                    if (is_array_property && !array_inner.is_valid())
                    {
                        return true;
                    }

                    return false;
                }
            };

            PropertyInfo property_info{};

            auto lua_table = lua.get_table();

            auto verify_and_convert_int64_to_int32 = [&](std::string_view field_name,
                                                         std::string_view second_field_name = {},
                                                         std::string_view third_field_name = {},
                                                         bool* has_error = nullptr) -> int32_t {
                int64_t integer;

                if (second_field_name.empty())
                {
                    // Ignore the third field name if the second one isn't set
                    integer = lua_table.get_int_field(field_name, has_error);
                }
                else if (third_field_name.empty())
                {
                    // If the second field name is set but the third isn't, then we have two layers to the table
                    integer = lua_table.get_table_field(field_name, has_error).get_int_field(second_field_name, has_error);
                }
                else
                {
                    // If both the second field name and the third field name is set, then we have three layers to the table
                    integer = lua_table.get_table_field(field_name, has_error).get_table_field(second_field_name, has_error).get_int_field(third_field_name, has_error);
                }

                if (integer < std::numeric_limits<int32_t>::min() || integer > std::numeric_limits<int32_t>::max())
                {
                    std::string error_field_names;

                    if (second_field_name.empty())
                    {
                        error_field_names = fmt::format("{}", field_name);
                    }
                    else if (third_field_name.empty())
                    {
                        error_field_names = fmt::format("{}.{}", field_name, second_field_name);
                    }
                    else
                    {
                        error_field_names = fmt::format("{}.{}.{}", field_name, second_field_name, third_field_name);
                    }

                    lua.throw_error(fmt::format(
                            "Parameter #1 for function 'RegisterCustomProperty'. The table value for key '{}' is outside the range of a 32-bit integer",
                            error_field_names));
                }

                return static_cast<int32_t>(integer);
            };

            // Always required, for all property types
            property_info.name = ensure_str(lua_table.get_string_field("Name"));
            property_info.type.name = lua_table.get_table_field("Type").get_string_field("Name");
            property_info.type.size = verify_and_convert_int64_to_int32("Type", "Size");
            property_info.type.ffieldclass_pointer = reinterpret_cast<void*>(lua_table.get_table_field("Type").get_int_field("FFieldClassPointer"));
            property_info.type.static_pointer = reinterpret_cast<void*>(lua_table.get_table_field("Type").get_int_field("StaticPointer"));
            property_info.belongs_to_class = ensure_str(lua_table.get_string_field("BelongsToClass"));

            std::string oi_property_name;
            int32_t oi_relative_offset{};

            bool error_while_getting_offset_internal{};
            property_info.offset_internal = verify_and_convert_int64_to_int32("OffsetInternal", "", "", &error_while_getting_offset_internal);

            if (error_while_getting_offset_internal)
            {
                // Failed to get integer from table
                // This means that we may have a table instead of an integer

                oi_property_name = lua_table.get_table_field("OffsetInternal").get_string_field("Property");
                oi_relative_offset = verify_and_convert_int64_to_int32("OffsetInternal", "RelativeOffset");

                property_info.offset_internal_is_table = true;
            }

            // Only required for ArrayProperty
            if (property_info.type.name == "ArrayProperty")
            {
                if (!lua_table.does_field_exist("ArrayProperty"))
                {
                    lua.throw_error("Parameter #1 for function 'RegisterCustomProperty'. The table entry 'ArrayProperty' is missing.");
                }
                else
                {
                    property_info.set_is_array_property();
                    property_info.array_inner.name = lua_table.get_table_field("ArrayProperty").get_table_field("Type").get_string_field("Name");
                    property_info.array_inner.size = verify_and_convert_int64_to_int32("ArrayProperty", "Type", "Size");
                    property_info.array_inner.ffieldclass_pointer =
                            reinterpret_cast<void*>(lua_table.get_table_field("ArrayProperty").get_table_field("Type").get_int_field("FFieldClassPointer"));
                    property_info.array_inner.static_pointer =
                            reinterpret_cast<void*>(lua_table.get_table_field("ArrayProperty").get_table_field("Type").get_int_field("StaticPointer"));
                }
            }

            if (property_info.is_missing_values())
            {
                lua.throw_error("Parameter #1 for function 'RegisterCustomProperty'. The table is missing required fields.");
            }

            Unreal::UClass* belongs_to_class = Unreal::UObjectGlobals::StaticFindObject<Unreal::UClass*>(nullptr, nullptr, property_info.belongs_to_class);
            if (!belongs_to_class)
            {
                lua.throw_error("Tried to 'RegisterCustomProperty' but 'BelongsToClass' could not be found");
            }

            if (property_info.offset_internal_is_table)
            {
                auto name = Unreal::FName(ensure_str(oi_property_name));
                Unreal::FProperty* oi_property = belongs_to_class->FindProperty(name);
                if (!oi_property)
                {
                    lua.throw_error(fmt::format("Was unable to find property '{}' in class '{}' for use for relative Offset_Internal",
                                                oi_property_name,
                                                to_string(property_info.belongs_to_class)));
                }

                property_info.offset_internal = oi_property->GetOffset_Internal() + oi_relative_offset;
            }

            if (property_info.type.size == 0)
            {
                lua.throw_error(fmt::format("The size for property '{}' was unknown. Custom sizes are unsupported but will likely be supported in the future.",
                                            property_info.type.name));
            }

            if (property_info.is_array_property && property_info.array_inner.size == 0)
            {
                lua.throw_error(
                        fmt::format("The size for inner property '{}' was unknown. Custom sizes are unsupported but will likely be supported in the future.",
                                    property_info.array_inner.name));
            }

            LuaType::LuaCustomProperty::StaticStorage::property_list.add(
                    property_info.name,
                    Unreal::CustomArrayProperty::construct(property_info.offset_internal,
                                                           belongs_to_class,
                                                           static_cast<Unreal::UClass*>(property_info.type.ffieldclass_pointer),
                                                           static_cast<Unreal::FProperty*>(property_info.array_inner.ffieldclass_pointer),
                                                           property_info.is_array_property ? property_info.array_inner.size : property_info.type.size

                                                           ));

            printf_s("Registered Custom Property\n");
            printf_s("PropertyInfo {\n");
            printf_s("\tName: %S\n", FromCharTypePtr<wchar_t>(property_info.name.c_str()));
            printf_s("\tType {\n");
            printf_s("\t\tName: %s\n", property_info.type.name.data());
            printf_s("\t\tSize: 0x%X\n", property_info.type.size);
            printf_s("\t\tFFieldClassPointer: 0x%p\n", property_info.type.ffieldclass_pointer);
            printf_s("\t\tStaticPointer: 0x%p\n", property_info.type.static_pointer);
            printf_s("\t}\n");
            printf_s("\tBelongsToClass: %S\n", FromCharTypePtr<wchar_t>(property_info.belongs_to_class.c_str()));
            printf_s("\tOffsetInternal: 0x%X\n", property_info.offset_internal);

            if (property_info.is_array_property)
            {
                printf_s("\tArrayProperty {\n");
                printf_s("\t\tType {\n");
                printf_s("\t\t\tName: %s\n", property_info.array_inner.name.data());
                printf_s("\t\t\tSize: 0x%X\n", property_info.array_inner.size);
                printf_s("\t\t\tFFieldClassPointer: %p\n", property_info.array_inner.ffieldclass_pointer);
                printf_s("\t\t\tStaticPointer: %p\n", property_info.array_inner.static_pointer);
                printf_s("\t\t}\n");
                printf_s("\t}\n");
            }

            printf_s("}\n");

            return 0;
        });

        lua.register_function("ForEachUObject", [](const LuaMadeSimple::Lua& lua) -> int {
            Unreal::UObjectGlobals::ForEachUObject([&](void* object, int32_t chunk_index, int32_t object_index) {
                // Duplicate the Lua function so that we can use it in subsequent iterations of this loop (call_function pops the function from the stack)
                lua_pushvalue(lua.get_lua_state(), 1);

                // Set the 'Object' parameter for the Lua function (P1)
                LuaType::auto_construct_object(lua, static_cast<Unreal::UObject*>(object));

                // Set the 'ChunkIndex' parameter for the Lua function (P2)
                lua.set_integer(chunk_index);

                // Set the 'ObjectIndex' parameter for the Lua function (P3)
                lua.set_integer(object_index);

                lua.call_function(3, 1);

                return LoopAction::Continue;
            });
            return 0;
        });

        lua.register_function("NotifyOnNewObject", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'NotifyOnNewObject'.
Overloads:
#1: NotifyOnNewObject(string UClassName, LuaFunction Callback))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto class_name = ensure_str(lua.get_string());

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto [hook_lua, thread_ref] = make_hook_state(mod);

            // Duplicate the Lua function to the top of the stack for lua_xmove and luaL_ref
            lua_pushvalue(lua.get_lua_state(), 1);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            const auto func_ref = hook_lua->registry().make_ref();

            if (class_name.contains(STR(' ')))
            {
                lua.throw_error(fmt::format("Param #1 for NotifyOnNewObject cannot contain spaces; Param value: '{}'", to_utf8_string(class_name)));
            }

            const auto name_parts = explode_by_occurrence(class_name, STR('.'));
            if (name_parts.size() < 2)
            {
                lua.throw_error(fmt::format("Param #1 for NotifyOnNewObject must contain at least two parts; Param value: '{}'", to_utf8_string(class_name)));
            }

            auto class_fname = Unreal::FName(name_parts.back(), Unreal::FNAME_Find);
            if (class_fname == Unreal::NAME_None)
            {
                class_fname = Unreal::FName(name_parts.back(), Unreal::FNAME_Add);
            }

            auto class_outer_fname = Unreal::FName(name_parts.front(), Unreal::FNAME_Find);
            if (class_outer_fname == Unreal::NAME_None)
            {
                class_outer_fname = Unreal::FName(name_parts.front(), Unreal::FNAME_Add);
            }

            LuaMod::m_static_construct_object_lua_callbacks.emplace_back(hook_lua, class_fname, class_outer_fname, func_ref, thread_ref);

            Output::send<LogLevel::Verbose>(STR("[NotifyOnNewObject] Registered notification for {}\n"), class_name);

            return 0;
        });

        lua.register_function("RegisterCustomEvent", [](const LuaMadeSimple::Lua& lua) -> int {
            std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterCustomEvent'.
Overloads:
#1: RegisterCustomEvent(string EventName, LuaFunction Callback))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto event_name = ensure_str(lua.get_string());

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();
            if (!LuaMod::find_function_hook_data(LuaMod::m_custom_event_callbacks, Unreal::FName(event_name, Unreal::FNAME_Add)))
            {
                LuaMod::m_custom_event_callbacks.emplace_back(LuaMod::FunctionHookData{
                        {Unreal::FName(event_name, Unreal::FNAME_Add)},
                        LuaMod::LuaCallbackData{
                                .lua = hook_lua,
                                .instance_of_class = nullptr,
                                .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, {lua_callback_registry_index}}},
                        }});
            }

            return 0;
        });

        lua.register_function("UnregisterCustomEvent", [](const LuaMadeSimple::Lua& lua) -> int {
            std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
            std::string error_overload_not_found{R"(
No overload found for function 'UnregisterCustomEvent'.
Overloads:
#1: UnregisterCustomEvent(string EventName))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }
            auto custom_event_name = ensure_str(lua.get_string());

            LuaMod::remove_function_hook_data(LuaMod::m_custom_event_callbacks, custom_event_name);

            return 0;
        });

        lua.register_function("RegisterLoadMapPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterLoadMapPreHook'.
Overloads:
#1: RegisterLoadMapPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the lua function (it also pops it off the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_load_map_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}}});

            return 0;
        });

        lua.register_function("RegisterLoadMapPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterLoadMapPostHook'.
Overloads:
#1: RegisterLoadMapPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the lua function (it also pops it off the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_load_map_post_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}}});

            return 0;
        });

        lua.register_function("RegisterInitGameStatePreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterInitGameStatePreHook'.
Overloads:
#1: RegisterInitGameStatePreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_init_game_state_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("RegisterInitGameStatePostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterInitGameStatePostHook'.
Overloads:
#1: RegisterInitGameStatePostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_init_game_state_post_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("RegisterBeginPlayPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterBeginPlayPreHook'.
Overloads:
#1: RegisterBeginPlayPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_begin_play_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("RegisterBeginPlayPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterBeginPlayPostHook'.
Overloads:
#1: RegisterBeginPlayPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_begin_play_post_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("RegisterEndPlayPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterEndPlayPreHook'.
Overloads:
#1: RegisterEndPlayPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_end_play_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("RegisterEndPlayPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterEndPlayPostHook'.
Overloads:
#1: RegisterEndPlayPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);

            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const int32_t lua_callback_registry_index = hook_lua->registry().make_ref();

            LuaMod::m_end_play_post_callbacks.emplace_back(LuaMod::LuaCallbackData{
                    .lua = hook_lua,
                    .instance_of_class = nullptr,
                    .registry_indexes = {std::pair<const LuaMadeSimple::Lua*, LuaMod::LuaCallbackData::RegistryIndex>{hook_lua, lua_callback_registry_index}},
            });

            return 0;
        });

        lua.register_function("IterateGameDirectories", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IterateGameDirectories'.
Overloads:
#1: IterateGameDirectories())"};

            std::filesystem::path game_executable_directory = UE4SSProgram::get_program().get_game_executable_directory();
            auto game_content_dir = game_executable_directory.parent_path().parent_path() / "Content";
            if (!std::filesystem::exists(game_content_dir))
            {
                Output::send<LogLevel::Warning>(STR("IterateGameDirectories: Could not locate the root directory because the directory structure is unknown "
                                                    "(not <RootGamePath>/Game/Binaries/Win64)\n"));
                lua.set_nil();
                return 1;
            }

            auto game_name = game_executable_directory.parent_path().parent_path().filename();
            auto game_root_directory = game_executable_directory.parent_path().parent_path().parent_path();
            auto directories_table = lua.prepare_new_table();

            std::function<void(const std::filesystem::path&, LuaMadeSimple::Lua::Table&)> iterate_directory =
                    [&](const std::filesystem::path& directory, LuaMadeSimple::Lua::Table& current_directory_table) {
                        try
                        {
                            std::error_code ec;
                            for (const auto& item : std::filesystem::directory_iterator(directory, ec))
                            {
                                try
                                {
                                    if (!item.is_directory())
                                    {
                                        continue;
                                    }

                                    auto path = item.path().filename();

                                    // Set key to "Game" if this is the game directory, otherwise use the actual name
                                    std::string table_key;
                                    if (path == game_name)
                                    {
                                        table_key = "Game";
                                    }
                                    else
                                    {
                                        // TODO: When UE5 String conversion is implemented, replace with StringCast<ANSICHAR>
                                        table_key = to_utf8_string(path);
                                    }

                                    current_directory_table.add_key(table_key.c_str());
                                    auto next_directory_table = lua.prepare_new_table();

                                    // Recursively iterate the subdirectory
                                    iterate_directory(item.path(), next_directory_table);
                                    current_directory_table.fuse_pair();
                                }
                                catch (const std::exception& e)
                                {
                                    Output::send<LogLevel::Error>(STR("Error processing directory entry: {}\n"), to_wstring(e.what()));
                                }
                            }

                            if (ec)
                            {
                                Output::send<LogLevel::Error>(STR("Error iterating directory {}: {}\n"), directory.wstring(), to_wstring(ec.message()));
                            }

                            auto meta_table = lua.prepare_new_table();

                            lua_pushcfunction(lua.get_lua_state(), [](lua_State* lua_state) -> int {
                                return TRY([&] {
                                    const auto& lua = LuaMadeSimple::Lua(lua_state);
                                    std::string name{};
                                    if (!lua.is_string(2))
                                    {
                                        return 0;
                                    }
                                    name = lua.get_string(2);

                                    if (name == "__name")
                                    {
                                        lua_getmetatable(lua_state, 1);
                                        lua_pushliteral(lua_state, "__name");
                                        lua_rawget(lua_state, 2);
                                        lua.discard_value();
                                        lua.discard_value();
                                        if (!lua.is_string())
                                        {
                                            throw std::runtime_error{"Couldn't find '__name' for directory entry."};
                                        }
                                        return 1;
                                    }
                                    else if (name == "__absolute_path")
                                    {
                                        lua_getmetatable(lua_state, 1);
                                        lua_pushliteral(lua_state, "__absolute_path");
                                        lua_rawget(lua_state, 2);
                                        lua.discard_value();
                                        lua.discard_value();
                                        if (!lua.is_string())
                                        {
                                            throw std::runtime_error{"Couldn't find '__absolute_path' for directory entry."};
                                        }
                                        return 1;
                                    }
                                    else if (name == "__files")
                                    {
                                        lua_getmetatable(lua_state, 1);
                                        lua_pushliteral(lua_state, "__absolute_path");
                                        lua_rawget(lua_state, 2);
                                        lua.discard_value();
                                        lua.discard_value();
                                        if (!lua.is_string())
                                        {
                                            throw std::runtime_error{"Couldn't find '__absolute_path' for directory entry."};
                                        }

                                        const auto path_str = lua.get_string();
                                        std::wstring path_wstr;

                                        // Try to convert the path string to wstring for filesystem operations
                                        try
                                        {
                                            path_wstr = RC::to_wstring(path_str);
                                        }
                                        catch (const std::exception&)
                                        {
                                            // If conversion fails, reconstruct path as basic conversion
                                            path_wstr.reserve(path_str.size());
                                            for (char c : path_str)
                                            {
                                                path_wstr.push_back(static_cast<wchar_t>(c));
                                            }

                                            // Check if the path exists first
                                            std::error_code path_ec;
                                            if (!std::filesystem::exists(path_wstr, path_ec))
                                            {
                                                // Path doesn't exist, try to reconstruct it
                                                if (path_str.find("LogicMods") != std::string::npos)
                                                {
                                                    // Get the game executable directory
                                                    std::filesystem::path game_exec_dir = UE4SSProgram::get_program().get_game_executable_directory();

                                                    // Navigate to content directory
                                                    std::filesystem::path content_dir = game_exec_dir;
                                                    content_dir = content_dir.parent_path(); // Up to Binaries
                                                    content_dir = content_dir.parent_path(); // Up to Game
                                                    content_dir /= "Content";

                                                    if (std::filesystem::exists(content_dir))
                                                    {
                                                        auto logic_mods_dir = content_dir / "Paks/LogicMods";

                                                        // Check if it exists or try to create it
                                                        if (!std::filesystem::exists(logic_mods_dir))
                                                        {
                                                            std::error_code dir_ec;
                                                            auto paks_dir = content_dir / "Paks";
                                                            if (!std::filesystem::exists(paks_dir))
                                                            {
                                                                std::filesystem::create_directory(paks_dir, dir_ec);
                                                            }
                                                            std::filesystem::create_directory(logic_mods_dir, dir_ec);
                                                        }

                                                        if (std::filesystem::exists(logic_mods_dir))
                                                        {
                                                            path_wstr = logic_mods_dir.wstring();
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        auto files_table = lua.prepare_new_table();
                                        auto index = 1;

                                        try
                                        {
                                            std::error_code ec;
                                            if (std::filesystem::exists(path_wstr, ec))
                                            {
                                                for (const auto& item : std::filesystem::directory_iterator(path_wstr, ec))
                                                {
                                                    try
                                                    {
                                                        if (!item.is_directory())
                                                        {
                                                            files_table.add_key(index);
                                                            auto file_table = lua.prepare_new_table();

                                                            // Create safe strings for filenames and paths
                                                            // TODO: When UE5 String conversion is implemented, replace with StringCast<ANSICHAR>
                                                            std::string safe_filename = to_utf8_string(item.path().filename());
                                                            std::string safe_path = to_utf8_string(item.path());

                                                            file_table.add_pair("__name", safe_filename.c_str());
                                                            file_table.add_pair("__absolute_path", safe_path.c_str());
                                                            files_table.fuse_pair();
                                                        }
                                                        ++index;
                                                    }
                                                    catch (const std::exception& e)
                                                    {
                                                        Output::send<LogLevel::Error>(STR("Error processing file: {}\n"), to_wstring(e.what()));
                                                    }
                                                }
                                            }

                                            if (ec)
                                            {
                                                Output::send<LogLevel::Error>(STR("Error iterating files in {}: {}\n"), path_wstr, to_wstring(ec.message()));
                                            }
                                        }
                                        catch (const std::exception& e)
                                        {
                                            Output::send<LogLevel::Error>(STR("Error iterating files: {}\n"), to_wstring(e.what()));
                                        }

                                        return 1;
                                    }
                                    else
                                    {
                                        lua.set_nil();
                                        return 1;
                                    }
                                });
                            });

                            lua_setfield(lua.get_lua_state(), -2, "__index");

                            // Set metadata using safe string conversion
                            // TODO: When UE5 String conversion is implemented, replace with StringCast<ANSICHAR>
                            std::string safe_filename = to_utf8_string(directory.filename());
                            std::string safe_path = to_utf8_string(directory);

                            meta_table.add_pair("__name", safe_filename.c_str());
                            meta_table.add_pair("__absolute_path", safe_path.c_str());
                            lua_setmetatable(lua.get_lua_state(), -2);
                        }
                        catch (const std::exception& e)
                        {
                            Output::send<LogLevel::Error>(STR("Exception in iterate_directory: {}\n"), to_wstring(e.what()));
                        }
                    };

            try
            {
                iterate_directory(game_root_directory, directories_table);
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("Exception in IterateGameDirectories: {}\n"), to_wstring(e.what()));
                lua.set_nil();
                return 1;
            }

            return 1;
        });

        lua.register_function("CreateLogicModsDirectory", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'CreateLogicModsDirectory'.
Overloads:
#1: CreateLogicModsDirectory())"};
            try
            {
                std::filesystem::path game_executable_directory = UE4SSProgram::get_program().get_game_executable_directory();
                auto game_content_dir = game_executable_directory.parent_path().parent_path() / "Content";
                if (!std::filesystem::exists(game_content_dir))
                {
                    lua.throw_error("CreateLogicModsDirectory: Could not locate the \"Content\" directory because the directory structure is unknown (not "
                                    "<RootGamePath>/Game/Content)\n");
                }

                auto logic_mods_dir = game_content_dir / "Paks/LogicMods";

                std::error_code ec;
                if (std::filesystem::exists(logic_mods_dir, ec))
                {
                    Output::send<LogLevel::Warning>(
                            STR("CreateLogicModsDirectory: \"LogicMods\" directory already exists. Cancelling creation of new directory.\n"));
                    lua.set_bool(true);
                    return 1;
                }

                // Try to create the Paks directory first if it doesn't exist
                auto paks_dir = game_content_dir / "Paks";
                if (!std::filesystem::exists(paks_dir, ec))
                {
                    ec.clear();
                    bool paks_created = std::filesystem::create_directory(paks_dir, ec);
                    if (!paks_created || ec)
                    {
                        Output::send<LogLevel::Error>(STR("CreateLogicModsDirectory: Failed to create Paks directory: {}\n"), to_wstring(ec.message()));
                        // Try to continue anyway
                    }
                }

                // Now create the LogicMods directory
                ec.clear();
                bool created = std::filesystem::create_directory(logic_mods_dir, ec);

                if (!created || ec)
                {
                    Output::send<LogLevel::Error>(STR("CreateLogicModsDirectory: Error creating directory: {}\n"), to_wstring(ec.message()));

                    // Check if the directory exists despite the error (might happen with Unicode paths)
                    ec.clear();
                    if (std::filesystem::exists(logic_mods_dir, ec))
                    {
                        lua.set_bool(true);
                        return 1;
                    }

                    lua.throw_error("CreateLogicModsDirectory: Unable to create \"LogicMods\" directory. Try creating manually.\n");
                }

                Output::send<LogLevel::Warning>(STR("CreateLogicModsDirectory: LogicMods directory created.\n"));

                lua.set_bool(true);
                return 1;
            }
            catch (const std::exception& e)
            {
                Output::send<LogLevel::Error>(STR("Exception in CreateLogicModsDirectory: {}\n"), to_wstring(e.what()));
                lua.throw_error(e.what());
                return 0;
            }
        });

        lua.register_function("ExecuteAsync", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ExecuteAsync'.
Overloads:
#1: ExecuteAsync(LuaFunction Callback))"};

            auto mod = get_mod_ref(lua);

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            const int32_t lua_function_ref = lua.registry().make_ref();

            mod->actions_lock();
            mod->m_pending_actions.emplace_back(LuaMod::AsyncAction{lua_function_ref, LuaMod::ActionType::Immediate});
            mod->actions_unlock();

            return 0;
        });

        lua.register_function("ExecuteWithDelay", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ExecuteWithDelay'.
Overloads:
#1: ExecuteWithDelay(integer DelayInMilliseconds, LuaFunction Callback))"};

            if (!lua.is_integer())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            int64_t delay = lua.get_integer();

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            const int32_t lua_function_ref = lua.registry().make_ref();

            auto mod = get_mod_ref(lua);

            mod->actions_lock();
            mod->m_pending_actions.emplace_back(LuaMod::AsyncAction{
                    lua_function_ref,
                    LuaMod::ActionType::Delayed,
                    std::chrono::steady_clock::now(),
                    delay,
            });
            mod->actions_unlock();
            return 0;
        });

        lua.register_function("LoopAsync", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'LoopAsync'.
Overloads:
#1: LoopAsync(integer DelayInMilliseconds, LuaFunction Callback))"};

            if (!lua.is_integer())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            int64_t delay = lua.get_integer();

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            const int32_t lua_function_ref = lua.registry().make_ref();

            auto mod = get_mod_ref(lua);

            mod->actions_lock();
            mod->m_pending_actions.emplace_back(LuaMod::AsyncAction{
                    lua_function_ref,
                    LuaMod::ActionType::Loop,
                    std::chrono::steady_clock::now(),
                    delay,
            });
            mod->actions_unlock();

            return 0;
        });

        lua.register_function("RegisterProcessConsoleExecPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterProcessConsoleExecPreHook'.
Overloads:
#1: RegisterProcessConsoleExecPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_process_console_exec_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterProcessConsoleExecPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterProcessConsoleExecPostHook'.
Overloads:
#1: RegisterProcessConsoleExecPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_process_console_exec_post_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterCallFunctionByNameWithArgumentsPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterCallFunctionByNameWithArgumentsPreHook'.
Overloads:
#1: RegisterCallFunctionByNameWithArgumentsPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_call_function_by_name_with_arguments_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterCallFunctionByNameWithArgumentsPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterCallFunctionByNameWithArgumentsPostHook'.
Overloads:
#1: RegisterCallFunctionByNameWithArgumentsPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_call_function_by_name_with_arguments_post_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterULocalPlayerExecPreHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterULocalPlayerExecPreHook'.
Overloads:
#1: RegisterULocalPlayerExecPreHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_local_player_exec_pre_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterULocalPlayerExecPostHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterULocalPlayerExecPostHook'.
Overloads:
#1: RegisterULocalPlayerExecPostHook(LuaFunction Callback))"};

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto mod = get_mod_ref(lua);
            auto hook_lua = get_hook_lua(mod);
            callback = &LuaMod::m_local_player_exec_post_callbacks.emplace_back(LuaMod::LuaCallbackData{hook_lua, nullptr, {}});
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(hook_lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterConsoleCommandGlobalHandler", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterConsoleCommandGlobalHandler'.
Overloads:
#1: RegisterConsoleCommandGlobalHandler(string CommandName, LuaFunction Callback))"};

            if (!lua.is_string())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            auto command_name = ensure_str(lua.get_string());

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto iter = LuaMod::m_global_command_lua_callbacks.find(command_name);
            if (iter == LuaMod::m_global_command_lua_callbacks.end())
            {
                auto mod = get_mod_ref(lua);
                auto hook_lua = get_hook_lua(mod);
                callback = &LuaMod::m_global_command_lua_callbacks.emplace(command_name, LuaMod::LuaCallbackData{hook_lua, nullptr, {}}).first->second;
            }
            else
            {
                callback = &iter->second;
            }
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(&lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("RegisterConsoleCommandHandler", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RegisterConsoleCommandHandler'.
Overloads:
#1: RegisterConsoleCommandHandler(string CommandName, LuaFunction Callback))"};

            if (!lua.is_string())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            auto command_name = ensure_str(lua.get_string());

            if (!lua.is_function())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            LuaMod::LuaCallbackData* callback = nullptr;
            auto iter = LuaMod::m_custom_command_lua_pre_callbacks.find(command_name);
            if (iter == LuaMod::m_custom_command_lua_pre_callbacks.end())
            {
                auto mod = get_mod_ref(lua);
                auto hook_lua = get_hook_lua(mod);
                callback = &LuaMod::m_custom_command_lua_pre_callbacks.emplace(command_name, LuaMod::LuaCallbackData{hook_lua, nullptr, {}}).first->second;
            }
            else
            {
                callback = &iter->second;
            }
            lua_xmove(lua.get_lua_state(), callback->lua->get_lua_state(), 1);
            const int32_t lua_function_ref = callback->lua->registry().make_ref();
            callback->registry_indexes.emplace_back(&lua, LuaMod::LuaCallbackData::RegistryIndex{lua_function_ref});
            return 0;
        });

        lua.register_function("LoadAsset", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'LoadAsset'.
Overloads:
#1: LoadAsset(string AssetPathAndName))"};

            if (!Unreal::IsInGameThread())
            {
                throw std::runtime_error{"Function 'LoadAsset' can only be called from within the game thread"};
            }

            if (!lua.is_string())
            {
                throw std::runtime_error{error_overload_not_found};
            }
            auto asset_path_and_name = Unreal::FName(ensure_str(lua.get_string()), Unreal::FNAME_Add);

            auto* asset_registry = static_cast<Unreal::UAssetRegistry*>(Unreal::UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
            if (!asset_registry)
            {
                throw std::runtime_error{"Did not load assets because asset_registry was nullptr\n"};
            }

            Unreal::UObject* loaded_asset{};
            bool was_asset_found{};
            bool did_asset_load{};
            Unreal::FAssetData asset_data = asset_registry->GetAssetByObjectPath(asset_path_and_name);
            if ((Unreal::Version::IsAtMost(5, 0) && asset_data.ObjectPath().GetComparisonIndex()) || asset_data.PackageName().GetComparisonIndex())
            {
                was_asset_found = true;
                loaded_asset = Unreal::UAssetRegistryHelpers::GetAsset(asset_data);
                if (loaded_asset)
                {
                    did_asset_load = true;
                    Output::send(STR("Asset loaded\n"));
                }
                else
                {
                    Output::send(STR("Asset was found but not loaded, could be a package\n"));
                }
            }

            LuaType::auto_construct_object(lua, loaded_asset);
            lua.set_bool(was_asset_found);
            lua.set_bool(did_asset_load);
            return 3;
        });

        lua.register_function("FindObject", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'FindObject'.
Overloads:
#1: FindObject(UClass InClass, UObject|UClass InOuter, string Name, bool ExactClass)
#2: FindObject(string|FName|nil ClassName, string|FName|nil ObjectShortName, EObjectFlags RequiredFlags, EObjectFlags BannedFlags)
#3: FindObject(UClass|nil Class, string|FName|nil ObjectShortName, EObjectFlags RequiredFlags, EObjectFlags BannedFlags))"};

            if (!lua.is_string() && !lua.is_userdata() && !lua.is_nil())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            Unreal::FName object_class_name{};
            Unreal::UClass* in_class{};
            bool could_be_in_class{};
            if (lua.is_string())
            {
                object_class_name = Unreal::FName(ensure_str(lua.get_string()), Unreal::FNAME_Add);
            }
            else if (lua.is_userdata())
            {
                // The API is a bit awkward, we have to tell it to preserve the stack
                // That way, when we call 'get_userdata' again with a more specific type, there's still something to actually get
                auto& userdata = lua.get_userdata<LuaType::UE4SSBaseObject>(1, true);
                if (std::string_view{userdata.get_object_name()} == "UClass")
                {
                    in_class = lua.get_userdata<LuaType::UClass>().get_remote_cpp_object();
                    could_be_in_class = true;
                    object_class_name = in_class->GetNamePrivate();
                }
                else if (std::string_view{userdata.get_object_name()} == "FName")
                {
                    object_class_name = lua.get_userdata<LuaType::FName>().get_local_cpp_object();
                }
                else
                {
                    throw std::runtime_error{error_overload_not_found};
                }
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
                could_be_in_class = true;
            }
            else
            {
                throw std::runtime_error{error_overload_not_found};
            }

            if (!lua.is_string() && !lua.is_userdata() && !lua.is_integer() && !lua.is_nil())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            Unreal::FName object_short_name{};
            Unreal::UObject* in_outer{};
            bool could_be_in_outer{};
            bool could_be_object_short_name{};
            if (lua.is_string())
            {
                object_short_name = Unreal::FName(ensure_str(lua.get_string()), Unreal::FNAME_Add);
                could_be_object_short_name = true;
            }
            else if (lua.is_userdata())
            {
                // The API is a bit awkward, we have to tell it to preserve the stack
                // That way, when we call 'get_userdata' again with a more specific type, there's still something to actually get
                auto& userdata = lua.get_userdata<LuaType::UE4SSBaseObject>(1, true);
                std::string_view lua_object_name = userdata.get_object_name();
                // TODO: Redo when there's a bette way of checking whether a lua object is derived from UObject
                if (lua_object_name == "UObject" || lua_object_name == "UWorld" || lua_object_name == "AActor" || lua_object_name == "UClass")
                {
                    if (lua_object_name == "UClass")
                    {
                        in_outer = lua.get_userdata<LuaType::UClass>().get_remote_cpp_object();
                    }
                    else
                    {
                        in_outer = lua.get_userdata<LuaType::UObject>().get_remote_cpp_object();
                    }
                    could_be_in_outer = true;
                }
                else if (lua_object_name == "FName")
                {
                    object_short_name = lua.get_userdata<LuaType::FName>().get_local_cpp_object();
                    could_be_object_short_name = true;
                }
                else
                {
                    throw std::runtime_error{error_overload_not_found};
                }
            }
            else if (lua.is_integer())
            {
                if (lua.get_integer() == -1)
                {
                    in_outer = Unreal::UObjectGlobals::ANY_PACKAGE;
                    could_be_in_outer = true;
                }
                else
                {
                    throw std::runtime_error{error_overload_not_found};
                }
            }
            else if (lua.is_nil())
            {
                could_be_in_outer = true;
                could_be_object_short_name = true;
                lua.discard_value();
            }
            else
            {
                throw std::runtime_error{error_overload_not_found};
            }

            int32_t required_flags{Unreal::EObjectFlags::RF_NoFlags};
            std::string in_name{};
            bool could_be_in_name{};
            if (lua.is_string())
            {
                in_name = lua.get_string();
                could_be_in_name = true;
            }
            else if (lua.is_integer())
            {
                required_flags = static_cast<int32_t>(lua.get_integer());
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }

            int32_t banned_flags{Unreal::EObjectFlags::RF_NoFlags};
            bool exact_class{};
            bool could_be_exact_class{};
            if (lua.is_bool())
            {
                exact_class = lua.get_bool();
                could_be_exact_class = true;
            }
            else if (lua.is_integer())
            {
                banned_flags = static_cast<int32_t>(lua.get_integer());
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }

            if (could_be_in_class && could_be_in_outer && could_be_in_name)
            {
                LuaType::auto_construct_object(lua, Unreal::UObjectGlobals::FindObject(in_class, in_outer, ensure_str(in_name), exact_class));
            }
            else
            {
                if (could_be_exact_class || !could_be_object_short_name)
                {
                    throw std::runtime_error{error_overload_not_found};
                }
                LuaType::auto_construct_object(lua, Unreal::UObjectGlobals::FindObject(object_class_name, object_short_name, required_flags, banned_flags));
            }
            return 1;
        });

        lua.register_function("FindObjects", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'FindObjects'.
Overloads:
#1: FindObjects(integer NumObjectsToFind, string|FName|nil ClassName, string|FName|nil ObjectShortName, EObjectFlags RequiredFlags, EObjectFlags BannedFlags, bool bExactClass)
#2: FindObjects(integer NumObjectsToFind, UClass|nil Class, string|FName|nil ObjectShortName, EObjectFlags RequiredFlags, EObjectFlags BannedFlags, bool bExactClass))"};

            int32_t num_objects_to_find{};
            if (lua.is_integer())
            {
                if (num_objects_to_find < 0)
                {
                    throw std::runtime_error{error_overload_not_found};
                }
                num_objects_to_find = static_cast<int32_t>(lua.get_integer());
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }
            else
            {
                throw std::runtime_error{error_overload_not_found};
            }

            if (!lua.is_string() && !lua.is_userdata() && !lua.is_nil())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            Unreal::FName object_class_name{};
            bool object_class_name_supplied{true};
            if (lua.is_string())
            {
                object_class_name = Unreal::FName(ensure_str(lua.get_string()), Unreal::FNAME_Add);
            }
            else if (lua.is_userdata())
            {
                // The API is a bit awkward, we have to tell it to preserve the stack
                // That way, when we call 'get_userdata' again with a more specific type, there's still something to actually get
                auto& userdata = lua.get_userdata<LuaType::UE4SSBaseObject>(1, true);
                if (std::string_view{userdata.get_object_name()} == "UClass")
                {
                    object_class_name = lua.get_userdata<LuaType::UClass>().get_remote_cpp_object()->GetNamePrivate();
                }
                else if (std::string_view{userdata.get_object_name()} == "FName")
                {
                    object_class_name = lua.get_userdata<LuaType::FName>().get_local_cpp_object();
                }
                else
                {
                    throw std::runtime_error{error_overload_not_found};
                }
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
                object_class_name_supplied = false;
            }
            else
            {
                throw std::runtime_error{error_overload_not_found};
            }

            if (!lua.is_string() && !lua.is_userdata() && !lua.is_nil())
            {
                throw std::runtime_error{error_overload_not_found};
            }

            Unreal::FName object_short_name{};
            if (lua.is_string())
            {
                object_short_name = Unreal::FName(ensure_str(lua.get_string()), Unreal::FNAME_Add);
            }
            else if (lua.is_userdata())
            {
                // The API is a bit awkward, we have to tell it to preserve the stack
                // That way, when we call 'get_userdata' again with a more specific type, there's still something to actually get
                auto& userdata = lua.get_userdata<LuaType::UE4SSBaseObject>(1, true);
                if (std::string_view{userdata.get_object_name()} == "FName")
                {
                    object_short_name = lua.get_userdata<LuaType::FName>().get_local_cpp_object();
                }
                else
                {
                    throw std::runtime_error{error_overload_not_found};
                }
            }
            else if (lua.is_nil())
            {
                if (!object_class_name_supplied)
                {
                    error_overload_not_found.append("\nBoth param #1 and param #2 cannot be nil");
                    throw std::runtime_error{error_overload_not_found};
                }
                else
                {
                    lua.discard_value();
                }
            }
            else
            {
                throw std::runtime_error{error_overload_not_found};
            }

            int32_t required_flags{Unreal::EObjectFlags::RF_NoFlags};
            if (lua.is_integer())
            {
                required_flags = static_cast<int32_t>(lua.get_integer());
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }

            int32_t banned_flags{Unreal::EObjectFlags::RF_NoFlags};
            if (lua.is_integer())
            {
                banned_flags = static_cast<int32_t>(lua.get_integer());
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }

            bool exact_class{true};
            if (lua.is_integer())
            {
                exact_class = lua.get_integer();
            }
            else if (lua.is_bool())
            {
                exact_class = lua.get_bool();
            }
            else if (lua.is_nil())
            {
                lua.discard_value();
            }

            std::vector<Unreal::UObject*> objects_found{};
            Unreal::UObjectGlobals::FindObjects(static_cast<size_t>(num_objects_to_find),
                                                object_class_name,
                                                object_short_name,
                                                objects_found,
                                                required_flags,
                                                banned_flags,
                                                exact_class);

            auto table = lua.prepare_new_table(static_cast<int32_t>(objects_found.size()));
            for (size_t i = 0; i < objects_found.size(); ++i)
            {
                table.add_key(i + 1);
                LuaType::auto_construct_object(lua, objects_found[i]);
                table.fuse_pair();
            }

            return 1;
        });

        lua.register_function("GetCurrentThreadId", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetCurrentThreadId'.
Overloads:
#1: GetCurrentThreadId())"};

            LuaType::ThreadId::construct(lua, std::this_thread::get_id());

            return 1;
        });

        lua.register_function("GetMainModThreadId", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetMainModThreadId'.
Overloads:
#1: GetMainModThreadId())"};

            const auto mod = get_mod_ref(lua);
            LuaType::ThreadId::construct(lua, mod->get_main_thread_id());

            return 1;
        });

        lua.register_function("GetAsyncThreadId", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetAsyncThreadId'.
Overloads:
#1: GetAsyncThreadId())"};

            const auto mod = get_mod_ref(lua);
            LuaType::ThreadId::construct(lua, mod->get_async_thread_id());

            return 1;
        });

        lua.register_function("GetGameThreadId", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetGameThreadId'.
Overloads:
#1: GetGameThreadId())"};

            LuaType::ThreadId::construct(lua, Unreal::GetGameThreadId());

            return 1;
        });

        lua.register_function("IsInMainModThread", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsInMainModThread'.
Overloads:
#1: IsInMainModThread())"};

            const auto mod = get_mod_ref(lua);
            lua.set_bool(std::this_thread::get_id() == mod->get_main_thread_id());

            return 1;
        });

        lua.register_function("IsInAsyncThread", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsInAsyncThread'.
Overloads:
#1: IsInAsyncThread())"};

            const auto mod = get_mod_ref(lua);
            lua.set_bool(std::this_thread::get_id() == mod->get_async_thread_id());

            return 1;
        });

        lua.register_function("IsInGameThread", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsInGameThread'.
Overloads:
#1: IsInGameThread())"};

            lua.set_bool(std::this_thread::get_id() == Unreal::GetGameThreadId());

            return 1;
        });
    }

    auto LuaMod::setup_lua_global_functions(const LuaMadeSimple::Lua& lua) const -> void
    {
        setup_lua_global_functions_internal(lua, IsTrueMod::Yes);
    }

    static auto process_simple_actions(std::vector<LuaMod::SimpleLuaAction>& actions) -> void
    {
        std::erase_if(actions, [&](const LuaMod::SimpleLuaAction& lua_data) -> bool {
            if (LuaMod::m_is_currently_executing_game_action)
            {
                return false;
            }

            LuaMod::m_is_currently_executing_game_action = true;

            lua_data.lua->registry().get_function_ref(lua_data.lua_action_function_ref);

            TRY([&]() {
                lua_data.lua->call_function(0, 0);
            });

            luaL_unref(lua_data.lua->get_lua_state(), LUA_REGISTRYINDEX, lua_data.lua_action_function_ref);

            LuaMod::m_is_currently_executing_game_action = false;
            return true;
        });
    }

    template <GameThreadExecutionMethod Executor>
    static auto process_delayed_actions(std::vector<LuaMod::DelayedGameThreadAction>& actions) -> void
    {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(actions, [&](LuaMod::DelayedGameThreadAction& action) -> bool {
            // Check if pending removal first - any executor can clean up removed actions
            // regardless of method, to avoid orphaned actions that never get cleaned up
            if (action.status == LuaMod::DelayedActionStatus::PendingRemoval)
            {
                // Unref the function, but NOT the thread - the thread is shared across all actions
                // and is anchored in the registry by ensure_hook_thread_exists
                luaL_unref(action.lua->get_lua_state(), LUA_REGISTRYINDEX, action.lua_action_function_ref);
                return true;
            }

            // Only handle actions matching the executor method
            if constexpr (Executor == GameThreadExecutionMethod::ProcessEvent)
            {
                if (action.method != GameThreadExecutionMethod::ProcessEvent)
                {
                    return false;
                }
            }
            else if constexpr (Executor == GameThreadExecutionMethod::EngineTick)
            {
                if (action.method != GameThreadExecutionMethod::EngineTick)
                {
                    return false;
                }
            }

            // Skip paused actions
            if (action.status == LuaMod::DelayedActionStatus::Paused)
            {
                return false;
            }

            // Check if ready to execute
            bool ready = false;
            if (action.method == GameThreadExecutionMethod::EngineTick && action.delay_frames > 0)
            {
                // Frame-based delay
                action.frames_remaining--;
                ready = action.frames_remaining <= 0;
            }
            else if (action.method != GameThreadExecutionMethod::EngineTick && action.delay_frames > 0)
            {
                // Skip frame-based delays - they can only be processed by EngineTick
                // This should never happen since frame-based functions error if EngineTick unavailable
                if (action.delay_frames > 0)
                {
                    Output::send<LogLevel::Warning>(STR("ProcessEvent hook received frame-based delayed action - this should not happen\n"));
                    return false;
                }
            }
            else
            {
                // Time-based delay
                ready = now >= action.execute_at;
            }

            if (!ready)
            {
                return false;
            }

            if (LuaMod::m_is_currently_executing_game_action)
            {
                return false;
            }

            LuaMod::m_is_currently_executing_game_action = true;

            action.lua->registry().get_function_ref(action.lua_action_function_ref);

            TRY([&]() {
                action.lua->call_function(0, 0);
            });

            LuaMod::m_is_currently_executing_game_action = false;

            // Handle looping
            if (action.is_looping && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
            {
                // Reset the timer/frame counter
                if (action.method == GameThreadExecutionMethod::EngineTick && action.delay_frames > 0)
                {
                    action.frames_remaining = action.delay_frames;
                }
                else
                {
                    action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(action.delay_ms);
                }
                return false; // Keep in list
            }

            // Unref the function, but NOT the thread - the thread is shared across all actions
            // and is anchored in the registry by ensure_hook_thread_exists
            luaL_unref(action.lua->get_lua_state(), LUA_REGISTRYINDEX, action.lua_action_function_ref);
            return true;
        });
    }

    auto static process_event_hook([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData,
                                   [[maybe_unused]] Unreal::UObject* Context,
                                   [[maybe_unused]] Unreal::UFunction* Function,
                                   [[maybe_unused]] void* Parms) -> void
    {
        std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

        process_simple_actions(LuaMod::m_game_thread_actions);
        process_delayed_actions<GameThreadExecutionMethod::ProcessEvent>(LuaMod::m_delayed_game_thread_actions);
    }

    auto static engine_tick_hook([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData,
                                 [[maybe_unused]] Unreal::UEngine* Context,
                                 [[maybe_unused]] float DeltaSeconds,
                                 [[maybe_unused]] bool bIdle) -> void
    {
        std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

        process_simple_actions(LuaMod::m_engine_tick_actions);
        process_delayed_actions<GameThreadExecutionMethod::EngineTick>(LuaMod::m_delayed_game_thread_actions);
    }

    // Local convenience wrappers for Capabilities functions
    static auto is_engine_tick_hook_available() -> bool
    {
        return UE4SSRuntime::IsEngineTickAvailable();
    }

    static auto is_process_event_hook_available() -> bool
    {
        return UE4SSRuntime::IsProcessEventAvailable();
    }

    // Helper to ensure engine tick hook is registered
    auto LuaMod::ensure_engine_tick_hooked() -> void
    {
        if (!m_is_engine_tick_hooked)
        {
            Unreal::Hook::RegisterEngineTickPreCallback(engine_tick_hook, {false, false, STR("UE4SS"), STR("LuaModImpl")});
            m_is_engine_tick_hooked = true;
        }
    }

    // Helper to ensure process event hook is registered
    auto LuaMod::ensure_process_event_hooked(LuaMod* mod) -> void
    {
        if (!mod->m_is_process_event_hooked)
        {
            Unreal::Hook::RegisterProcessEventPreCallback(process_event_hook, {false, false, STR("UE4SS"), STR("LuaModImpl")});
            mod->m_is_process_event_hooked = true;
        }
    }

    auto LuaMod::setup_lua_global_functions_main_state_only() const -> void
    {
        m_lua.register_function("RegisterHook", [](const LuaMadeSimple::Lua& lua) -> int {
            std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

            std::string error_overload_not_found{R"(
No overload found for function 'RegisterHook'.
Overloads:
#1: RegisterHook(string UFunction_Name, LuaFunction Callback, LuaFunction PostCallback))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto function_name_no_prefix = get_function_name_without_prefix(ensure_str(lua.get_string()));

            if (!lua.is_function())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto mod = get_mod_ref(lua);
            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod); // operates on LuaMod::m_lua incrementing its stack via lua_newthread

            // Duplicate the Lua function to the top of the stack for lua_xmove and luaL_ref
            lua_pushvalue(lua.get_lua_state(), 1); // operates on LuaMadeSimple::Lua::m_lua_state
            lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);

            // Take a reference to the Lua function (it also pops it of the stack)
            const auto lua_callback_registry_index = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            bool has_post_callback{};
            int lua_post_callback_registry_index = -1;
            if (lua.is_function())
            {
                lua.discard_value();
                // Duplicate the second Lua function to the top of the stack for lua_xmove and luaL_ref
                lua_pushvalue(lua.get_lua_state(), 1); // operates on LuaMadeSimple::Lua::m_lua_state
                lua_xmove(lua.get_lua_state(), hook_lua->get_lua_state(), 1);
                lua_post_callback_registry_index = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);
                has_post_callback = true;
            }

            Unreal::UFunction* unreal_function = Unreal::UObjectGlobals::StaticFindObject<Unreal::UFunction*>(nullptr, nullptr, function_name_no_prefix);
            if (!unreal_function)
            {
                lua.throw_error(std::format(
                        "Tried to register a hook with Lua function 'RegisterHook' but no UFunction with the specified name was found.\nFunction Name: {}",
                        to_string(function_name_no_prefix)));
            }

            int32_t pre_id{};
            int32_t post_id{};

            auto func_ptr = unreal_function->GetFunc();
            if (func_ptr && func_ptr != Unreal::UObject::ProcessInternalInternal.get_function_address() &&
                unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))
            {
                auto& custom_data = g_hooked_script_function_data.emplace_back(std::make_unique<LuaUnrealScriptFunctionData>(
                        0, 0, unreal_function, mod, *hook_lua, lua_callback_registry_index, lua_post_callback_registry_index, lua_thread_registry_index));
                pre_id = unreal_function->RegisterPreHook(&lua_unreal_script_function_hook_pre, custom_data.get());
                post_id = unreal_function->RegisterPostHook(&lua_unreal_script_function_hook_post, custom_data.get());
                custom_data->pre_callback_id = pre_id;
                custom_data->post_callback_id = post_id;
                Output::send<LogLevel::Verbose>(STR("[RegisterHook] Registered native hook ({}, {}) for {}\n"),
                                                custom_data->pre_callback_id,
                                                custom_data->post_callback_id,
                                                unreal_function->GetFullName());
            }
            else if (func_ptr && func_ptr == Unreal::UObject::ProcessInternalInternal.get_function_address() &&
                     !unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))
            {
                auto function_data = find_function_hook_data(m_script_hook_callbacks, unreal_function);
                if (!function_data)
                {
                    function_data = &m_script_hook_callbacks.emplace_back(get_object_names(unreal_function), LuaCallbackData{hook_lua, nullptr, {}});
                }
                auto& callback_data = function_data->callback_data;
                // Note that non-native hooks don't have a different id for the post-callback.
                pre_id = Unreal::UnrealScriptFunctionData::MakeNewId();
                post_id = pre_id;
                callback_data.registry_indexes.emplace_back(hook_lua, LuaCallbackData::RegistryIndex{lua_callback_registry_index, pre_id});
                Output::send<LogLevel::Verbose>(STR("[RegisterHook] Registered script hook ({}, {}) for {}\n"),
                                                pre_id,
                                                post_id,
                                                unreal_function->GetFullName());
            }
            else
            {
                std::string error_message{"Was unable to register a hook with Lua function 'RegisterHook', information:\n"};
                error_message.append(fmt::format("FunctionName: {}\n", to_string(function_name_no_prefix)));
                error_message.append(fmt::format("UFunction::Func: {}\n", std::bit_cast<void*>(func_ptr)));
                error_message.append(fmt::format("ProcessInternal: {}\n", Unreal::UObject::ProcessInternalInternal.get_function_address()));
                error_message.append(
                        fmt::format("FUNC_Native: {}\n", static_cast<uint32_t>(unreal_function->HasAnyFunctionFlags(Unreal::EFunctionFlags::FUNC_Native))));
                lua.throw_error(error_message);
            }

            lua.set_integer(pre_id);
            lua.set_integer(post_id);

            return 2;
        });


        // Register EGameThreadMethod enum table
        {
            lua_State* L = m_lua.get_lua_state();
            lua_newtable(L);
            lua_pushinteger(L, static_cast<int>(GameThreadExecutionMethod::EngineTick));
            lua_setfield(L, -2, "EngineTick");
            lua_pushinteger(L, static_cast<int>(GameThreadExecutionMethod::ProcessEvent));
            lua_setfield(L, -2, "ProcessEvent");
            lua_setglobal(L, "EGameThreadMethod");
        }

        // Register capability globals
        // These indicate whether certain hooks are available (scan succeeded)
        {
            lua_State* L = m_lua.get_lua_state();
            lua_pushboolean(L, UE4SSRuntime::IsEngineTickAvailable());
            lua_setglobal(L, "EngineTickAvailable");
            lua_pushboolean(L, UE4SSRuntime::IsProcessEventAvailable());
            lua_setglobal(L, "ProcessEventAvailable");
        }

        m_lua.register_function("ExecuteInGameThread", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ExecuteInGameThread'.
Overloads:
#1: ExecuteInGameThread(LuaFunction callback)
#2: ExecuteInGameThread(LuaFunction callback, EGameThreadMethod method)
    method: EGameThreadMethod.EngineTick or EGameThreadMethod.ProcessEvent)"};

            lua_State* L = lua.get_lua_state();
            GameThreadExecutionMethod method = LuaMod::m_default_game_thread_method;
            int callback_idx = 1;

            if (lua_isfunction(L, 1) && lua_isinteger(L, 2))
            {
                // Overload #2: callback, method
                method = static_cast<GameThreadExecutionMethod>(lua_tointeger(L, 2));
            }
            else if (!lua_isfunction(L, 1))
            {
                lua.throw_error(error_overload_not_found);
            }

            const auto mod = get_mod_ref(lua);

            // Check hook availability before registering
            if (method == GameThreadExecutionMethod::EngineTick)
            {
                if (!is_engine_tick_hook_available())
                {
                    lua.throw_error("ExecuteInGameThread: EngineTick method requested but EngineTick hook is not available (AOB scan failed)");
                }
                LuaMod::ensure_engine_tick_hooked();
            }
            else if (method == GameThreadExecutionMethod::ProcessEvent)
            {
                if (!is_process_event_hook_available())
                {
                    lua.throw_error("ExecuteInGameThread: ProcessEvent method requested but ProcessEvent hook is not available (AOB scan failed)");
                }
                LuaMod::ensure_process_event_hooked(mod);
            }

            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

            lua_pushvalue(L, callback_idx);
            lua_xmove(L, hook_lua->get_lua_state(), 1);
            const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            SimpleLuaAction simpleAction{hook_lua, func_ref, lua_thread_registry_index};
            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                if (method == GameThreadExecutionMethod::EngineTick)
                {
                    LuaMod::m_engine_tick_actions.emplace_back(simpleAction);
                }
                else
                {
                    mod->m_game_thread_actions.emplace_back(simpleAction);
                }
            }

            return 0;
        });

        // ExecuteInGameThreadWithDelay - executes callback after a time delay
        // Uses default method from config, falls back to the other if unavailable
        m_lua.register_function("ExecuteInGameThreadWithDelay", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ExecuteInGameThreadWithDelay'.
Overloads:
#1: ExecuteInGameThreadWithDelay(integer delayMs, LuaFunction callback) -> integer handle
#2: ExecuteInGameThreadWithDelay(integer handle, integer delayMs, LuaFunction callback) -> nil (only creates if handle doesn't exist))"};

            lua_State* L = lua.get_lua_state();

            // Determine which overload based on argument count
            int num_args = lua_gettop(L);
            bool has_handle = (num_args >= 3 && lua_isinteger(L, 1) && lua_isinteger(L, 2) && lua_isfunction(L, 3));
            bool no_handle = (num_args >= 2 && lua_isinteger(L, 1) && lua_isfunction(L, 2));

            if (!has_handle && !no_handle)
            {
                lua.throw_error(error_overload_not_found);
            }

            const auto mod = get_mod_ref(lua);

            // Use default method from config, fall back to the other if unavailable
            GameThreadExecutionMethod method = LuaMod::m_default_game_thread_method;
            if (method == GameThreadExecutionMethod::EngineTick)
            {
                LuaMod::ensure_engine_tick_hooked();
                if (!is_engine_tick_hook_available())
                {
                    LuaMod::ensure_process_event_hooked(mod);
                    if (!is_process_event_hook_available())
                    {
                        lua.throw_error("ExecuteInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::ProcessEvent;
                }
            }
            else if (method == GameThreadExecutionMethod::ProcessEvent)
            {
                LuaMod::ensure_process_event_hooked(mod);
                if (!is_process_event_hook_available())
                {
                    LuaMod::ensure_engine_tick_hooked();
                    if (!is_engine_tick_hook_available())
                    {
                        lua.throw_error("ExecuteInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::EngineTick;
                }
            }

            if (has_handle)
            {
                // Overload #2: ExecuteInGameThreadWithDelay(handle, delayMs, callback)
                // Like UE's Delay - only creates if handle doesn't already exist
                auto handle = lua_tointeger(L, 1);
                auto delay_ms = lua_tointeger(L, 2);

                // Check if handle already exists
                {
                    std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                    for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                    {
                        if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                        {
                            // Handle exists, do nothing (like UE's Delay node)
                            return 0;
                        }
                    }
                }

                // Handle doesn't exist, create new action
                auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

                lua_pushvalue(L, 3);
                lua_xmove(L, hook_lua->get_lua_state(), 1);
                const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

                DelayedGameThreadAction action{};
                action.lua = hook_lua;
                action.lua_action_function_ref = func_ref;
                action.lua_action_thread_ref = lua_thread_registry_index;
                action.method = method;
                action.delay_ms = delay_ms;
                action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
                action.handle = handle;

                {
                    std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                    LuaMod::m_delayed_game_thread_actions.emplace_back(action);
                }

                return 0;
            }
            else
            {
                // Overload #1: ExecuteInGameThreadWithDelay(delayMs, callback) -> handle
                auto delay_ms = lua_tointeger(L, 1);
                auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

                lua_pushvalue(L, 2);
                lua_xmove(L, hook_lua->get_lua_state(), 1);
                const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

                DelayedGameThreadAction action{};
                action.lua = hook_lua;
                action.lua_action_function_ref = func_ref;
                action.lua_action_thread_ref = lua_thread_registry_index;
                action.method = method;
                action.delay_ms = delay_ms;
                action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
                action.handle = LuaMod::m_next_delayed_action_handle++;

                {
                    std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                    LuaMod::m_delayed_game_thread_actions.emplace_back(action);
                }

                lua.set_integer(action.handle);
                return 1;
            }
        });

        // RetriggerableExecuteInGameThreadWithDelay - executes callback after a time delay, resets timer if called again with same handle
        // Uses default method from config, falls back to the other if unavailable
        m_lua.register_function("RetriggerableExecuteInGameThreadWithDelay", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RetriggerableExecuteInGameThreadWithDelay'.
Overloads:
#1: RetriggerableExecuteInGameThreadWithDelay(integer handle, integer delayMs, LuaFunction callback))"};

            lua_State* L = lua.get_lua_state();
            if (!lua_isinteger(L, 1) || !lua_isinteger(L, 2) || !lua_isfunction(L, 3))
            {
                lua.throw_error(error_overload_not_found);
            }

            const auto mod = get_mod_ref(lua);

            // Use default method from config, fall back to the other if unavailable
            GameThreadExecutionMethod method = LuaMod::m_default_game_thread_method;
            if (method == GameThreadExecutionMethod::EngineTick)
            {
                LuaMod::ensure_engine_tick_hooked();
                if (!is_engine_tick_hook_available())
                {
                    LuaMod::ensure_process_event_hooked(mod);
                    if (!is_process_event_hook_available())
                    {
                        lua.throw_error("RetriggerableExecuteInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::ProcessEvent;
                }
            }
            else if (method == GameThreadExecutionMethod::ProcessEvent)
            {
                LuaMod::ensure_process_event_hooked(mod);
                if (!is_process_event_hook_available())
                {
                    LuaMod::ensure_engine_tick_hooked();
                    if (!is_engine_tick_hook_available())
                    {
                        lua.throw_error("RetriggerableExecuteInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::EngineTick;
                }
            }

            auto handle = lua_tointeger(L, 1);
            auto delay_ms = lua_tointeger(L, 2);

            // Check if an action with this handle already exists
            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        // Reset the timer for the existing action
                        action.delay_ms = delay_ms;
                        action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
                        action.status = LuaMod::DelayedActionStatus::Active;  // Unpause if paused
                        lua.set_integer(handle);
                        return 1;
                    }
                }
            }

            // No existing action, create a new one
            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

            lua_pushvalue(L, 3);
            lua_xmove(L, hook_lua->get_lua_state(), 1);
            const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            DelayedGameThreadAction action{};
            action.lua = hook_lua;
            action.lua_action_function_ref = func_ref;
            action.lua_action_thread_ref = lua_thread_registry_index;
            action.method = method;
            action.delay_ms = delay_ms;
            action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
            action.is_retriggerable = true;
            action.handle = handle;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                LuaMod::m_delayed_game_thread_actions.emplace_back(action);
            }

            return 0;
        });

        // ExecuteInGameThreadAfterFrames - executes callback after a frame delay
        // Requires EngineTick hook - cannot fall back to ProcessEvent since frames cannot be counted there
        m_lua.register_function("ExecuteInGameThreadAfterFrames", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ExecuteInGameThreadAfterFrames'.
Overloads:
#1: ExecuteInGameThreadAfterFrames(integer frames, LuaFunction callback) -> integer handle)"};

            lua_State* L = lua.get_lua_state();
            if (!lua.is_integer() || !lua_isfunction(L, 2))
            {
                lua.throw_error(error_overload_not_found);
            }

            // Frame-based delays require EngineTick - cannot use ProcessEvent
            if (!is_engine_tick_hook_available())
            {
                lua.throw_error("ExecuteInGameThreadAfterFrames: EngineTick hook is not available (AOB scan failed). Frame-based delays require EngineTick.");
            }

            auto frames = lua.get_integer();
            auto mod = get_mod_ref(lua);
            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

            lua_pushvalue(L, 1);
            lua_xmove(L, hook_lua->get_lua_state(), 1);
            const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            DelayedGameThreadAction action{};
            action.lua = hook_lua;
            action.lua_action_function_ref = func_ref;
            action.lua_action_thread_ref = lua_thread_registry_index;
            action.delay_frames = frames;
            action.frames_remaining = frames;
            action.handle = LuaMod::m_next_delayed_action_handle++;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                LuaMod::m_delayed_game_thread_actions.emplace_back(action);
                LuaMod::ensure_engine_tick_hooked();
            }

            lua.set_integer(action.handle);
            return 1;
        });

        // LoopInGameThreadWithDelay - executes callback repeatedly with a time delay
        // Uses default method from config, falls back to the other if unavailable
        m_lua.register_function("LoopInGameThreadWithDelay", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'LoopInGameThreadWithDelay'.
Overloads:
#1: LoopInGameThreadWithDelay(integer delayMs, LuaFunction callback) -> integer handle)"};

            lua_State* L = lua.get_lua_state();
            if (!lua.is_integer() || !lua_isfunction(L, 2))
            {
                lua.throw_error(error_overload_not_found);
            }

            const auto mod = get_mod_ref(lua);

            // Use default method from config, fall back to the other if unavailable
            GameThreadExecutionMethod method = LuaMod::m_default_game_thread_method;
            if (method == GameThreadExecutionMethod::EngineTick)
            {
                LuaMod::ensure_engine_tick_hooked();
                if (!is_engine_tick_hook_available())
                {
                    LuaMod::ensure_process_event_hooked(mod);
                    if (!is_process_event_hook_available())
                    {
                        lua.throw_error("LoopInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::ProcessEvent;
                }
            }
            else if (method == GameThreadExecutionMethod::ProcessEvent)
            {
                LuaMod::ensure_process_event_hooked(mod);
                if (!is_process_event_hook_available())
                {
                    LuaMod::ensure_engine_tick_hooked();
                    if (!is_engine_tick_hook_available())
                    {
                        lua.throw_error("LoopInGameThreadWithDelay: Neither EngineTick nor ProcessEvent hooks are available (AOB scans failed)");
                    }
                    method = GameThreadExecutionMethod::EngineTick;
                }
            }

            auto delay_ms = lua.get_integer();
            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

            lua_pushvalue(L, 1);
            lua_xmove(L, hook_lua->get_lua_state(), 1);
            const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            DelayedGameThreadAction action{};
            action.lua = hook_lua;
            action.lua_action_function_ref = func_ref;
            action.lua_action_thread_ref = lua_thread_registry_index;
            action.method = method;
            action.delay_ms = delay_ms;
            action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
            action.is_looping = true;
            action.handle = LuaMod::m_next_delayed_action_handle++;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                LuaMod::m_delayed_game_thread_actions.emplace_back(action);
            }

            lua.set_integer(action.handle);
            return 1;
        });

        // LoopInGameThreadAfterFrames - executes callback repeatedly with a frame delay
        // Requires EngineTick hook - cannot fall back to ProcessEvent since frames cannot be counted there
        m_lua.register_function("LoopInGameThreadAfterFrames", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'LoopInGameThreadAfterFrames'.
Overloads:
#1: LoopInGameThreadAfterFrames(integer frames, LuaFunction callback) -> integer handle)"};

            lua_State* L = lua.get_lua_state();
            if (!lua.is_integer() || !lua_isfunction(L, 2))
            {
                lua.throw_error(error_overload_not_found);
            }

            // Frame-based delays require EngineTick - cannot use ProcessEvent
            if (!is_engine_tick_hook_available())
            {
                lua.throw_error("LoopInGameThreadAfterFrames: EngineTick hook is not available (AOB scan failed). Frame-based delays require EngineTick.");
            }

            auto frames = lua.get_integer();
            auto mod = get_mod_ref(lua);
            auto [hook_lua, lua_thread_registry_index] = make_hook_state(mod);

            // After get_integer() pops the first arg, the function is now at index 1
            lua_pushvalue(L, 1);
            lua_xmove(L, hook_lua->get_lua_state(), 1);
            const auto func_ref = luaL_ref(hook_lua->get_lua_state(), LUA_REGISTRYINDEX);

            DelayedGameThreadAction action{};
            action.lua = hook_lua;
            action.lua_action_function_ref = func_ref;
            action.lua_action_thread_ref = lua_thread_registry_index;
            action.delay_frames = frames;
            action.frames_remaining = frames;
            action.is_looping = true;
            action.handle = LuaMod::m_next_delayed_action_handle++;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                LuaMod::m_delayed_game_thread_actions.emplace_back(action);
                LuaMod::ensure_engine_tick_hooked();
            }

            lua.set_integer(action.handle);
            return 1;
        });

        // ResetDelayedActionTimer - resets the timer for any delayed action using the original delay (only if owned by calling mod)
        m_lua.register_function("ResetDelayedActionTimer", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ResetDelayedActionTimer'.
Overloads:
#1: ResetDelayedActionTimer(integer handle) -> boolean success)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            bool found = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Only allow resetting actions owned by the calling mod
                    if (action.handle == handle && action.lua == mod_hook_lua && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        // Reset the timer based on whether it's time-based or frame-based
                        if (action.delay_frames > 0)
                        {
                            action.frames_remaining = action.delay_frames;
                        }
                        else
                        {
                            action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(action.delay_ms);
                        }
                        action.status = LuaMod::DelayedActionStatus::Active;  // Unpause if paused
                        found = true;
                        break;
                    }
                }
            }

            lua.set_bool(found);
            return 1;
        });

        // SetDelayedActionTimer - sets a new delay for a delayed action and restarts the timer (only if owned by calling mod)
        m_lua.register_function("SetDelayedActionTimer", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'SetDelayedActionTimer'.
Overloads:
#1: SetDelayedActionTimer(integer handle, integer newDelay) -> boolean success)"};

            lua_State* L = lua.get_lua_state();
            if (!lua.is_integer() || !lua_isinteger(L, 2))
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            auto new_delay = lua_tointeger(L, 2);
            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            bool found = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Only allow modifying actions owned by the calling mod
                    if (action.handle == handle && action.lua == mod_hook_lua && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        // Set new delay and reset the timer
                        if (action.delay_frames > 0)
                        {
                            action.delay_frames = new_delay;
                            action.frames_remaining = new_delay;
                        }
                        else
                        {
                            action.delay_ms = new_delay;
                            action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(new_delay);
                        }
                        action.status = LuaMod::DelayedActionStatus::Active;  // Unpause if paused
                        found = true;
                        break;
                    }
                }
            }

            lua.set_bool(found);
            return 1;
        });

        // PauseDelayedAction - pauses a delayed action timer (only if owned by calling mod)
        m_lua.register_function("PauseDelayedAction", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'PauseDelayedAction'.
Overloads:
#1: PauseDelayedAction(integer handle) -> boolean success)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            bool found = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Only allow pausing actions owned by the calling mod
                    if (action.handle == handle && action.lua == mod_hook_lua && action.status == LuaMod::DelayedActionStatus::Active)
                    {
                        // Store remaining time before pausing
                        auto now = std::chrono::steady_clock::now();
                        if (action.execute_at > now)
                        {
                            action.time_remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(action.execute_at - now).count();
                        }
                        else
                        {
                            action.time_remaining_ms = 0;
                        }
                        action.status = LuaMod::DelayedActionStatus::Paused;
                        found = true;
                        break;
                    }
                }
            }

            lua.set_bool(found);
            return 1;
        });

        // UnpauseDelayedAction - resumes a paused delayed action timer (only if owned by calling mod)
        m_lua.register_function("UnpauseDelayedAction", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnpauseDelayedAction'.
Overloads:
#1: UnpauseDelayedAction(integer handle) -> boolean success)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            bool found = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Only allow unpausing actions owned by the calling mod
                    if (action.handle == handle && action.lua == mod_hook_lua && action.status == LuaMod::DelayedActionStatus::Paused)
                    {
                        // Restore execute_at from remaining time
                        action.execute_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(action.time_remaining_ms);
                        action.status = LuaMod::DelayedActionStatus::Active;
                        found = true;
                        break;
                    }
                }
            }

            lua.set_bool(found);
            return 1;
        });

        // CancelDelayedAction - cancels a delayed action (only if owned by calling mod)
        m_lua.register_function("CancelDelayedAction", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'CancelDelayedAction'.
Overloads:
#1: CancelDelayedAction(integer handle) -> boolean success)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            bool found = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Only allow cancelling actions owned by the calling mod
                    if (action.handle == handle && action.lua == mod_hook_lua && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        action.status = LuaMod::DelayedActionStatus::PendingRemoval;
                        found = true;
                        break;
                    }
                }
            }

            lua.set_bool(found);
            return 1;
        });

        // IsValidDelayedActionHandle - checks if a handle refers to an existing, non-cancelled action
        m_lua.register_function("IsValidDelayedActionHandle", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsValidDelayedActionHandle'.
Overloads:
#1: IsValidDelayedActionHandle(integer handle) -> boolean valid)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            bool valid = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        valid = true;
                        break;
                    }
                }
            }

            lua.set_bool(valid);
            return 1;
        });

        // IsDelayedActionActive - checks if a delayed action is active (not paused or cancelled)
        m_lua.register_function("IsDelayedActionActive", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsDelayedActionActive'.
Overloads:
#1: IsDelayedActionActive(integer handle) -> boolean active)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            bool active = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status == LuaMod::DelayedActionStatus::Active)
                    {
                        active = true;
                        break;
                    }
                }
            }

            lua.set_bool(active);
            return 1;
        });

        // IsDelayedActionPaused - checks if a delayed action is paused
        m_lua.register_function("IsDelayedActionPaused", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'IsDelayedActionPaused'.
Overloads:
#1: IsDelayedActionPaused(integer handle) -> boolean paused)"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            bool paused = false;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status == LuaMod::DelayedActionStatus::Paused)
                    {
                        paused = true;
                        break;
                    }
                }
            }

            lua.set_bool(paused);
            return 1;
        });

        // GetDelayedActionTimeRemaining - returns remaining time in milliseconds (or frames for frame-based)
        m_lua.register_function("GetDelayedActionTimeRemaining", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetDelayedActionTimeRemaining'.
Overloads:
#1: GetDelayedActionTimeRemaining(integer handle) -> integer remainingMs (or -1 if not found))"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            int64_t remaining = -1;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        if (action.delay_frames > 0)
                        {
                            // Frame-based: return frames remaining
                            remaining = action.frames_remaining;
                        }
                        else if (action.status == LuaMod::DelayedActionStatus::Paused)
                        {
                            // Paused: return stored remaining time
                            remaining = action.time_remaining_ms;
                        }
                        else
                        {
                            // Active: calculate remaining time
                            auto now = std::chrono::steady_clock::now();
                            if (action.execute_at > now)
                            {
                                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(action.execute_at - now).count();
                            }
                            else
                            {
                                remaining = 0;
                            }
                        }
                        break;
                    }
                }
            }

            lua.set_integer(remaining);
            return 1;
        });

        // GetDelayedActionTimeElapsed - returns elapsed time in milliseconds (or frames for frame-based)
        m_lua.register_function("GetDelayedActionTimeElapsed", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetDelayedActionTimeElapsed'.
Overloads:
#1: GetDelayedActionTimeElapsed(integer handle) -> integer elapsedMs (or -1 if not found))"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            int64_t elapsed = -1;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        if (action.delay_frames > 0)
                        {
                            // Frame-based: return frames elapsed
                            elapsed = action.delay_frames - action.frames_remaining;
                        }
                        else if (action.status == LuaMod::DelayedActionStatus::Paused)
                        {
                            // Paused: calculate from stored remaining time
                            elapsed = action.delay_ms - action.time_remaining_ms;
                        }
                        else
                        {
                            // Active: calculate elapsed time
                            auto now = std::chrono::steady_clock::now();
                            if (action.execute_at > now)
                            {
                                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(action.execute_at - now).count();
                                elapsed = action.delay_ms - remaining;
                            }
                            else
                            {
                                elapsed = action.delay_ms;
                            }
                        }
                        break;
                    }
                }
            }

            lua.set_integer(elapsed);
            return 1;
        });

        // GetDelayedActionRate - returns the configured delay rate (not remaining time)
        m_lua.register_function("GetDelayedActionRate", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'GetDelayedActionRate'.
Overloads:
#1: GetDelayedActionRate(integer handle) -> integer rateMs (or -1 if not found))"};

            if (!lua.is_integer())
            {
                lua.throw_error(error_overload_not_found);
            }

            auto handle = lua.get_integer();
            int64_t rate = -1;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (const auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    if (action.handle == handle && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        if (action.delay_frames > 0)
                        {
                            // Frame-based: return frames
                            rate = action.delay_frames;
                        }
                        else
                        {
                            // Time-based: return ms
                            rate = action.delay_ms;
                        }
                        break;
                    }
                }
            }

            lua.set_integer(rate);
            return 1;
        });

        // ClearAllDelayedActions - cancels all delayed actions for the current mod
        m_lua.register_function("ClearAllDelayedActions", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'ClearAllDelayedActions'.
Overloads:
#1: ClearAllDelayedActions() -> integer count)"};

            const auto mod = get_mod_ref(lua);
            const LuaMadeSimple::Lua* mod_hook_lua = mod->m_hook_lua;
            int64_t count = 0;

            {
                std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};
                for (auto& action : LuaMod::m_delayed_game_thread_actions)
                {
                    // Check if this action belongs to the current mod by comparing hook lua states
                    if (action.lua == mod_hook_lua && action.status != LuaMod::DelayedActionStatus::PendingRemoval)
                    {
                        // Mark for removal
                        action.status = LuaMod::DelayedActionStatus::PendingRemoval;
                        count++;
                    }
                }
            }

            lua.set_integer(count);
            return 1;
        });

        // MakeHandle - returns a unique action handle
        m_lua.register_function("MakeActionHandle", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'MakeActionHandle'.
Overloads:
#1: MakeActionHandle() -> integer Handle)"};

            lua.set_integer(m_next_delayed_action_handle++);

            return 1;
        });

        m_lua.register_function("RestartCurrentMod", [](const LuaMadeSimple::Lua& lua) -> int {
            auto mod = get_mod_ref(lua);
            if (!mod)
            {
                lua.throw_error("RestartCurrentMod: Could not get mod reference");
            }

            // Use mod ID for safe cross-thread reference
            ModId mod_id = mod->get_id();
            UE4SSProgram::get_program().queue_reinstall_mod(mod_id);

            return 0;
        });

        m_lua.register_function("UninstallCurrentMod", [](const LuaMadeSimple::Lua& lua) -> int {
            auto mod = get_mod_ref(lua);
            if (!mod)
            {
                lua.throw_error("UninstallCurrentMod: Could not get mod reference");
            }

            // Use mod ID for safe cross-thread reference
            ModId mod_id = mod->get_id();
            UE4SSProgram::get_program().queue_uninstall_mod(mod_id);

            return 0;
        });

        // P1: string mod_name - Name of the mod to restart
        m_lua.register_function("RestartMod", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'RestartMod'.
Overloads:
#1: RestartMod(string mod_name))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }
                        
            UE4SSProgram::get_program().queue_reinstall_mod_by_name(lua.get_string());

            return 0;
        });

        // P1: string mod_name - Name of the mod to uninstall
        m_lua.register_function("UninstallMod", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UninstallMod'.
Overloads:
#1: UninstallMod(string mod_name))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }
            
            UE4SSProgram::get_program().queue_uninstall_mod_by_name(lua.get_string());

            return 0;
        });
    }

    auto static is_unreal_version_out_of_bounds_from_64bit(int64_t major_version, int64_t minor_version) -> bool
    {
        if (major_version < std::numeric_limits<uint32_t>::min() || major_version > std::numeric_limits<uint32_t>::max() ||
            minor_version < std::numeric_limits<uint32_t>::min() || minor_version > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        else
        {
            return true;
        }
    };

    using UnrealVersionCheckFunctionPtr = bool (*)(int32_t, int32_t);
    auto static unreal_version_check(const LuaMadeSimple::Lua& lua, UnrealVersionCheckFunctionPtr check_function, const std::string& error_overload_not_found) -> void
    {
        // Removing the table from the stack as we don't need it
        // This is required in order to align the parameters (or manually provide the stack index for the params)
        if (lua.is_table())
        {
            lua.discard_value();
        }

        // Checking the first and second param, without retrieving either
        // Makes for less code
        if (!lua.is_integer() || !lua.is_integer(2))
        {
            lua.throw_error(error_overload_not_found);
        }

        int64_t major_version = lua.get_integer();
        int64_t minor_version = lua.get_integer();

        if (!is_unreal_version_out_of_bounds_from_64bit(major_version, minor_version))
        {
            lua.throw_error("[UnrealVersion::unreal_version_check] Major/minor version numbers must be within the range of uint32");
        }

        lua.set_bool(check_function(static_cast<int32_t>(major_version), static_cast<int32_t>(minor_version)));
    }

    auto static setup_lua_classes_internal(const LuaMadeSimple::Lua& lua) -> void
    {
        // UE4SS Class -> START
        auto mod_class = lua.prepare_new_table();
        mod_class.set_has_userdata(false);

        mod_class.add_pair("GetVersion", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.set_integer(UE4SS_LIB_VERSION_MAJOR);
            lua.set_integer(UE4SS_LIB_VERSION_MINOR);
            lua.set_integer(UE4SS_LIB_VERSION_HOTFIX);
            return 3;
        });
        mod_class.make_global("UE4SS");
        // UE4SS Class -> END

        // UnrealVersion Class -> START
        auto unreal_version_class = lua.prepare_new_table();
        unreal_version_class.set_has_userdata(false);

        unreal_version_class.add_pair("GetMajor", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.set_integer(Unreal::Version::Major);
            return 1;
        });

        unreal_version_class.add_pair("GetMinor", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.set_integer(Unreal::Version::Minor);
            return 1;
        });

        unreal_version_class.add_pair("IsEqual", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnrealVersion:IsEqual'.
Overloads:
#1: IsEqual(number MajorVersion, number MinorVersion))"};

            unreal_version_check(lua, &Unreal::Version::IsEqual, error_overload_not_found);

            return 1;
        });

        unreal_version_class.add_pair("IsAtLeast", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnrealVersion:IsAtLeast'.
Overloads:
#1: IsAtLeast(number MajorVersion, number MinorVersion))"};

            unreal_version_check(lua, &Unreal::Version::IsAtLeast, error_overload_not_found);

            return 1;
        });

        unreal_version_class.add_pair("IsAtMost", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnrealVersion:IsAtMost'.
Overloads:
#1: IsAtMost(number MajorVersion, number MinorVersion))"};

            unreal_version_check(lua, &Unreal::Version::IsAtMost, error_overload_not_found);

            return 1;
        });

        unreal_version_class.add_pair("IsBelow", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnrealVersion:IsBelow'.
Overloads:
#1: IsBelow(number MajorVersion, number MinorVersion))"};

            unreal_version_check(lua, &Unreal::Version::IsBelow, error_overload_not_found);

            return 1;
        });

        unreal_version_class.add_pair("IsAbove", [](const LuaMadeSimple::Lua& lua) -> int {
            std::string error_overload_not_found{R"(
No overload found for function 'UnrealVersion:IsAbove'.
Overloads:
#1: IsAbove(number MajorVersion, number MinorVersion))"};

            unreal_version_check(lua, &Unreal::Version::IsAbove, error_overload_not_found);

            return 1;
        });
        unreal_version_class.make_global("UnrealVersion");
        // UnrealVersion Class -> END

        // FName Class -> START
        // Pre-load the global FName table
        // Without this, the metatable won't be created until an FName is constructed by another part of UE4SS
        LuaType::FName::construct(lua, Unreal::FName(static_cast<int64_t>(0)));
        lua_setglobal(lua.get_lua_state(), "FName");
        LuaType::FName::construct(lua, Unreal::NAME_None);
        lua_setglobal(lua.get_lua_state(), "NAME_None");
        // FName Class -> END

        // FText Class -> START
        // Pre-load the global FText table
        // Without this, the metatable won't be created until an FText is constructed by another part of UE4SS
        LuaType::FText::construct(lua, Unreal::FText());
        lua_setglobal(lua.get_lua_state(), "FText");
        // FText Class -> END

        // FString Class -> START
        // Pre-load the global FString constructor
        lua.register_function("FString",
                              [](const LuaMadeSimple::Lua& lua) -> int {
                                  if (lua.get_stack_size() < 1 || !lua.is_string())
                                  {
                                      lua.throw_error("FString constructor requires a string argument");
                                  }
                                  std::string_view str = lua.get_string();
                                  auto fstring = Unreal::FString(ensure_str(std::string(str)).c_str());
                                  LuaType::FString::construct(lua, &fstring);
                                  return 1;
                              });
        // FString Class -> END

        // FUtf8String Class -> START
        // Pre-load the global FUtf8String constructor
        lua.register_function("FUtf8String",
                              [](const LuaMadeSimple::Lua& lua) -> int {
                                  if (lua.get_stack_size() < 1 || !lua.is_string())
                                  {
                                      lua.throw_error("FUtf8String constructor requires a string argument");
                                  }
                                  std::string_view str = lua.get_string();
                                  auto utf8string = Unreal::FUtf8String(reinterpret_cast<const Unreal::UTF8CHAR*>(str.data()));
                                  LuaType::FUtf8String::construct(lua, &utf8string);
                                  return 1;
                              });
        // FUtf8String Class -> END

        // FAnsiString Class -> START
        // Pre-load the global FAnsiString constructor
        lua.register_function("FAnsiString",
                              [](const LuaMadeSimple::Lua& lua) -> int {
                                  if (lua.get_stack_size() < 1 || !lua.is_string())
                                  {
                                      lua.throw_error("FAnsiString constructor requires a string argument");
                                  }
                                  std::string_view str = lua.get_string();
                                  auto ansistring = Unreal::FAnsiString(str.data());
                                  LuaType::FAnsiString::construct(lua, &ansistring);
                                  return 1;
                              });
        // FAnsiString Class -> END

        // FPackageName -> START
        auto package_name = lua.prepare_new_table();
        package_name.set_has_userdata(false);

        package_name.add_pair("IsShortPackageName", [](const LuaMadeSimple::Lua& lua) -> int {
            static std::string error_overload_not_found{R"(
No overload found for function 'FPackageName:IsShortPackageName'.
Overloads:
#1: IsShortPackageName(string PossiblyLongName))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }

            File::StringType PossiblyLongName = ensure_str(lua.get_string());
            lua.set_bool(Unreal::FPackageName::IsShortPackageName(PossiblyLongName));

            return 1;
        });

        package_name.add_pair("IsValidLongPackageName", [](const LuaMadeSimple::Lua& lua) -> int {
            static std::string error_overload_not_found{R"(
No overload found for function 'FPackageName:IsValidLongPackageName'.
Overloads:
#1: IsValidLongPackageName(string InLongPackageName))"};

            if (!lua.is_string())
            {
                lua.throw_error(error_overload_not_found);
            }

            File::StringType InLongPackageName = ensure_str(lua.get_string());
            lua.set_bool(Unreal::FPackageName::IsValidLongPackageName(InLongPackageName));

            return 1;
        });

        package_name.make_global("FPackageName");
        // FPackageName -> END
    }

    auto LuaMod::setup_lua_classes(const LuaMadeSimple::Lua& lua) const -> void
    {
        setup_lua_classes_internal(lua);
    }

    auto LuaMod::prepare_mod(const LuaMadeSimple::Lua& lua) -> void
    {
        try
        {
            lua.open_all_libs();
            setup_lua_require_paths(lua);
            setup_lua_global_functions(lua);
            setup_lua_classes(lua);

            // Setup a global reference for this mod
            // It can be accessed later when you otherwise don't have access to the 'Mod' instance
            LuaType::LuaModRef::construct(lua, this);
            lua_setglobal(lua.get_lua_state(), "ModRef");

            // Setup all the input related globals (keys & modifier keys)
            register_input_globals(lua);

            register_all_property_types(lua);
            register_object_flags(lua);
            register_efindname(lua);

            // VeinCF: Register ImGui bindings + overlay Lua API
            LuaType::register_imgui_bindings(lua.get_lua_state());
            LuaType::register_overlay_bindings(lua.get_lua_state());

            lua.set_nil();
            lua_setglobal(lua.get_lua_state(), "__OriginalReturnValue");
        }
        catch (std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("Exception during mod preparation: {}\n"), ensure_str(e.what()));
            throw; // Re-throw to allow proper error handling upstream
        }
        catch (...)
        {
            Output::send<LogLevel::Error>(STR("Unknown exception during mod preparation\n"));
            throw; // Re-throw to allow proper error handling upstream
        }
    }

    auto LuaMod::fire_on_lua_start_for_cpp_mods() -> void
    {
        if (!is_started())
        {
            return;
        }

        for (const auto& mod : UE4SSProgram::get_program().m_mods)
        {
            if (auto cpp_mod = dynamic_cast<CppMod*>(mod.get()); cpp_mod && mod->is_started())
            {
                if (mod->get_name() == get_name())
                {
                    cpp_mod->fire_on_lua_start(m_lua, *m_main_lua, *m_async_lua, m_hook_lua);
                }
                cpp_mod->fire_on_lua_start(get_name(), m_lua, *m_main_lua, *m_async_lua, m_hook_lua);
            }
        }
    }

    auto LuaMod::fire_on_lua_stop_for_cpp_mods() -> void
    {
        if (!is_started())
        {
            return;
        }

        for (const auto& mod : UE4SSProgram::get_program().m_mods)
        {
            if (auto cpp_mod = dynamic_cast<CppMod*>(mod.get()); cpp_mod && mod->is_started())
            {
                if (mod->get_name() == get_name())
                {
                    cpp_mod->fire_on_lua_stop(m_lua, *m_main_lua, *m_async_lua, m_hook_lua);
                }
                cpp_mod->fire_on_lua_stop(get_name(), m_lua, *m_main_lua, *m_async_lua, m_hook_lua);
            }
        }
    }

    auto LuaMod::load_and_execute_script(const std::filesystem::path& script_path) -> bool
    {
        try
        {
            if (!std::filesystem::exists(script_path))
            {
                return false;
            }

            // Read the file content
            std::ifstream file(script_path, std::ios::binary);
            if (!file.is_open())
            {
                throw std::runtime_error(fmt::format("Failed to open script file: {}", to_utf8_string(script_path)));
            }

            // Get file size and read content
            file.seekg(0, std::ios::end);
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<char> buffer(size);
            if (!file.read(buffer.data(), size))
            {
                throw std::runtime_error(fmt::format("Failed to read script file: {}", to_utf8_string(script_path)));
            }
            file.close();

            // Generate a UTF-8 chunk name for better error messages
            std::string chunk_name = "@" + to_utf8_string(script_path);

            lua_State* L = main_lua()->get_lua_state();

            // Push error handler first so we capture the stack before it unwinds
            int err_handler_idx = LuaMadeSimple::push_pcall_error_handler(L);

            // Load the buffer
            if (int status = luaL_loadbuffer(L, buffer.data(), buffer.size(), chunk_name.c_str()); status != LUA_OK)
            {
                std::string error_msg = lua_tostring(L, -1);
                Output::send<LogLevel::Error>(STR("Error loading script: {}\n"), ensure_str(error_msg));
                lua_pop(L, 1);
                lua_remove(L, err_handler_idx); // Clean up error handler
                return false;
            }

            // Execute the chunk with our error handler
            if (int status = lua_pcall(L, 0, 0, err_handler_idx); status != LUA_OK)
            {
                // Error handler already captured the stack and notified debugger
                std::string error_msg = lua_tostring(L, -1);
                Output::send<LogLevel::Error>(STR("Error executing script: {}\n"), ensure_str(error_msg));
                lua_pop(L, 1);
                lua_remove(L, err_handler_idx); // Clean up error handler
                return false;
            }

            lua_remove(L, err_handler_idx); // Clean up error handler
            return true;
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("Exception in load_and_execute_script: {}\n"), ensure_str(e.what()));
            return false;
        }
    }

    auto LuaMod::start_mod() -> void
    {
        try
        {
            m_main_thread_id = std::this_thread::get_id();

            prepare_mod(lua());
            make_main_state(this, lua());
            setup_lua_global_functions_main_state_only();
            make_async_state(this, lua());
            start_async_thread();

            m_is_started = true;
            fire_on_lua_start_for_cpp_mods();

            // VeinCF: Register ReinstallAllMods as a Lua global
            // This wraps UE4SSProgram::queue_reinstall_mods() so Lua mods
            // can build in-game reload buttons.
            {
                auto* L = m_lua.get_lua_state();
                lua_pushcfunction(L, [](lua_State* L) -> int {
                    UE4SSProgram::get_program().queue_reinstall_mods();
                    return 0;
                });
                lua_setglobal(L, "ReinstallAllMods");
            }

            // Set up the custom module loader for handling UTF-8 paths
            setup_custom_module_loader(main_lua());

            // Use the scripts path that was already determined in the constructor
            std::filesystem::path main_script_path = m_scripts_path / STR("main.lua");

            if (std::filesystem::exists(main_script_path))
            {
                if (!load_and_execute_script(main_script_path))
                {
                    Output::send<LogLevel::Error>(STR("Failed to execute main script: {}\n"), ensure_str(main_script_path));
                }
            }
            else
            {
                // This case implies m_scripts_path itself is valid, but main.lua is missing
                Output::send<LogLevel::Error>(
                        STR("Main script 'main.lua' not found in scripts directory: {} -- Ensure your script file uses the correct casing.\n"),
                        ensure_str(m_scripts_path));
            }
        }
        catch (const std::exception& e)
        {
            Output::send<LogLevel::Error>(STR("Exception during mod startup: {}\n"), ensure_str(e.what()));
        }
    }

    template <typename T>
    concept IsPair = requires(T t) {
        { t.second };
    };
    template <typename T>
    concept IsDataIndirect = requires(T t) {
        { t.callback_data };
    };

    static auto erase_from_container(LuaMod* mod, auto& container) -> void
    {
        for (auto it = container.begin(); it != container.end();)
        {
            const auto& data = [&] {
                if constexpr (IsPair<decltype(*it)>)
                {
                    return it->second;
                }
                else if constexpr (IsDataIndirect<decltype(*it)>)
                {
                    return it->callback_data;
                }
                else
                {
                    return *it;
                }
            }();
            if (get_mod_ref(*data.lua) == mod)
            {
                it = container.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    auto LuaMod::uninstall() -> void
    {
        // ProcessEvent hook may try to run, and the lua state will not be valid
        std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

        Output::send(STR("Stopping mod '{}' for uninstall\n"), m_mod_name);

        fire_on_lua_stop_for_cpp_mods();

        if (m_async_thread.joinable())
        {
            m_async_thread.request_stop();
            m_async_thread.join();
        }

        erase_from_container(this, m_static_construct_object_lua_callbacks);
        erase_from_container(this, m_process_console_exec_pre_callbacks);
        erase_from_container(this, m_process_console_exec_post_callbacks);
        erase_from_container(this, m_global_command_lua_callbacks);
        erase_from_container(this, m_custom_command_lua_pre_callbacks);
        erase_from_container(this, m_custom_event_callbacks);
        erase_from_container(this, m_load_map_pre_callbacks);
        erase_from_container(this, m_load_map_post_callbacks);
        erase_from_container(this, m_init_game_state_pre_callbacks);
        erase_from_container(this, m_init_game_state_post_callbacks);
        erase_from_container(this, m_begin_play_pre_callbacks);
        erase_from_container(this, m_begin_play_post_callbacks);
        erase_from_container(this, m_call_function_by_name_with_arguments_pre_callbacks);
        erase_from_container(this, m_call_function_by_name_with_arguments_post_callbacks);
        erase_from_container(this, m_local_player_exec_pre_callbacks);
        erase_from_container(this, m_local_player_exec_post_callbacks);
        erase_from_container(this, m_script_hook_callbacks);

        UE4SSProgram::get_program().get_all_input_events([&](auto& key_set) {
            std::erase_if(key_set.key_data,
                          [&](auto& item) -> bool {
                              auto& [_, key_data] = item;
                              std::erase_if(key_data,
                                            [&](Input::KeyData& key_data) -> bool {
                                                // custom_data == 1: Bind came from Lua, and custom_data2 is a pointer to LuaMod.
                                                // custom_data == 2: Bind came from C++, and custom_data2 is a pointer to KeyDownEventData. Must free it.
                                                if (key_data.custom_data == 1)
                                                {
                                                    return key_data.custom_data2 == this;
                                                }
                                                else
                                                {
                                                    return false;
                                                }
                                            });

                              return key_data.empty();
                          });
        });


        // Mark all hooks for this mod as scheduled_for_removal BEFORE closing Lua state
        // This prevents hooks from firing with an invalid Lua state during the window between
        // lua_close and the actual hook unregistration
        for (auto& item : g_hooked_script_function_data)
        {
            if (item->mod == this)
            {
                item->scheduled_for_removal = true;
            }
        }

        // Remove any pending game thread actions for this mod BEFORE closing Lua state
        // Otherwise process_event_hook may try to execute actions with an invalid Lua state
        // Note: action.lua points to m_hook_lua (a thread), so compare against that
        // Must be done BEFORE m_hook_lua is set to nullptr
        std::erase_if(m_game_thread_actions, [&](const SimpleLuaAction& action) {
            return action.lua == m_hook_lua;
        });

        // Remove any pending engine tick actions for this mod
        std::erase_if(m_engine_tick_actions, [&](const SimpleLuaAction& action) {
            return action.lua == m_hook_lua;
        });

        // Remove any delayed game thread actions for this mod
        std::erase_if(m_delayed_game_thread_actions, [&](const DelayedGameThreadAction& action) {
            return action.lua == m_hook_lua;
        });

        if (m_hook_lua != nullptr)
        {
            m_hook_lua = nullptr; // lua_newthread results are handled by lua GC
        }

        if (m_async_lua && m_async_lua->get_lua_state())
        {
            lua_resetthread(m_async_lua->get_lua_state());
        }

        if (m_main_lua && m_main_lua->get_lua_state())
        {
            lua_resetthread(m_main_lua->get_lua_state());
        }

        lua_close(lua().get_lua_state());

        // Unhook all UFunctions for this mod & remove from the map that keeps track of which UFunctions have been hooked
        std::erase_if(g_hooked_script_function_data, [&](std::unique_ptr<LuaUnrealScriptFunctionData>& item) -> bool {
            if (item->mod == this)
            {
                Output::send(STR("\tUnregistering hook by id '{}#{}' for mod {}\n"), item->unreal_function->GetName(), item->pre_callback_id, item->mod->get_name());
                Output::send(STR("\tUnregistering hook by id '{}#{}' for mod {}\n"), item->unreal_function->GetName(), item->post_callback_id, item->mod->get_name());
                item->unreal_function->UnregisterHook(item->pre_callback_id);
                item->unreal_function->UnregisterHook(item->post_callback_id);
                return true;
            }

            return false;
        });

        clear_delayed_actions();
    }

    auto LuaMod::lua() const -> const LuaMadeSimple::Lua&
    {
        return m_lua;
    }

    auto LuaMod::main_lua() const -> const LuaMadeSimple::Lua*
    {
        return m_main_lua;
    }

    auto LuaMod::async_lua() const -> const LuaMadeSimple::Lua*
    {
        return m_async_lua;
    }

    auto LuaMod::get_lua_state() const -> lua_State*
    {
        return lua().get_lua_state();
    }

    auto static start_console_lua_executor() -> void
    {
        LuaStatics::console_executor = &LuaMadeSimple::new_state();
        LuaStatics::console_executor->open_all_libs();
        setup_lua_global_functions_internal(*LuaStatics::console_executor, LuaMod::IsTrueMod::No);
        setup_lua_classes_internal(*LuaStatics::console_executor);
        register_input_globals(*LuaStatics::console_executor);
        register_all_property_types(*LuaStatics::console_executor);
        register_object_flags(*LuaStatics::console_executor);
        LuaStatics::console_executor_enabled = true;
    };

    auto static stop_console_lua_executor() -> void
    {
        lua_close(LuaStatics::console_executor->get_lua_state());

        LuaStatics::console_executor = nullptr;
        LuaStatics::console_executor_enabled = false;
    }

    static auto script_hook([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::UObject* Context, Unreal::FFrame& Stack, [[maybe_unused]] void* RESULT_DECL) -> void
    {
        std::lock_guard<std::recursive_mutex> guard{LuaMod::m_thread_actions_mutex};

        auto execute_hook = [&](std::vector<LuaMod::FunctionHookData>& callback_container, bool precise_name_match) {
            if (callback_container.empty())
            {
                return;
            }
            auto data = precise_name_match ? LuaMod::find_function_hook_data(callback_container, Stack.Node())
                                           : LuaMod::find_function_hook_data(callback_container, Stack.Node()->GetNamePrivate());
            if (data)
            {
                const auto& callback_data = data->callback_data;
                for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                {
                    const auto& lua = *lua_ptr;

                    // -1 is a special value that signifies that the hook has been unregistered.
                    if (registry_index.lua_index == -1)
                    {
                        continue;
                    }

                    lua.registry().get_function_ref(registry_index.lua_index);

                    static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                    LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);

                    auto node = Stack.Node();
                    auto return_value_offset = node->GetReturnValueOffset();
                    auto has_return_value = return_value_offset != 0xFFFF;
                    auto num_unreal_params = node->GetNumParms();
                    if (has_return_value && num_unreal_params > 0)
                    {
                        --num_unreal_params;
                    }

                    if (has_return_value || num_unreal_params > 0)
                    {
                        for (Unreal::FProperty* param : Unreal::TFieldRange<Unreal::FProperty>(node, Unreal::EFieldIterationFlags::IncludeDeprecated))
                        {
                            if (!param->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_Parm))
                            {
                                continue;
                            }
                            if (has_return_value && param->GetOffset_Internal() == return_value_offset)
                            {
                                continue;
                            }

                            auto param_type = param->GetClass().GetFName();
                            auto param_type_comparison_index = param_type.GetComparisonIndex();
                            if (auto it = LuaType::StaticState::m_property_value_pushers.find(param_type_comparison_index);
                                it != LuaType::StaticState::m_property_value_pushers.end())
                            {
                                void* data{};
                                if (param->HasAnyPropertyFlags(Unreal::EPropertyFlags::CPF_OutParm))
                                {
                                    data = Unreal::FindOutParamValueAddress(Stack, param);
                                }
                                else
                                {
                                    data = param->ContainerPtrToValuePtr<void>(Stack.Locals());
                                }
                                const LuaType::PusherParams pusher_param{
                                        .operation = LuaType::Operation::GetParam,
                                        .lua = lua,
                                        .base = nullptr,
                                        .data = data,
                                        .property = param,
                                };
                                auto& type_handler = it->second;
                                type_handler(pusher_param);
                            }
                            else
                            {
                                lua.throw_error(fmt::format(
                                        "[script_hook] Tried accessing unreal property without a registered handler. Property type '{}' not supported.",
                                        to_string(param_type.ToString())));
                            }
                        };
                    }

                    lua.call_function(num_unreal_params + 1, 1);

                    bool return_value_handled{};
                    if (has_return_value && RESULT_DECL && lua.get_stack_size() > 0 && !lua.is_nil())
                    {
                        auto return_property = node->GetReturnProperty();
                        if (return_property)
                        {
                            auto return_property_type = return_property->GetClass().GetFName();
                            auto return_property_type_comparison_index = return_property_type.GetComparisonIndex();

                            if (auto it = LuaType::StaticState::m_property_value_pushers.find(return_property_type_comparison_index);
                                it != LuaType::StaticState::m_property_value_pushers.end())
                            {
                                const LuaType::PusherParams pusher_params{.operation = LuaType::Operation::Set,
                                                                          .lua = lua,
                                                                          .base = static_cast<Unreal::UObject*>(RESULT_DECL),
                                                                          .data = RESULT_DECL,
                                                                          .property = return_property};
                                auto& type_handler = it->second;
                                type_handler(pusher_params);
                                return_value_handled = true;
                            }
                            else
                            {
                                auto return_property_type_name = return_property_type.ToString();
                                auto return_property_name = return_property->GetName();

                                Output::send(STR("Tried altering return value of a custom BP function without a registered handler for return type Return "
                                                 "property '{}' of type '{}' not supported."),
                                             return_property_name,
                                             return_property_type_name);
                            }
                        }
                    }

                    if (!return_value_handled)
                    {
                        lua.discard_value();
                    }
                }
            }
        };

        TRY([&] {
            execute_hook(LuaMod::m_custom_event_callbacks, false);
            execute_hook(LuaMod::m_script_hook_callbacks, true);
        });
    }

    auto LuaMod::on_program_start() -> void
    {
        Unreal::UObjectArray::AddUObjectDeleteListener(&LuaType::FLuaObjectDeleteListener::s_lua_object_delete_listener);
        const Unreal::Hook::FCallbackOptions common_opts {false, false, STR("UE4SS"), STR("LuaModImpl")};
        Unreal::Hook::RegisterLoadMapPreCallback(
                [](Unreal::Hook::TCallbackIterationData<bool>& CallbackIterationData, Unreal::UEngine* Engine, Unreal::FWorldContext& WorldContext, Unreal::FURL URL, Unreal::UPendingNetGame* PendingGame, Unreal::FString& Error) {
                    TRY([&] {
                        for (const auto& callback_data : m_load_map_pre_callbacks)
                        {
                            for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                            {
                                const auto& lua = *lua_ptr;

                                lua.registry().get_function_ref(registry_index.lua_index);
                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(lua, &Engine, s_object_property_name);
                                LuaType::RemoteUnrealParam::construct(lua, &WorldContext.GetThisCurrentWorld(), s_object_property_name);
                                LuaType::FURL::construct(lua, URL);
                                LuaType::RemoteUnrealParam::construct(lua, &PendingGame, s_object_property_name);
                                callback_data.lua->set_string(to_string(*Error));
                                lua.call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    callback_data.lua->discard_value();
                                }
                                else if (!callback_data.lua->is_bool())
                                {
                                    throw std::runtime_error{"A callback for 'LoadMap' must return bool or nil"};
                                }
                                else
                                {
                                    CallbackIterationData.TrySetReturnValue(callback_data.lua->get_bool());
                                }
                            }
                        }
                    });
                }, common_opts);

        Unreal::Hook::RegisterLoadMapPostCallback(
                [](Unreal::Hook::TCallbackIterationData<bool>& CallbackIterationData, Unreal::UEngine* Engine, Unreal::FWorldContext& WorldContext, Unreal::FURL URL, Unreal::UPendingNetGame* PendingGame, Unreal::FString& Error) {
                    TRY([&] {
                        for (const auto& callback_data : m_load_map_post_callbacks)
                        {
                            for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                            {
                                const auto& lua = *lua_ptr;

                                lua.registry().get_function_ref(registry_index.lua_index);
                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(lua, &Engine, s_object_property_name);
                                LuaType::RemoteUnrealParam::construct(lua, &WorldContext.GetThisCurrentWorld(), s_object_property_name);
                                LuaType::FURL::construct(lua, URL);
                                LuaType::RemoteUnrealParam::construct(lua, &PendingGame, s_object_property_name);
                                callback_data.lua->set_string(to_string(*Error));
                                lua.call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    callback_data.lua->discard_value();
                                }
                                else if (!callback_data.lua->is_bool())
                                {
                                    throw std::runtime_error{"A callback for 'LoadMap' must return bool or nil"};
                                }
                                else
                                {
                                    CallbackIterationData.TrySetReturnValue(callback_data.lua->get_bool());
                                }
                            }
                        }
                    });
                }, common_opts);

        Unreal::Hook::RegisterInitGameStatePreCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AGameModeBase* Context) {
            TRY([&] {
                for (const auto& callback_data : m_init_game_state_pre_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        lua.call_function(1, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterInitGameStatePostCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AGameModeBase* Context) {
            TRY([&] {
                for (const auto& callback_data : m_init_game_state_post_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        lua.call_function(1, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterBeginPlayPreCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AActor* Context) {
            TRY([&] {
                for (const auto& callback_data : m_begin_play_pre_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        lua.call_function(1, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterBeginPlayPostCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AActor* Context) {
            TRY([&] {
                for (const auto& callback_data : m_begin_play_post_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        lua.call_function(1, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterEndPlayPreCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AActor* Context, Unreal::EEndPlayReason EndPlayReason) {
            TRY([&] {
                for (const auto& callback_data : m_end_play_pre_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        static auto s_int_property_name = Unreal::FName(STR("IntProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &EndPlayReason, s_int_property_name);
                        lua.call_function(2, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterEndPlayPostCallback([]([[maybe_unused]] Unreal::Hook::TCallbackIterationData<void>& CallbackIterationData, [[maybe_unused]] Unreal::AActor* Context, Unreal::EEndPlayReason EndPlayReason) {
            TRY([&] {
                for (const auto& callback_data : m_end_play_post_callbacks)
                {
                    for (const auto& [lua_ptr, registry_index] : callback_data.registry_indexes)
                    {
                        const auto& lua = *lua_ptr;

                        lua.registry().get_function_ref(registry_index.lua_index);
                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &Context, s_object_property_name);
                        static auto s_int_property_name = Unreal::FName(STR("IntProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(lua, &EndPlayReason, s_int_property_name);
                        lua.call_function(2, 0);
                    }
                }
            });
        }, common_opts);

        Unreal::Hook::RegisterStaticConstructObjectPostCallback([](const Unreal::FStaticConstructObjectParameters&, Unreal::UObject* constructed_object) {
            return TRY([&] {
                auto attempt_to_call_callback = [constructed_object](const Unreal::UStruct* comparison_class) {
                    std::erase_if(m_static_construct_object_lua_callbacks, [&](const LuaCancellableCallbackData& callback_data) -> bool {
                        bool cancel = false;
                        if (comparison_class->GetNamePrivate().Equals(callback_data.instance_class_name) && comparison_class->GetOuterPrivate()->GetNamePrivate().Equals(callback_data.instance_class_outer_name))
                        {
                            callback_data.lua->registry().get_function_ref(callback_data.lua_callback_function_ref);
                            LuaType::auto_construct_object(*callback_data.lua, constructed_object);
                            callback_data.lua->call_function(1, 1);
                            cancel = callback_data.lua->is_bool(-1) && callback_data.lua->get_bool(-1);
                            if (cancel)
                            {
                                // Release the thread_ref to GC.
                                luaL_unref(callback_data.lua->get_lua_state(), LUA_REGISTRYINDEX, callback_data.lua_callback_thread_ref);
                            }
                        }

                        return cancel;
                    });
                };
                Unreal::UStruct* object_class = constructed_object->GetClassPrivate();
                attempt_to_call_callback(object_class);
                for (const auto comparison_class : Unreal::TSuperStructRange(object_class))
                {
                    attempt_to_call_callback(comparison_class);
                }
                return constructed_object;
            });
        });

        Unreal::Hook::RegisterULocalPlayerExecPreCallback([](Unreal::ULocalPlayer* context, Unreal::UWorld* in_world, const TCHAR* cmd, Unreal::FOutputDevice& ar)
                                                                  -> Unreal::Hook::ULocalPlayerExecCallbackReturnValue {
            return TRY([&] {
                for (const auto& callback_data : m_local_player_exec_pre_callbacks)
                {
                    Unreal::Hook::ULocalPlayerExecCallbackReturnValue return_value{};

                    for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                    {
                        callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                        LuaType::RemoteUnrealParam::construct(*callback_data.lua, &in_world, s_object_property_name);
                        callback_data.lua->set_string(to_string(cmd));
                        LuaType::FOutputDevice::construct(*callback_data.lua, &ar);

                        callback_data.lua->call_function(4, 2);

                        if (callback_data.lua->is_nil())
                        {
                            return_value.UseOriginalReturnValue = true;
                            callback_data.lua->discard_value();
                        }
                        else if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"The first return value for 'RegisterULocalPlayerExecPreHook' must return bool or nil"};
                        }
                        else
                        {
                            return_value.UseOriginalReturnValue = false;
                            return_value.NewReturnValue = callback_data.lua->get_bool();
                        }

                        if (callback_data.lua->is_nil())
                        {
                            return_value.ExecuteOriginalFunction = true;
                            callback_data.lua->discard_value();
                        }
                        else if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"The second return value for callback 'RegisterULocalPlayerExecPreHook' must return bool or nil"};
                        }
                        else
                        {
                            return_value.ExecuteOriginalFunction = callback_data.lua->get_bool();
                        }
                    }

                    return return_value;
                }

                return Unreal::Hook::ULocalPlayerExecCallbackReturnValue{};
            });
        });

        Unreal::Hook::RegisterULocalPlayerExecPostCallback([](Unreal::ULocalPlayer* context, Unreal::UWorld* in_world, const TCHAR* cmd, Unreal::FOutputDevice& ar)
                                                                   -> Unreal::Hook::ULocalPlayerExecCallbackReturnValue {
            return TRY([&] {
                for (const auto& callback_data : m_local_player_exec_post_callbacks)
                {
                    Unreal::Hook::ULocalPlayerExecCallbackReturnValue return_value{};

                    for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                    {
                        callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                        static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                        LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                        LuaType::RemoteUnrealParam::construct(*callback_data.lua, &in_world, s_object_property_name);
                        callback_data.lua->set_string(to_string(cmd));
                        LuaType::FOutputDevice::construct(*callback_data.lua, &ar);

                        callback_data.lua->call_function(4, 2);

                        if (callback_data.lua->is_nil())
                        {
                            return_value.UseOriginalReturnValue = true;
                            callback_data.lua->discard_value();
                        }
                        else if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"A callback for 'RegisterULocalPlayerExecPreHook' must return bool or nil"};
                        }
                        else
                        {
                            return_value.UseOriginalReturnValue = false;
                            return_value.NewReturnValue = callback_data.lua->get_bool();
                        }

                        if (callback_data.lua->is_nil())
                        {
                            return_value.ExecuteOriginalFunction = true;
                            callback_data.lua->discard_value();
                        }
                        else if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"The second return value for callback 'RegisterULocalPlayerExecPreHook' must return bool or nil"};
                        }
                        else
                        {
                            return_value.ExecuteOriginalFunction = callback_data.lua->get_bool();
                        }
                    }

                    return return_value;
                }

                return Unreal::Hook::ULocalPlayerExecCallbackReturnValue{};
            });
        });

        Unreal::Hook::RegisterCallFunctionByNameWithArgumentsPreCallback(
                [](Unreal::UObject* context, const TCHAR* str, Unreal::FOutputDevice& ar, Unreal::UObject* executor, bool b_force_call_with_non_exec)
                        -> std::pair<bool, bool> {
                    return TRY([&] {
                        std::pair<bool, bool> return_value{};
                        for (const auto& callback_data : m_call_function_by_name_with_arguments_pre_callbacks)
                        {

                            for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                            {
                                callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                                callback_data.lua->set_string(to_string(str));
                                LuaType::FOutputDevice::construct(*callback_data.lua, &ar);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &executor, s_object_property_name);
                                callback_data.lua->set_bool(b_force_call_with_non_exec);

                                callback_data.lua->call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    return_value.first = false;
                                    callback_data.lua->discard_value();
                                }
                                else if (!callback_data.lua->is_bool())
                                {
                                    throw std::runtime_error{"A callback for 'RegisterCallFunctionByNameWithArgumentsPreHook' must return bool or nil"};
                                }
                                else
                                {
                                    return_value.first = true;
                                    return_value.second = callback_data.lua->get_bool();
                                }
                            }
                        }

                        return return_value;
                    });
                });

        Unreal::Hook::RegisterCallFunctionByNameWithArgumentsPostCallback(
                [](Unreal::UObject* context, const TCHAR* str, Unreal::FOutputDevice& ar, Unreal::UObject* executor, bool b_force_call_with_non_exec)
                        -> std::pair<bool, bool> {
                    return TRY([&] {
                        std::pair<bool, bool> return_value{};
                        for (const auto& callback_data : m_call_function_by_name_with_arguments_post_callbacks)
                        {

                            for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                            {
                                callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                                callback_data.lua->set_string(to_string(str));
                                LuaType::FOutputDevice::construct(*callback_data.lua, &ar);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &executor, s_object_property_name);
                                callback_data.lua->set_bool(b_force_call_with_non_exec);

                                callback_data.lua->call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    return_value.first = false;
                                    callback_data.lua->discard_value();
                                }
                                else if (!callback_data.lua->is_bool())
                                {
                                    throw std::runtime_error{"A callback for 'RegisterCallFunctionByNameWithArgumentsPreHook' must return bool or nil"};
                                }
                                else
                                {
                                    return_value.first = true;
                                    return_value.second = callback_data.lua->get_bool();
                                }
                            }
                        }

                        return return_value;
                    });
                });

        // Lua from the in-game console.
        Unreal::Hook::RegisterProcessConsoleExecCallback([](Unreal::UObject* context, const TCHAR* cmd, Unreal::FOutputDevice& ar, Unreal::UObject* executor) -> bool {
            auto logln = [&ar](const File::StringType& log_message) {
                Output::send(fmt::format(STR("{}\n"), log_message));
                ar.Log(FromCharTypePtr<TCHAR>(log_message.c_str()));
            };

            if (!LuaStatics::console_executor_enabled && String::iequal(File::StringViewType{ToCharTypePtr(cmd)}, File::StringViewType{STR("luastart")}))
            {
                start_console_lua_executor();
                logln(STR("Console Lua executor started"));
                return true;
            }
            else if (LuaStatics::console_executor_enabled && String::iequal(File::StringViewType{ToCharTypePtr(cmd)}, File::StringViewType{STR("luastop")}))
            {
                stop_console_lua_executor();
                logln(STR("Console Lua executor stopped"));
                return true;
            }
            else if (LuaStatics::console_executor_enabled && String::iequal(File::StringViewType{ToCharTypePtr(cmd)}, File::StringViewType{STR("luarestart")}))
            {
                stop_console_lua_executor();
                start_console_lua_executor();
                logln(STR("Console Lua executor restarted"));
                return true;
            }
            else if (String::iequal(File::StringViewType{ToCharTypePtr(cmd)}, File::StringViewType{STR("clear")}))
            {
                // TODO: Replace with proper implementation when we have UGameViewportClient and UConsole.
                //       This should be fairly cross-game & cross-engine-version compatible even without the proper implementation.
                //       This is because I don't think they've changed the layout here and we have a reflected property right before the unreflected one that we're looking for.
                Unreal::UObject** console = static_cast<Unreal::UObject**>(context->GetValuePtrByPropertyName(FromCharTypePtr<TCHAR>(STR("ViewportConsole"))));
                auto* default_texture_white = std::bit_cast<Unreal::TArray<Unreal::FString>*>(
                        static_cast<uint8_t*>((*console)->GetValuePtrByPropertyNameInChain(FromCharTypePtr<TCHAR>(STR("DefaultTexture_White")))) + 0x8);
                auto* scrollback = std::bit_cast<int32_t*>(std::bit_cast<uint8_t*>(default_texture_white) + 0x10);
                default_texture_white->SetNum(0);
                default_texture_white->SetMax(0);
                *scrollback = 0;
                return true;
            }
            else if (LuaStatics::console_executor_enabled)
            {
                if (!LuaStatics::console_executor)
                {
                    logln(STR("Console Lua executor is enabled but the Lua instance is nullptr. Please try run RC_LUA_START again."));
                    return true;
                }

                LuaLibrary::set_outputdevice_ref(*LuaStatics::console_executor, &ar);

                // logln(fmt::format(STR("Executing '{}' as Lua"), cmd));

                try
                {
                    lua_State* L = LuaStatics::console_executor->get_lua_state();

                    // Push error handler to capture stack before it unwinds
                    int err_handler_idx = LuaMadeSimple::push_pcall_error_handler(L);

                    if (int status = luaL_loadstring(L, to_string(cmd).c_str()); status != LUA_OK)
                    {
                        lua_remove(L, err_handler_idx);
                        LuaStatics::console_executor->throw_error(
                                fmt::format("luaL_loadstring returned {}", LuaStatics::console_executor->resolve_status_message(status, true)));
                    }

                    if (int status = lua_pcall(L, 0, LUA_MULTRET, err_handler_idx); status != LUA_OK)
                    {
                        lua_pop(L, 1); // Pop error message
                        lua_remove(L, err_handler_idx);
                        LuaStatics::console_executor->throw_error(
                                fmt::format("lua_pcall returned {}", LuaStatics::console_executor->resolve_status_message(status, true)));
                    }

                    lua_remove(L, err_handler_idx);
                }
                catch (std::runtime_error& e)
                {
                    logln(ensure_str(e.what()));
                }

                // We always return true when the console Lua executor is enabled in order to suppress other handlers
                return true;
            }
            else
            {
                return false;
            }
        });

        // RegisterProcessConsoleExecPreHook
        Unreal::Hook::RegisterProcessConsoleExecGlobalPreCallback(
                [](Unreal::UObject* context, const TCHAR* cmd, Unreal::FOutputDevice& ar, Unreal::UObject* executor) -> std::pair<bool, bool> {
                    return TRY([&] {
                        auto command = File::StringType{ToCharTypePtr(cmd)};
                        auto command_parts = explode_by_occurrence_with_quotes(command, STR(' '));

                        std::pair<bool, bool> return_value{};
                        for (const auto& callback_data : m_process_console_exec_pre_callbacks)
                        {

                            for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                            {
                                callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                                callback_data.lua->set_string(to_string(command));
                                auto params_table = callback_data.lua->prepare_new_table();
                                for (size_t i = 0; i < command_parts.size(); ++i)
                                {
                                    const auto& command_part = command_parts[i];
                                    params_table.add_pair(i + 1, to_string(command_part).c_str());
                                }
                                LuaType::FOutputDevice::construct(*callback_data.lua, &ar);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &executor, s_object_property_name);

                                callback_data.lua->call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    return_value.first = false;
                                    callback_data.lua->discard_value();
                                }
                                else if (callback_data.lua->is_bool())
                                {
                                    return_value.first = true;
                                    return_value.second = callback_data.lua->get_bool();
                                }
                                else
                                {
                                    throw std::runtime_error{"A callback for 'RegisterProcessConsoleExecHook' must return bool or nil"};
                                }
                            }
                        }

                        return return_value;
                    });
                });

        // RegisterProcessConsoleExecPostHook
        Unreal::Hook::RegisterProcessConsoleExecGlobalPostCallback(
                [](Unreal::UObject* context, const TCHAR* cmd, Unreal::FOutputDevice& ar, Unreal::UObject* executor) -> std::pair<bool, bool> {
                    return TRY([&] {
                        auto command = File::StringType{ToCharTypePtr(cmd)};
                        auto command_parts = explode_by_occurrence_with_quotes(command, STR(' '));

                        std::pair<bool, bool> return_value{};
                        for (const auto& callback_data : m_process_console_exec_post_callbacks)
                        {
                            for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                            {
                                callback_data.lua->registry().get_function_ref(registry_index.lua_index);

                                static auto s_object_property_name = Unreal::FName(STR("ObjectProperty"), Unreal::FNAME_Find);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &context, s_object_property_name);
                                callback_data.lua->set_string(to_string(command));
                                auto params_table = callback_data.lua->prepare_new_table();
                                for (size_t i = 0; i < command_parts.size(); ++i)
                                {
                                    const auto& command_part = command_parts[i];
                                    params_table.add_pair(i + 1, to_string(command_part).c_str());
                                }
                                LuaType::FOutputDevice::construct(*callback_data.lua, &ar);
                                LuaType::RemoteUnrealParam::construct(*callback_data.lua, &executor, s_object_property_name);

                                callback_data.lua->call_function(5, 1);

                                if (callback_data.lua->is_nil())
                                {
                                    return_value.first = false;
                                    callback_data.lua->discard_value();
                                }
                                else if (callback_data.lua->is_bool())
                                {
                                    return_value.first = true;
                                    return_value.second = callback_data.lua->get_bool();
                                }
                                else
                                {
                                    throw std::runtime_error{"A callback for 'RegisterProcessConsoleExecHook' must return bool or nil"};
                                }
                            }
                        }

                        return return_value;
                    });
                });

        // RegisterConsoleCommandHandler
        Unreal::Hook::RegisterProcessConsoleExecCallback([](Unreal::UObject* context, const TCHAR* cmd, Unreal::FOutputDevice& ar, Unreal::UObject* executor) -> bool {
            (void)executor;

            if (!Unreal::Cast<Unreal::UGameViewportClient>(context))
            {
                return false;
            }

            return TRY([&] {
                auto command = File::StringType{ToCharTypePtr(cmd)};
                auto command_parts = explode_by_occurrence_with_quotes(command, STR(' '));
                File::StringType command_name = command;
                if (command_parts.size() > 1)
                {
                    command_name = command_parts[0];
                }

                if (auto it = m_custom_command_lua_pre_callbacks.find(command_name); it != m_custom_command_lua_pre_callbacks.end())
                {
                    const auto& callback_data = it->second;

                    bool return_value{};

                    for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                    {
                        callback_data.lua->registry().get_function_ref(registry_index.lua_index);
                        callback_data.lua->set_string(to_string(command));

                        auto params_table = callback_data.lua->prepare_new_table();
                        for (size_t i = 1; i < command_parts.size(); ++i)
                        {
                            const auto& command_part = command_parts[i];
                            params_table.add_pair(i, to_string(command_part).c_str());
                        }

                        LuaType::FOutputDevice::construct(*callback_data.lua, &ar);

                        callback_data.lua->call_function(3, 1);

                        if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"A custom console command handle must return true or false"};
                        }

                        return_value = callback_data.lua->get_bool();
                    }

                    return return_value;
                }

                return false;
            });
        });

        // RegisterConsoleCommandGlobalHandler
        Unreal::Hook::RegisterProcessConsoleExecCallback([](Unreal::UObject* context, const TCHAR* cmd, Unreal::FOutputDevice& ar, Unreal::UObject* executor) -> bool {
            (void)context;
            (void)executor;

            return TRY([&] {
                auto command = File::StringType{ToCharTypePtr(cmd)};
                auto command_parts = explode_by_occurrence_with_quotes(command, STR(' '));
                File::StringType command_name = command;
                if (command_parts.size() > 1)
                {
                    command_name = command_parts[0];
                }

                if (auto it = m_global_command_lua_callbacks.find(command_name); it != m_global_command_lua_callbacks.end())
                {
                    const auto& callback_data = it->second;

                    bool return_value{};

                    for (const auto& [lua, registry_index] : callback_data.registry_indexes)
                    {
                        callback_data.lua->registry().get_function_ref(registry_index.lua_index);
                        callback_data.lua->set_string(to_string(command));

                        auto params_table = callback_data.lua->prepare_new_table();
                        for (size_t i = 1; i < command_parts.size(); ++i)
                        {
                            const auto& command_part = command_parts[i];
                            params_table.add_pair(i, to_string(command_part).c_str());
                        }

                        LuaType::FOutputDevice::construct(*callback_data.lua, &ar);

                        callback_data.lua->call_function(3, 1);

                        if (!callback_data.lua->is_bool())
                        {
                            throw std::runtime_error{"A custom console command handle must return true or false"};
                        }

                        return_value = callback_data.lua->get_bool();
                    }

                    return return_value;
                }

                return false;
            });
        });

        if (Unreal::UObject::ProcessLocalScriptFunctionInternal.is_ready() && Unreal::Version::IsAtLeast(4, 22))
        {
            Output::send(STR("Enabling custom events\n"));
            Unreal::Hook::RegisterProcessLocalScriptFunctionPostCallback(script_hook, {false, false, STR("UE4SS"), STR("LuaModImplScriptHook")});
        }
        else if (Unreal::UObject::ProcessInternalInternal.is_ready() && Unreal::Version::IsBelow(4, 22))
        {
            Output::send(STR("Enabling custom events\n"));
            Unreal::Hook::RegisterProcessInternalPostCallback(script_hook, {false, false, STR("UE4SS"), STR("LuaModImplScriptHook")});
        }
    }

    auto LuaMod::update_async() -> void
    {
        for (m_processing_events = true; m_processing_events && !m_async_thread.get_stop_token().stop_requested();)
        {
            if (m_pause_events_processing)
            {
                continue;
            }

            process_delayed_actions();

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    auto LuaMod::process_delayed_actions() -> void
    {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        actions_lock();

        m_delayed_actions.insert(m_delayed_actions.end(), std::make_move_iterator(m_pending_actions.begin()), std::make_move_iterator(m_pending_actions.end()));
        m_pending_actions.clear();

        actions_unlock();

        m_delayed_actions.erase(std::remove_if(m_delayed_actions.begin(),
                                               m_delayed_actions.end(),
                                               [&](AsyncAction& action) -> bool {
                                                   auto passed =
                                                           now -
                                                           std::chrono::duration_cast<std::chrono::milliseconds>(action.created_at.time_since_epoch()).count();
                                                   auto duration_since_creation = (action.type == LuaMod::ActionType::Immediate || passed >= action.delay);
                                                   if (duration_since_creation)
                                                   {
                                                       bool result = true;
                                                       try
                                                       {
                                                           async_lua()->registry().get_function_ref(action.lua_action_function_ref);
                                                           if (action.type == LuaMod::ActionType::Loop)
                                                           {
                                                               async_lua()->call_function(0, 1);
                                                               result = async_lua()->is_bool() && async_lua()->get_bool();
                                                               action.created_at = std::chrono::steady_clock::now();
                                                           }
                                                           else
                                                           {
                                                               async_lua()->call_function(0, 0);
                                                           }
                                                       }
                                                       catch (std::runtime_error& e)
                                                       {
                                                           Output::send(STR("[{}] {}\n"),
                                                                        ensure_str(action.type == LuaMod::ActionType::Loop ? "LoopAsync" : "DelayedAction"),
                                                                        ensure_str(e.what()));
                                                       }

                                                       return result;
                                                   }
                                                   else
                                                   {
                                                       return false;
                                                   }
                                               }),
                                m_delayed_actions.end());
    }

    auto LuaMod::clear_delayed_actions() -> void
    {
        actions_lock();
        m_pending_actions.clear();
        m_delayed_actions.clear();
        actions_unlock();
    }
} // namespace RC

#if PLATFORM_WINDOWS
#include <Unreal/Core/Windows/HideWindowsPlatformTypes.hpp>
#endif