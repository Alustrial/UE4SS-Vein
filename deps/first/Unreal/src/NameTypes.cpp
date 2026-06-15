#include <Unreal/NameTypes.hpp>

#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/Core/Containers/FUtf8String.hpp>
#include <Unreal/Core/Containers/FAnsiString.hpp>
#include <Unreal/BPMacros.hpp>
#include <Unreal/UnrealInitializer.hpp>

namespace RC::Unreal
{
    Function<void(const FName*, class FStringOut&)> FName::ToStringInternal;
    UFunction* FName::Conv_NameToStringInternal{};
    UObject* FName::KismetStringLibraryCDO{};
    Function<FName(const CharType*, EFindName)> FName::ConstructorInternal;

    /** An unpacked FNameEntryId */
    struct FNameEntryHandle
    {
        uint32 Block = 0;
        uint32 Offset = 0;

        FNameEntryHandle(uint32 InBlock, uint32 InOffset)
            : Block(InBlock)
            , Offset(InOffset)
        {
            checkName(Block < FNameMaxBlocks);
            checkName(Offset < FNameBlockOffsets);
        }

        FNameEntryHandle(FNameEntryId Id)
            : Block(Id.ToUnstableInt() >> FNameBlockOffsetBits)
            , Offset(Id.ToUnstableInt() & (FNameBlockOffsets - 1))
        {
        }

        operator FNameEntryId() const
        {
            return FNameEntryId::FromUnstableInt(Block << FNameBlockOffsetBits | Offset);
        }

        explicit operator bool() const { return Block | Offset; }
    };

    static uint32 GetTypeHash(FNameEntryHandle Handle)
    {
        return (Handle.Block << (32 - FNameMaxBlockBits)) + Handle.Block // Let block index impact most hash bits
            + (Handle.Offset << FNameBlockOffsetBits) + Handle.Offset // Let offset impact most hash bits
            + (Handle.Offset >> 4); // Reduce impact of non-uniformly distributed entry name lengths
    }

    uint32 GetTypeHash(FNameEntryId Id)
    {
        return GetTypeHash(FNameEntryHandle(Id));
    }

    // Special wrapper for 'UKismetStringLibrary::Conv_NameToString' that doesn't use StaticFindObject and is safe to use during init.
    FString UKismetStringLibrary_Conv_NameToString(FName InName)
    {
        // We make some assumptions in this function to increase performance.
        // We assume that the CDO, and function always exist and never change or get reallocated.
        // If you use BPMacros, there will be a lot of error checking, but since FName::ToString is a very hot function,
        // we need to avoid as much overhead as possible.

        static const auto ParamStructSize = FName::Conv_NameToStringInternal->GetParmsSize();
        auto ParamData = static_cast<uint8*>(_malloca(ParamStructSize));
        FMemory::Memzero(ParamData, ParamStructSize);

        static const auto Offset = []() {
            return FName::Conv_NameToStringInternal->FindProperty(FName(L"InName", FNAME_Find))->GetOffset_Internal();
        }();
        *std::bit_cast<FName*>(&ParamData[Offset]) = InName;

        FName::KismetStringLibraryCDO->ProcessEvent(FName::Conv_NameToStringInternal, ParamData);

        static const auto ReturnOffset = []() {
            return FName::Conv_NameToStringInternal->GetReturnProperty()->GetOffset_Internal();
        }();
        const auto RetValue = *std::bit_cast<FString*>(&ParamData[ReturnOffset]);
        _freea(ParamData);
        return RetValue;
    }

    const StringType ToStringInternalWrapper_UsingScan(const FName* name)
    {
        FStringOut string{};
        FName::ToStringInternal(name, string);

        StringType name_string{*string ? *string : STR("UE4SS_None")};
        return name_string;
    }

    const StringType ToStringInternalWrapper_UsingConv_NameToString(const FName* name)
    {
        const auto string = UKismetStringLibrary_Conv_NameToString(*name);

        StringType name_string{*string ? *string : STR("UE4SS_None")};
        return name_string;
    }

    const StringType ToStringInternalWrapper(const FName* name)
    {
        using namespace UnrealInitializer;
        if (StaticStorage::GlobalConfig.FNameToStringMethod == FNameToStringMethod::Scan && FName::ToStringInternal.is_ready())
        {
            return ToStringInternalWrapper_UsingScan(name);
        }
        else if (FName::Conv_NameToStringInternal)
        {
            return ToStringInternalWrapper_UsingConv_NameToString(name);
        }
        else
        {
            throw std::runtime_error{"FName::ToString was not ready but was called anyway"};
        }
    }

    auto FName::ToString() -> StringType
    {
        return ToStringInternalWrapper(this);
    }

    auto FName::ToString() const -> const StringType
    {
        return ToStringInternalWrapper(this);
    }

    // Returns FString (TCHAR-based)
    FString FName::ToFString() const
    {
        StringType str = ToString();
        return FString(str);
    }

    FUtf8String FName::ToFUtf8String() const
    {
        StringType str = ToString();
        return FUtf8String(str);
    }

    FAnsiString FName::ToFAnsiString() const
    {
        StringType str = ToString();
        return FAnsiString(str);
    }

    uint32 FName::GetPlainNameString(TCHAR(&OutName)[NAME_SIZE])
    {
        const uint32 Entry = GetDisplayIndex().ToUnstableInt();
        auto String = FName(Entry).ToString();
        std::memcpy(OutName, &String[0], String.size() * sizeof(File::StringType::size_type));
        return static_cast<uint32>(String.size());
    }
}
