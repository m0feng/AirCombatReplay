#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "SimTypes.generated.h" 

USTRUCT(BlueprintType)
struct FManifestData : public FTableRowBase
{
    GENERATED_BODY()

    //UPROPERTY(EditAnywhere, BlueprintReadWrite)
    //FString ID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Category;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Type;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Team;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString TrackFile;     // 对应CSV表头 TrackFile

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString ExplosionFile; // 对应CSV表头 ExplosionFile
};

// 轨迹数据 (Track_*.csv)
USTRUCT(BlueprintType)
struct FFlightData : public FTableRowBase
{
    GENERATED_BODY()

    //UPROPERTY(EditAnywhere, BlueprintReadWrite)
    //int32 FrameID = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Time = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 Active = 1; // CSV里是0或1，用int或bool都可以，int比较稳

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Lon = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Lat = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Alt = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Roll = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Pitch = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Yaw = 0.0;
};

// 爆炸数据 (Track_*_Explosion_*.csv)
USTRUCT(BlueprintType)
struct FExplosionData : public FTableRowBase
{
    GENERATED_BODY()

    //UPROPERTY(EditAnywhere, BlueprintReadWrite)
    //int32 FrameID = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Time = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 Explosion = 0; // 0=无, 1=爆炸

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Lon = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Lat = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Alt = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Roll = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Pitch = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Yaw = 0.0;
};