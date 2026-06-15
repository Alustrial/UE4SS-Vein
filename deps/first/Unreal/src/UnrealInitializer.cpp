#include <stdexcept>
#include <format>

#include <Helpers/Casting.hpp>
#include <SigScanner/SinglePassSigScanner.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/VersionedContainer/Container.hpp>
#include <Unreal/VersionedContainer/UnrealVirtualImpl/UnrealVirtualBaseVC.hpp>
#include <Unreal/UnrealVersion.hpp>
#include <Unreal/Signatures.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/FAssetData.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/AGameModeBase.hpp>
#include <Unreal/ULocalPlayer.hpp>
#include <Unreal/Searcher/ObjectSearcher.hpp>
#include <Unreal/ClassListener.hpp>
#include <Unreal/UGameViewportClient.hpp>
#include <Zydis/Zydis.h>

#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <Helpers/String.hpp>
#include <Helpers/SysError.hpp>

#include <Unreal/Hooks/Internal/ProcessEventProfiler.hpp>

namespace RC::Unreal::UnrealInitializer
{
    std::filesystem::path StaticStorage::GameExe;
    bool StaticStorage::bIsInitialized{false};
    bool StaticStorage::bVersionedContainerIsInitialized{false};
    Config StaticStorage::GlobalConfig{};
    bool StaticStorage::bPreInitCompleted{};
    bool StaticStorage::bScanFullyCompleted{};

    // These globals are explicitly not defined in a header file.
    // This is to force access via getter to catch if/when the game thread id is being used before it's been set.
    std::thread::id GGameThreadId{};
    bool GGameThreadIdInitialized{};

    auto HookedEngineTick(Hook::TCallbackIterationData<void>&, UEngine*, float, bool) -> void
    {
        if (GGameThreadId == std::thread::id{})
        {
            GGameThreadId = std::this_thread::get_id();
            GGameThreadIdInitialized = GGameThreadId != std::thread::id{};
            if (!GGameThreadIdInitialized)
            {
                Output::send<LogLevel::Error>(STR("Unable to retrieve ID of game thread\n"));
            }
        }
    }

    auto LoadExport(StringViewType Name) -> void*
    {
        void* symbol{};
        for (const auto& module_info : SigScannerStaticData::m_modules_info.array)
        {
            symbol = std::bit_cast<void*>(GetProcAddress(static_cast<HMODULE>(module_info.lpBaseOfDll), to_string(Name).c_str()));
            if (symbol)
            {
                break;
            }
        }
        return symbol;
    }

    auto LoadExport(std::string_view Name) -> void*
    {
        return LoadExport(ensure_str(Name));
    }

    auto SetupUnrealModules() -> void
    {
        // Setup all modules for the aob scanner
        MODULEINFO ModuleInfo;
        K32GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &ModuleInfo, sizeof(MODULEINFO));
        SigScannerStaticData::m_modules_info[ScanTarget::MainExe] = ModuleInfo;

        HMODULE Modules[1024];
        DWORD BytesRequired;

        if (K32EnumProcessModules(GetCurrentProcess(), Modules, sizeof(Modules), &BytesRequired) == 0)
        {
            throw std::runtime_error{fmt::format("Was unable to enumerate game modules. Error: {}",
                                                 to_string(SysError(GetLastError())).c_str())};
        }

        // Default all modules to be the same as MainExe
        // This is because most UE4 games only have the MainExe module
        for (size_t i = 0; i < static_cast<size_t>(ScanTarget::Max); ++i)
        {
            SigScannerStaticData::m_modules_info.array[i] = ModuleInfo;
        }

        // Check for modules and save the module info if they exist
        for (auto i = 0; i < BytesRequired / sizeof(HMODULE); ++i)
        {
            char ModuleRawName[MAX_PATH];
            // TODO: Fix an occasional error: "Call to K32GetModuleBaseNameA failed. Error Code: 6 (ERROR_INVALID_HANDLE)"
            if (K32GetModuleBaseNameA(GetCurrentProcess(), Modules[i], ModuleRawName, sizeof(ModuleRawName) / sizeof(char)) == 0)
            {
                continue;
            }

            std::string ModuleName{ModuleRawName};

            for (size_t i2 = 0; i2 < static_cast<size_t>(ScanTarget::Max); ++i2)
            {
                std::string ModuleToFind{"-"};
                ModuleToFind.append(ScanTargetToString(i2));
                ModuleToFind.append("-Win64-Shipping.dll");

                size_t Occurrence = ModuleName.find(ModuleToFind);
                if (Occurrence != ModuleName.npos)
                {
                    if (!SigScannerStaticData::m_is_modular) { SigScannerStaticData::m_is_modular = true; }

                    K32GetModuleInformation(GetCurrentProcess(), Modules[i], &SigScannerStaticData::m_modules_info[static_cast<ScanTarget>(i2)], sizeof(MODULEINFO));
                }
            }
        }
    }

    auto InitializeVersionedContainer() -> void
    {
        Container::SetDerivedBaseObjects();
        Container::UnrealVirtualVC->set_virtual_offsets();
        StaticStorage::bVersionedContainerIsInitialized = true;
    }

    auto static PostInitialize(const Config& UnrealConfig) -> void
    {
        if (!GMalloc)
        {
            throw std::runtime_error{"UnrealInitializer::PostInitialize: GMalloc is uninitialized."};
        }

        Output::send(STR("Post-initialization: GMalloc: {} -> {}\n"), (void*)GMalloc, (void*)*GMalloc);

        // FAssetData was not reflected before 4.17
        // We'll need to manually add FAssetData for every engine version eventually
        if (Version::IsAtLeast(4, 17))
        {
            if (FAssetData::FAssetDataAssumedStaticSize < FAssetData::StaticSize())
            {
                Output::send<LogLevel::Error>(STR("Tell a developer: FAssetData::StaticSize is too small to hold the entire struct. Assumed Size: {}; Found size: {}\n"), FAssetData::FAssetDataAssumedStaticSize, FAssetData::StaticSize());
            }
            bFAssetDataAvailable = true;
        }

        if (UnrealConfig.bUseUObjectArrayCache)
        {
            // Construct searcher pools
            AllSearcherPools.emplace(HashSearcherKey<UClass, AnySuperStruct>(), std::make_unique<ObjectSearcherPool<UClass, AnySuperStruct>>());
            AllSearcherPools.emplace(HashSearcherKey<UClass, AActor>(), std::make_unique<ObjectSearcherPool<UClass, AActor>>());
            AllSearcherPools.emplace(HashSearcherKey<AActor, AnySuperStruct>(), std::make_unique<ObjectSearcherPool<AActor, AnySuperStruct>>());

            // Populate searcher pools
            UObjectGlobals::ForEachUObject([](UObject* Object, ...) {
                auto* ObjectItem = Object->GetObjectItem();

                if (Object->IsA<UClass>())
                {
                    std::lock_guard<std::mutex> AnyPoolLock(ObjectSearcherPool<UClass, AnySuperStruct>::PoolMutex);
                    ObjectSearcherPool<UClass, AnySuperStruct>::Add(ObjectItem);

                    if (static_cast<UClass*>(Object)->IsChildOf<AActor>())
                    {
                        std::lock_guard<std::mutex> ActorPoolLock(ObjectSearcherPool<UClass, AActor>::PoolMutex);
                        ObjectSearcherPool<UClass, AActor>::Add(ObjectItem);
                    }
                }

                if (Object->IsA<AActor>())
                {
                    std::lock_guard<std::mutex> ActorInstPoolLock(ObjectSearcherPool<AActor, AnySuperStruct>::PoolMutex);
                    ObjectSearcherPool<AActor, AnySuperStruct>::Add(ObjectItem);
                }

                return LoopAction::Continue;
            });

            Output::send(STR("Adding GUObjectArray listeners\n"));
            UObjectArray::AddUObjectCreateListener(&FClassCreateListener::ClassCreateListener);
            UObjectArray::AddUObjectDeleteListener(&FClassDeleteListener::ClassDeleteListener);
        }

        StaticStorage::bIsInitialized = true;

    #ifdef UE_HOOK_TEST
        std::thread test_thread([](){
            Output::send(STR("Starting tests in 10 seconds\n"));
            std::this_thread::sleep_for(std::chrono::seconds(10));
            auto result = StartTests();
            Output::send<LogLevel::Verbose>(result ? STR("All tests passed!") : STR("Not all tests passed!"));
        });
        test_thread.detach();
    #else
        Output::send(STR("Starting callback garbage collector!"));
        Hook::StartCallbackGarbageCollector();
    #endif

        Hook::RegisterStaticConstructObjectPostCallback([](auto& data, auto& params) {
            if (UnrealInitializer::StaticStorage::bIsInitialized) {
                UObject* object = data.GetCurrentResolvedReturnValue();
                if(!object)
                {
                    Output::send<LogLevel::Warning>(STR("[{}.{}.{}] StaticConstructObject is set to return nullptr, not adding to ObjectSearcherPool!"),
                                                    STR("UE4SS"), STR("StaticConstructObject"), STR("ObjectSearcherPoolHook"));
                    return;
                }
                if (object->IsA<AActor>())
                {
                    std::lock_guard<std::mutex> ActorInstPoolLock(ObjectSearcherPool<AActor, AnySuperStruct>::PoolMutex);
                    ObjectSearcherPool<AActor, AnySuperStruct>::Add(object->GetObjectItem());
                }
            }
        }, {false, true, STR("UE4SS"), STR("ObjectSearcherPoolHook")});
    }

    struct PsScanConfig
    {
        bool g_uobject_array{};
        bool fname_tostring_fstring{};
        bool fname_ctor_wchar{};
        bool gmalloc{};
        bool static_construct_object_internal{};
        bool ftext_fstring{};
        bool engine_version{};
        bool fuobject_hash_tables_get{};
        bool gnatives{};
        bool console_manager_singleton{};
        bool gameengine_tick{};
    };

    struct PsCtx
    {
        void (*default_)(CharType* msg);
        void (*normal)(CharType* msg);
        void (*verbose)(CharType* msg);
        void (*warning)(CharType* msg);
        void (*error)(CharType* msg);
        PsScanConfig config{};
    };

    struct PsEngineVersion
    {
        uint16_t major{};
        uint16_t minor{};
    };

    struct PsScanResults
    {
        void* g_uobject_array{};
        void* fname_tostring_fstring{};
        void* fname_ctor_wchar{};
        void* gmalloc{};
        void* static_construct_object_internal{};
        void* ftext_fstring{};
        PsEngineVersion engine_version{};
        void* fuobject_hash_tables_get{};
        void* gnatives{};
        void* console_manager_singleton{};
        void* gameengine_tick{};
    };

    extern "C" {
        bool ps_scan(PsCtx& ctx, PsScanResults& results);
    }

    auto ScanGame() -> void
    {
        enum class OutputErrorsByThrowing { Yes, No };
        enum class ErrorsOnly { Yes, No };

        size_t scan_count{};

        const auto& UnrealConfig = StaticStorage::GlobalConfig;
        PsScanConfig config{};
        config.g_uobject_array = !UnrealConfig.ScanOverrides.guobjectarray;
        config.fname_tostring_fstring = !UnrealConfig.ScanOverrides.fname_to_string;
        config.fname_ctor_wchar = !UnrealConfig.ScanOverrides.fname_constructor;
        config.gmalloc = !UnrealConfig.ScanOverrides.fmemory_free;
        config.static_construct_object_internal = !UnrealConfig.ScanOverrides.static_construct_object;
        config.engine_version = !UnrealConfig.ScanOverrides.version_finder;
        config.fuobject_hash_tables_get = !UnrealConfig.ScanOverrides.fuobject_hash_tables_get;
        config.gnatives = !UnrealConfig.ScanOverrides.gnatives;
        config.console_manager_singleton = !UnrealConfig.ScanOverrides.console_manager_singleton;
        config.gameengine_tick = !UnrealConfig.ScanOverrides.gameengine_tick;

        PsCtx ctx {
            [](CharType* msg){ Output::send<LogLevel::Default>(STR("[PS] {}\n"), msg); },
            [](CharType* msg){ Output::send<LogLevel::Normal>(STR("[PS] {}\n"), msg); },
            [](CharType* msg){ Output::send<LogLevel::Verbose>(STR("[PS] {}\n"), msg); },
            [](CharType* msg){ Output::send<LogLevel::Warning>(STR("[PS] {}\n"), msg); },
            [](CharType* msg){ Output::send<LogLevel::Error>(STR("[PS] {}\n"), msg); },
            config,
        };

        PsScanResults results{};

        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            Output::send<LogLevel::Verbose>(STR("PS Scan attempt {} (Phase {})\n"), scan_count + 1, UnrealConfig.bIsForcedPreScan ? 1 : 2);

            if (ps_scan(ctx, results))
            {
                // All of the requested resolvers were found so break and continue
                Output::send<LogLevel::Default>(STR("PS scan successful\n"));
                break;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() > UnrealConfig.SecondsToScanBeforeGivingUp)
            {
                throw std::runtime_error{"PS scan timed out"};
            }
            ++scan_count;
        }

        auto OutputResult = [](Signatures::ScanResult& ScanResult, OutputErrorsByThrowing OutputErrorsByThrowing = OutputErrorsByThrowing::No, ErrorsOnly ErrorsOnly = ErrorsOnly::No) {
            if (ScanResult.Status == Signatures::ScanStatus::Failed)
            {
                std::string AllErrors{"AOB scans could not be completed because of the following reasons:\n"};
                std::string FatalErrors{};
                std::string NonFatalErrors{};
                for (const auto& Error : ScanResult.Errors)
                {
                    if (Error.bIsFatal)
                    {
                        FatalErrors.append(Error.Message + "\n\n");
                    }
                    else
                    {
                        NonFatalErrors.append(Error.Message + "\n\n");
                    }
                }

                AllErrors.append(FatalErrors);
                AllErrors.append(NonFatalErrors);

                if (!FatalErrors.empty() && OutputErrorsByThrowing == OutputErrorsByThrowing::Yes)
                {
                    throw std::runtime_error{AllErrors};
                }
                else
                {
                    Output::send(ensure_str(AllErrors));
                }
            }

            if (ErrorsOnly == ErrorsOnly::No)
            {
                for (const auto& SuccessMessage : ScanResult.SuccessMessage)
                {
                    Output::send(SuccessMessage);
                }

                for (const auto& InfoMessage : ScanResult.InfoMessages)
                {
                    Output::send(STR("Info: {}"), InfoMessage);
                }
            }
        };

        SinglePassScanner::m_scan_method = UnrealConfig.ScanMethod;
        auto DoScan = [&](auto ScannerFunction) {
            Signatures::ScanResult ScanResult{};
            size_t scan_count{};
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < UnrealConfig.SecondsToScanBeforeGivingUp)
            {
                Output::send<LogLevel::Verbose>(STR("Lua Scan attempt {} (Phase {})\n"), scan_count + 1, UnrealConfig.bIsForcedPreScan ? 1 : 2);

                ScanResult = ScannerFunction(UnrealConfig);
                OutputResult(ScanResult);

                bool bHasFatalError{};
                for (const auto& Error : ScanResult.Errors)
                {
                    if (Error.bIsFatal)
                    {
                        bHasFatalError = true;
                        break;
                    }
                }
                if (!bHasFatalError) { break; }
                ++scan_count;
                if (UnrealConfig.bIsForcedPreScan && scan_count > 0)
                {
                    break;
                }
            }

            OutputResult(ScanResult, OutputErrorsByThrowing::Yes, ErrorsOnly::Yes);
        };

        // First pass
        {
            if (ctx.config.engine_version)
            {
                Version::Major = results.engine_version.major;
                Version::Minor = results.engine_version.minor;
            }
            if (ctx.config.fname_tostring_fstring)
            {
                FName::ToStringInternal.assign_address(results.fname_tostring_fstring);
            }
            if (ctx.config.static_construct_object_internal)
            {
                UObjectGlobals::SetupStaticConstructObjectInternalAddress(results.static_construct_object_internal);
            }
            if (ctx.config.gameengine_tick)
            {
                UEngine::TickInternal.assign_address(results.gameengine_tick);
            }

            // If there are any overrides in the first pass then scan for them
            // engine_version omitted from this check as is not actually a scan and sets the version directly from the config
            if (!ctx.config.fname_tostring_fstring ||
                !ctx.config.static_construct_object_internal)
            {
                Output::send<LogLevel::Default>(STR("Running first pass of Lua override scans\n"));
                DoScan(&Signatures::ScanForGameFunctionsAndData);
            }
        }

        InitializeVersionedContainer();

        // Second pass
        {
            if (ctx.config.fname_ctor_wchar)
            {
                FName::ConstructorInternal.assign_address(results.fname_ctor_wchar);
            }
            if (ctx.config.gmalloc)
            {
                GMalloc = std::bit_cast<FMalloc**>(results.gmalloc);
            }
            if (ctx.config.g_uobject_array)
            {
                UObjectArray::SetupGUObjectArrayAddress(results.g_uobject_array);
            }
            if (ctx.config.gnatives)
            {
                GNatives_Internal = reinterpret_cast<FNativeFuncPtr*>(results.gnatives);
            }

            // If there are any overrides in the second pass then scan for them
            if (!ctx.config.fname_ctor_wchar ||
                !ctx.config.gmalloc ||
                !ctx.config.g_uobject_array ||
                !ctx.config.ftext_fstring ||
                !ctx.config.gnatives)
            {
                Output::send<LogLevel::Default>(STR("Running second pass of Lua override scans\n"));
                DoScan(&Signatures::ScanForGUObjectArray);
            }
        }

        StaticStorage::bScanFullyCompleted = true;
    }

    auto PreInitialize(const Config& UnrealConfig) -> void
    {
        StaticStorage::GlobalConfig = UnrealConfig;
        // Assume it's always a forced pre-scan, and let the caller override otherwise.
        StaticStorage::GlobalConfig.bIsForcedPreScan = true;
        SinglePassScanner::m_num_threads = UnrealConfig.NumScanThreads;
        SinglePassScanner::m_multithreading_module_size_threshold = UnrealConfig.MultithreadingModuleSizeThreshold;

        SetupUnrealModules();

        StaticStorage::bPreInitCompleted = true;
    }

    auto Initialize(const Config& In_UnrealConfig) -> void
    {
        if (!StaticStorage::bPreInitCompleted)
        {
            PreInitialize(In_UnrealConfig);
        }

        StaticStorage::GlobalConfig.bIsForcedPreScan = false;
        const auto& UnrealConfig = StaticStorage::GlobalConfig;

        if (!StaticStorage::bScanFullyCompleted)
        {
            ScanGame();
        }

        Output::send(STR("Using engine version: {}.{}\n"), Version::Major, Version::Minor);

        if (Version::IsDebug())
        {
            if (Version::IsAtLeast(4, 25))
            {
                Output::send(STR("Adding 0x{:X} bytes to the size of FUObjectItem due to game being debug build.\n"), sizeof(void*));
                FUObjectItem::UEP_TotalSize() += sizeof(void*);
            }
        }

        // Delay until enough elements have been constructed by the engine to the point where we know we can start constructing FNames.
        Output::send(STR("Waiting for object construction...\n"));
        while (UObjectArray::GetNumElements() < 10000)
        {
        }
        // We're assuming that KismetStringLibrary, KismetStringLibrary.Conv_NameToString, and the KismetStringLibrary CDO exists.
        // We will lock here forever if that's not the case.
        // Consider adding a limit to how long we can wait.
        Output::send(STR("Locating KismetSystemLibrary...\n"));
        UClass* KismetStringLibrary{};
        while (!KismetStringLibrary)
        {
            KismetStringLibrary = static_cast<UClass*>(UObjectGlobals::StaticFindObject_InternalNoToStringFromStrings({STR("/Script/Engine"), STR("KismetStringLibrary")}));
        }
        // For some games, it's found in GUObjectArray, and in other games, it's found in the function linked list.
        Output::send(STR("Locating KismetSystemLibrary:Conv_NameToString...\n"));
        while (!FName::Conv_NameToStringInternal)
        {
            FName::Conv_NameToStringInternal = KismetStringLibrary->GetFunctionByName(FName(STR("Conv_NameToString"), FNAME_Find));
            if (!FName::Conv_NameToStringInternal)
            {
                FName::Conv_NameToStringInternal = static_cast<UFunction*>(UObjectGlobals::StaticFindObject_InternalNoToStringFromStrings({STR("/Script/Engine"), STR("KismetStringLibrary"), STR("Conv_NameToString")}));
            }
        }
        Output::send(STR("Locating KismetSystemLibrary CDO...\n"));
        while (!FName::KismetStringLibraryCDO)
        {
            FName::KismetStringLibraryCDO = KismetStringLibrary->GetClassDefaultObject();
        }

        // Objects that are required to exist before we can continue
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("Class")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("Struct")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("Pawn")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("Character")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("Actor")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("Vector")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("Default__DefaultPawn")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("HitResult")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("Default__MaterialExpression")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("ActorComponent")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("OrientedBox")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("MovementComponent")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("HUD")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("PlayerController")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("PlayerCameraManager")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("EInterpCurveMode")});
        Hook::AddRequiredObject({STR("/Script/Engine"), STR("ENetRole")});
        Hook::AddRequiredObject({STR("/Script/MovieScene"), STR("MovieSceneEditorData")});
        Hook::AddRequiredObject({STR("/Script/UMG"), STR("Widget")});
        Hook::AddRequiredObject({STR("/Script/UMG"), STR("ComboBoxString")});
        Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("Interface")});
        if (Version::IsBelow(5, 4))
        {
            Hook::AddRequiredObject({STR("/Script/CoreUObject"), STR("DynamicClass")});
        }

        if (!Hook::AllRequiredObjectsConstructed())
        {
            for (int32_t i = 0; i < 2000 && !Hook::StaticStorage::bAllRequiredObjectsConstructed; ++i)
            {
                // The control variable for this loop is controlled from the game thread in a
                // hook created in the function call right above this loop

                if (Hook::StaticStorage::RequiredObjectsForInit.empty()) { break; }
                for (auto& RequiredObject : Hook::StaticStorage::RequiredObjectsForInit)
                {
                    if (Hook::StaticStorage::NumRequiredObjectsConstructed >= Hook::StaticStorage::RequiredObjectsForInit.size())
                    {
                        Hook::StaticStorage::bAllRequiredObjectsConstructed = true;
                        break;
                    }

                    if (RequiredObject.ObjectConstructed) { continue; }

                    UObject* required_object_ptr = UObjectGlobals::StaticFindObject_InternalNoToStringFromNames(RequiredObject.ObjectNameParts);
                    if (required_object_ptr)
                    {
                        RequiredObject.ObjectConstructed = true;
                        ++Hook::StaticStorage::NumRequiredObjectsConstructed;
                        Output::send(STR("Constructed [{} / {}]: {}\n"), Hook::StaticStorage::NumRequiredObjectsConstructed, Hook::StaticStorage::RequiredObjectsForInit.size(), RequiredObject.ObjectNameParts.back().ToString());
                    }
                }

                // Sleeping here will prevent this loop from getting optimized away
                // It will also prevent unnecessarily high CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        auto GetInstanceFromClass = [](const TCHAR* ClassName, const TCHAR* FallbackCDO) {
            auto Instance = UObjectGlobals::FindFirstOf(ClassName);
            if (!Instance)
            {
                Instance = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, FallbackCDO);
            }
            return Instance;
        };

        auto* Object = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/CoreUObject.Default__Object"));
        if (!Object)
        {
            throw std::runtime_error{"Post-initialization: Was unable to find 'CoreUObject.Default__Object' to use to retrieve the address of ProcessEvent"};
        }

        auto* Struct = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/CoreUObject.Default__Struct"));
        if (!Struct)
        {
            throw std::runtime_error{"Post-initialization: Was unable to find 'CoreUObject.Default__Struct' to use to retrieve the address of SetSuperStruct"};
        }

        auto* GameEngine = GetInstanceFromClass(STR("GameEngine"), STR("/Script/Engine.Default__GameEngine"));
        if (!GameEngine)
        {
            throw std::runtime_error{"Post-initialization: Was unable to find 'Engine.Default__GameEngine' to use to retrieve the address of LoadMap"};
        }

        // Some UE versions don't use GameModeBase (i.e: 4.13), so we must check both.
        // We can't just use GameMode for all games, because games that use GameModeBase often inherit directly from it instead of GameMode.
        auto* GameMode = GetInstanceFromClass(STR("GameModeBase"), STR("/Script/Engine.Default__GameModeBase"));
        if (!GameMode)
        {
            GameMode = GetInstanceFromClass(STR("GameMode"), STR("/Script/Engine.Default__GameMode"));
            if (!GameMode)
            {
                throw std::runtime_error{"Post-initialization: Was unable to find 'Engine.Default__GameModeBase' or 'Engine.Default__GameMode' to use to retrieve the address of InitGameState"};
            }
        }

        auto* Actor = UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, STR("/Script/Engine.Default__Actor"));
        if (!Actor)
        {
            throw std::runtime_error{"Post-initialization: Was unable to find 'Engine.Default__Actor' to use to retrieve the address of BeginPlay"};
        }

        auto* GameViewportClient = GetInstanceFromClass(STR("GameViewportClient"), STR("/Script/Engine.Default__GameViewportClient"));
        if (!GameViewportClient)
        {
            Output::send<LogLevel::Warning>(STR("Post-initialization: Was unable to find 'Engine.Default__GameViewportClient' to use to retrieve the address of UGameViewportClient::Tick"));
            StaticStorage::GlobalConfig.bHookGameViewportClientTick = false;
        }

        if (UnrealConfig.bHookLoadMap)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(UEngine, LoadMap, GameEngine); func_address)
            {
                Output::send(STR("GameEngine::LoadMap address {}\n"), func_address);
                UEngine::LoadMapInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookEngineTick)
        {
            auto vtable_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(UEngine, Tick, GameEngine);
            auto scan_address = UEngine::TickInternal.get_function_address();

            Output::send(STR("GameEngine::Tick address (vtable: {}; scan: {})\n"), vtable_address, scan_address);

            if (vtable_address && scan_address && vtable_address != scan_address)
            {
                Output::send<LogLevel::Warning>(STR("WARNING: VTable and scan addresses differ for UGameEngine::Tick. Potentially customized vtables.\n"));
                Output::send<LogLevel::Warning>(STR("GameEngine: {} {}\n"), static_cast<void*>(GameEngine), GameEngine->GetFullName());
                Output::send<LogLevel::Warning>(STR("VTable entries:\n"));

                if (const auto it = UEngine::VTableLayoutMap.find(L"Tick"); it == UEngine::VTableLayoutMap.end())
                {
                    Output::send<LogLevel::Error>(STR("Unable to find 'Tick' in UEngine VTable, cannot display values relative to Tick offset in vtable.\n"));
                }
                else
                {
                    const auto TickOffset = it->second;
                    for (int32_t Count = -8; Count <= 8; ++Count)
                    {
                        auto Offset = TickOffset + Count * sizeof(void*);
                        auto VTable = std::bit_cast<std::byte*>(*std::bit_cast<std::byte**>(GameEngine));
                        auto EntryPtr = std::bit_cast<uintptr_t>(VTable + Offset);
                        auto Entry = ASM::resolve_function_address_from_potential_jmp(*std::bit_cast<void**>(EntryPtr));
                        auto Line = fmt::format(STR("{}{}: {}\n"), Count > 0 ? STR("+") : STR(""), Count == 0 ? STR("S0") : fmt::format(STR("{}"), Count), Entry);
                        if (Entry == scan_address)
                        {
                            Output::send<Color::Green>(STR("{}"), Line);
                        }
                        else
                        {
                            Output::send(STR("{}"), Line);
                        }
                    }
                }
            }

            if (UnrealConfig.EngineTickResolveMethod == FunctionResolveMethod::Scan)
            {
                // Prefer scan, fallback to vtable
                if (scan_address)
                {
                    Output::send(STR("Using scan address for GameEngine::Tick\n"));
                    UEngine::TickInternal.assign_address(scan_address);
                }
                else if (vtable_address)
                {
                    Output::send(STR("Scan failed, falling back to vtable address for GameEngine::Tick\n"));
                    UEngine::TickInternal.assign_address(vtable_address);
                }
            }
            else // FunctionResolveMethod::VTable
            {
                // Prefer vtable, fallback to scan
                if (vtable_address)
                {
                    Output::send(STR("Using vtable address for GameEngine::Tick\n"));
                    UEngine::TickInternal.assign_address(vtable_address);
                }
                else if (scan_address)
                {
                    Output::send(STR("VTable lookup failed, falling back to scan address for GameEngine::Tick\n"));
                    UEngine::TickInternal.assign_address(scan_address);
                }
            }
            Hook::RegisterEngineTickPreCallback(HookedEngineTick, {true, false, STR("UE4SS"), STR("GameThreadInitializer")});
        }
        if (UnrealConfig.bHookInitGameState)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(AGameModeBase, InitGameState, GameMode); func_address)
            {
                Output::send(STR("GameModeBase::InitGameState address {}\n"), func_address);
                AGameModeBase::InitGameStateInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookBeginPlay)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(AActor, BeginPlay, Actor); func_address)
            {
                Output::send(STR("AActor::BeginPlay address {}\n"), func_address);
                AActor::BeginPlayInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookEndPlay)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(AActor, EndPlay, Actor); func_address)
            {
                Output::send(STR("AActor::EndPlay address {}\n"), func_address);
                AActor::EndPlayInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookAActorTick)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(AActor, Tick, Actor); func_address)
            {
                Output::send(STR("AActor::Tick address {}\n"), func_address);
                AActor::TickInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookGameViewportClientTick)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(UGameViewportClient, Tick, GameViewportClient); func_address)
            {
                Output::send(STR("GameViewportClient::Tick address {}\n"), func_address);
                UGameViewportClient::TickInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookUObjectProcessEvent)
        {
            if (auto func_address = OPTIONAL_GET_ADDRESS_OF_UNREAL_VIRTUAL(UObject, ProcessEvent, Object); func_address)
            {
                Output::send(STR("ProcessEvent address {}\n"), func_address);
                UObject::ProcessEventInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookProcessConsoleExec)
        {
            if (auto func_address = GET_ADDRESS_OF_UNREAL_VIRTUAL(UObject, ProcessConsoleExec, Object); func_address)
            {
                Output::send(STR("ProcessConsoleExec address {}\n"), func_address);
                UObject::ProcessConsoleExecInternal.assign_address(func_address);
            }
        }
        if (UnrealConfig.bHookUStructLink)
        {
            if (auto func_address = GET_ADDRESS_OF_UNREAL_VIRTUAL(UStruct, Link, Struct); func_address)
            {
                Output::send(STR("UStruct::Link address {}\n"), func_address);
                UStruct::LinkInternal.assign_address(func_address);
            }
        }

        TypeChecker::store_all_object_names();

        Output::send(STR("Constructed {} of {} objects\n"), Hook::StaticStorage::NumRequiredObjectsConstructed, Hook::StaticStorage::RequiredObjectsForInit.size());
        if (!Hook::StaticStorage::bAllRequiredObjectsConstructed)
        {
            Output::send(STR("Fatal error! The following objects were never constructed:\n"));
            for (const auto& RequiredObject : Hook::StaticStorage::RequiredObjectsForInit)
            {
                if (RequiredObject.ObjectConstructed) { continue; }
                Output::send(STR("{}\n"), RequiredObject.ObjectNameParts.back().ToString());
            }
            throw std::runtime_error{""};
        }

        if (!TypeChecker::store_all_object_types())
        {
            throw std::runtime_error{"TypeChecker: Was unable to find some or all of the required core objects"};
        }

        if (UnrealConfig.bHookProcessInternal || UnrealConfig.bHookProcessLocalScriptFunction)
        {
            auto ExecuteUbergraphFunction = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/CoreUObject.Object:ExecuteUbergraph"));
            if (!ExecuteUbergraphFunction)
            {
                throw std::runtime_error{"Was unable to find locate ProcessInternal because '/Script/CoreUObject.Object:ExecuteUbergraph' wasn't found in GUObjectArray"};
            }
            auto ProcessInternal = ExecuteUbergraphFunction->GetFuncPtr();
            ProcessInternal = std::bit_cast<decltype(ProcessInternal)>(ASM::resolve_function_address_from_potential_jmp(std::bit_cast<void*>(ProcessInternal)));
            // Only assign ProcessInternalInternal if no override exists, allowing Lua override to take precedence
            if (!UnrealConfig.ScanOverrides.process_internal)
            {
                UObject::ProcessInternalInternal.assign_address(ProcessInternal);
                // Log ProcessInternal address only for built-in detection
                Output::send(STR("ProcessInternal address: {}\n"), static_cast<void*>(ProcessInternal));
            }

            // Skip ProcessLocalScriptFunction detection if override exists, preserving built-in logic otherwise
            if (!UnrealConfig.ScanOverrides.process_local_script_function && Version::IsAtLeast(4, 22))
            {
                // Use the final ProcessInternal address (overridden or built-in) for disassembly
                auto process_internal_addr = UObject::ProcessInternalInternal.get_function_address();
                if (!process_internal_addr)
                {
                    Output::send(STR("Error: ProcessInternal address is null, cannot compute ProcessLocalScriptFunction\n"));
                }
                else
                {
                    int CallCount{};
                    auto Data = std::bit_cast<ZyanU8*>(process_internal_addr);
                    ZydisDecoder Decoder;
                    ZydisDecoderInit(&Decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                    ZyanU64 RuntimeAddress = std::bit_cast<ZyanU64>(process_internal_addr);
                    ZyanUSize Offset = 0;
                    const ZyanUSize NumBytesToDecode = 164;
                    ZydisDecodedInstruction Instruction;
                    ZydisDecodedOperand Operands[10]{};
                    while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder, Data + Offset, NumBytesToDecode - Offset, &Instruction, Operands)))
                    {
                        if (Instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
                        {
                            ++CallCount;
                        }
                        if (CallCount == 3)
                        {
                            auto FunctionPtr = ASM::resolve_function_address_from_potential_jmp(std::bit_cast<void*>(RuntimeAddress));
                            if (FunctionPtr)
                            {
                                UObject::ProcessLocalScriptFunctionInternal.assign_address(FunctionPtr);
                                // Log ProcessLocalScriptFunction address only for built-in detection
                                Output::send(STR("ProcessLocalScriptFunction address: {}\n"), static_cast<void*>(FunctionPtr));
                            }
                            break;
                        }
                        Offset += Instruction.length;
                        RuntimeAddress += Instruction.length;
                    }
                }
            }
        }

        Output::send<LogLevel::Verbose>(STR("UnrealConfig.FExecVTableOffsetInLocalPlayer: {:X}\n"), UnrealConfig.FExecVTableOffsetInLocalPlayer);

        PostInitialize(UnrealConfig);
    }
}

namespace RC::Unreal
{
    auto GetGameThreadId() -> std::thread::id
    {
        if (!UnrealInitializer::GGameThreadIdInitialized)
        {
            throw std::runtime_error{"GetGameThreadId called too early, not yet initialized by UGameEngine::Tick"};
        }
        return UnrealInitializer::GGameThreadId;
    }

    auto IsInGameThread() -> bool
    {
        return std::this_thread::get_id() == GetGameThreadId();
    }

    auto IsGameThreadInitialized() noexcept-> bool
    {
        return UnrealInitializer::GGameThreadIdInitialized;
    }

    auto GetGameThreadIdRaw() noexcept -> std::thread::id
    {
        if (!UnrealInitializer::GGameThreadIdInitialized)
        {
            return {};
        }
        return UnrealInitializer::GGameThreadId;
    }

    auto IsInGameThreadRaw() noexcept -> bool
    {
        return IsGameThreadInitialized() && std::this_thread::get_id() == GetGameThreadId();
    }
}
