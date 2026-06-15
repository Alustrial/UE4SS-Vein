#pragma once

#include <NameTypes.hpp>
#include <Unreal/Core/Templates/TypeHash.hpp>

namespace RC::Unreal
{
    struct FPrimaryAssetType
    {
        FName Name{};

        bool operator==(const FPrimaryAssetType& Other) const
        {
            return Name == Other.Name;
        }

        bool operator!=(const FPrimaryAssetType& Other) const
        {
            return !(*this == Other);
        }
    };

    inline uint32 GetTypeHash(const FPrimaryAssetType& Type)
    {
        return GetTypeHash(Type.Name);
    }

    struct FPrimaryAssetId
    {
        FPrimaryAssetType PrimaryAssetType{};
        FName PrimaryAssetName{};

        FPrimaryAssetType GetPrimaryAssetType()
        {
            return PrimaryAssetType;
        }

        FName GetPrimaryAssetName()
        {
            return PrimaryAssetName;
        }

        bool operator==(const FPrimaryAssetId& Other) const
        {
            return PrimaryAssetType == Other.PrimaryAssetType && PrimaryAssetName == Other.PrimaryAssetName;
        }

        bool operator!=(const FPrimaryAssetId& Other) const
        {
            return !(*this == Other);
        }
    };

    inline uint32 GetTypeHash(const FPrimaryAssetId& Id)
    {
        uint32 Hash = GetTypeHash(Id.PrimaryAssetType);
        Hash = HashCombine(Hash, GetTypeHash(Id.PrimaryAssetName));
        return Hash;
    }
}
