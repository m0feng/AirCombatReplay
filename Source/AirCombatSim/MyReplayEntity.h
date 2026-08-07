#pragma once

#include "CoreMinimal.h"
#include "CesiumGeoreference.h"
#include "GameFramework/Actor.h"
#include "SimTypes.h"
#include "MyReplayEntity.generated.h"

class UTextRenderComponent;
class UCanvas;
class APlayerController;

UCLASS()
class AIRCOMBATSIM_API AMyReplayEntity : public AActor
{
    GENERATED_BODY()

public:
    AMyReplayEntity();

    void InitializeEntity(
        UDataTable* InTrackTable,
        UDataTable* InExplosionTable,
        ACesiumGeoreference* InGeoRef,
        const FString& InEntityId,
        const FString& InEntityType,
        const FString& InEntityCategory,
        const FString& InTeam
    );

    void UpdateReplay(float InReplayTime, float DeltaTime);
    void ResetReplayState();
    void SetSelected(bool bInSelected);
    void DrawTrailOverlay(UCanvas* Canvas, APlayerController* PlayerController, float UIScale) const;
    void SetLabelLayoutOffset(
        const FVector2D& InLayoutOffset,
        const FVector& CameraLocation,
        const FRotator& CameraRotation
    );

    float GetReplayDuration() const { return ReplayDuration; }
    const FString& GetEntityId() const { return EntityId; }
    const FString& GetEntityType() const { return EntityType; }
    const FString& GetEntityCategory() const { return EntityCategory; }
    const FString& GetTeam() const { return EntityTeam; }
    bool IsCurrentlyActive() const { return bCurrentlyActive; }
    bool HasExploded() const { return bHasExploded; }
    double GetCurrentAltitudeMeters() const { return CurrentAltitudeMeters; }
    double GetCurrentSpeedMetersPerSecond() const { return CurrentSpeedMetersPerSecond; }
    const FRotator& GetCurrentReplayRotation() const { return CurrentReplayRotation; }

    // --- 远距离模型缩放 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale")
    bool bEnableDynamicScale = true;

    // Preserve a readable model size as the tactical camera zooms. This scales
    // the original mesh itself; it does not add a marker or replacement model.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale")
    bool bUseScreenSpaceScale = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale", meta = (ClampMin = "4.0", ClampMax = "128.0"))
    float TargetScreenSizePixels = 56.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale", meta = (ClampMin = "0.1"))
    float ScreenSpaceScaleInterpSpeed = 6.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale")
    float MinScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale")
    float MaxScale = 600.0f;

    // Legacy distance mapping used only when bUseScreenSpaceScale is disabled.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale")
    float ScaleDistanceThreshold = 500000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual Scale", meta = (ClampMin = "1.0"))
    float SelectedScaleMultiplier = 1.2f;

    // --- ID / 高度标注 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label")
    bool bShowEntityLabel = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label", meta = (ClampMin = "1.0"))
    float LabelWorldSize = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label", meta = (ClampMin = "100.0"))
    float LabelReferenceDistance = 8000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label", meta = (ClampMin = "0.1"))
    float LabelMinScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label", meta = (ClampMin = "1.0"))
    float LabelMaxScale = 1200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Entity Label", meta = (ClampMin = "0.0"))
    float LabelScreenOffset = 140.0f;

    // --- 最近一段时间的渐隐轨迹 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail")
    bool bEnableTrail = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail", meta = (ClampMin = "1.0", Units = "s"))
    float TrailDuration = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail", meta = (ClampMin = "0.05", Units = "s"))
    float TrailSampleInterval = 0.1f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail", meta = (ClampMin = "0.016", Units = "s"))
    float TrailDrawInterval = 1.0f / 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail", meta = (ClampMin = "0.1"))
    float TrailThickness = 3.0f;

    // 模型缩放前，从 Actor 中心沿后方移动到机尾的距离。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trail", meta = (ClampMin = "0.0"))
    float TrailTailOffset = 600.0f;

    // --- 垂直投影与地面圆盘 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
    bool bEnableProjectionLine = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
    FColor ProjectionLineColor = FColor::Yellow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
    float ProjectionCircleRadius = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (Units = "m"))
    double ProjectionTraceEndHeight = -1000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.016"))
    float ProjectionUpdateInterval = 0.1f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0"))
    float ProjectionLineThickness = 2.0f;

    // --- 爆炸特效 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VFX")
    TSubclassOf<AActor> ExplosionActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VFX", meta = (ClampMin = "0.0", Units = "s"))
    float ExplosionLifeSpan = 5.0f;

    // CSV 没有有效 Time 时才使用这个值。
    UPROPERTY(EditAnywhere, Category = "Replay Config", meta = (ClampMin = "0.001", Units = "s"))
    float SampleRate = 0.1f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    USceneComponent* RootScene;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Entity Label")
    UTextRenderComponent* EntityLabel;

protected:
    virtual void BeginPlay() override;

    UFUNCTION(BlueprintImplementableEvent)
    void OnTeamColorChanged(const FString& Team);

private:
    struct FTrailPoint
    {
        float Time = 0.0f;
        FVector Location = FVector::ZeroVector;
    };

    void UpdateTrail(float ReplayTime, float DeltaTime, bool bAddCurrentPoint, const FVector& CurrentLocation);
    void RefreshLabelColor();
    void RefreshLabelTransform(
        const FVector& EntityLocation,
        const FVector& CameraLocation,
        const FRotator& CameraRotation,
        bool bHasCameraLocation
    );
    float GetUnscaledVisualBoundsRadius();

    UPROPERTY()
    UDataTable* FlightDataTable = nullptr;

    UPROPERTY()
    UDataTable* ExplosionDataTable = nullptr;

    UPROPERTY()
    ACesiumGeoreference* GeoRef = nullptr;

    UPROPERTY()
    TArray<AActor*> SpawnedExplosionActors;

    FString EntityId;
    FString EntityType;
    FString EntityCategory;
    FString EntityTeam;
    FColor BaseLabelColor = FColor::White;
    FColor BaseTrailColor = FColor::White;
    FVector2D LabelLayoutOffset = FVector2D::ZeroVector;
    float CachedUnscaledVisualBoundsRadius = 0.0f;
    bool bVisualScaleInitialized = false;
    TArray<FTrailPoint> TrailPoints;

    float DataSampleInterval = 0.1f;
    float ReplayDuration = 0.0f;
    int32 TrackRowCount = 0;
    float LastReplayTime = -1.0f;
    float LastMotionSampleTime = -1.0f;
    float ProjectionUpdateAccumulator = 0.0f;
    float TrailDrawAccumulator = 0.0f;
    FVector LastMotionLocation = FVector::ZeroVector;

    double CurrentAltitudeMeters = 0.0;
    double CurrentSpeedMetersPerSecond = 0.0;
    FRotator CurrentReplayRotation = FRotator::ZeroRotator;

    bool bIsInitialized = false;
    bool bCurrentlyActive = false;
    bool bHasExploded = false;
    bool bIsSelected = false;
    int32 LastDisplayedAltitudeMeters = TNumericLimits<int32>::Lowest();
    int32 LastExplodedFrameIndex = -1;
};
