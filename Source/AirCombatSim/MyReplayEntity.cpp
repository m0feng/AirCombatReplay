#include "MyReplayEntity.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/MeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Canvas.h"
#include "GameFramework/PlayerController.h"

AMyReplayEntity::AMyReplayEntity()
{
    PrimaryActorTick.bCanEverTick = false;

    RootScene = CreateDefaultSubobject<USceneComponent>(TEXT("RootScene"));
    RootComponent = RootScene;

    EntityLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("EntityLabel"));
    EntityLabel->SetupAttachment(RootScene);
    EntityLabel->SetAbsolute(true, true, true);
    EntityLabel->SetHorizontalAlignment(EHTA_Center);
    EntityLabel->SetVerticalAlignment(EVRTA_TextCenter);
    EntityLabel->SetWorldSize(LabelWorldSize);
    EntityLabel->SetTextRenderColor(FColor::White);
    EntityLabel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    EntityLabel->SetGenerateOverlapEvents(false);
    EntityLabel->SetCastShadow(false);
    EntityLabel->SetTranslucentSortPriority(100);
    EntityLabel->bAlwaysRenderAsText = true;
}

void AMyReplayEntity::BeginPlay()
{
    Super::BeginPlay();
    SetActorEnableCollision(false);
    EntityLabel->SetWorldSize(LabelWorldSize);
    EntityLabel->SetVisibility(false, true);
}

void AMyReplayEntity::InitializeEntity(
    UDataTable* InTrackTable,
    UDataTable* InExplosionTable,
    ACesiumGeoreference* InGeoRef,
    const FString& InEntityId,
    const FString& InEntityType,
    const FString& InEntityCategory,
    const FString& InTeam
)
{
    FlightDataTable = InTrackTable;
    ExplosionDataTable = InExplosionTable;
    GeoRef = InGeoRef;
    EntityId = InEntityId;
    EntityType = InEntityType;
    EntityCategory = InEntityCategory;
    EntityTeam = InTeam;
    LabelLayoutOffset = FVector2D::ZeroVector;

    if (EntityTeam.Equals(TEXT("Red"), ESearchCase::IgnoreCase))
    {
        BaseLabelColor = FColor(255, 96, 96);
        BaseTrailColor = FColor(255, 48, 48);
    }
    else if (EntityTeam.Equals(TEXT("Blue"), ESearchCase::IgnoreCase))
    {
        BaseLabelColor = FColor(176, 240, 255);
        BaseTrailColor = FColor(32, 160, 255);
    }
    else
    {
        BaseLabelColor = FColor::White;
        BaseTrailColor = FColor::White;
    }

    DataSampleInterval = FMath::Max(SampleRate, 0.001f);
    ReplayDuration = 0.0f;

    if (FlightDataTable)
    {
        static const FString ContextString(TEXT("ReplayInitialization"));
        TrackRowCount = FlightDataTable->GetRowNames().Num();
        const int32 RowCount = TrackRowCount;
        const FFlightData* Row0 = FlightDataTable->FindRow<FFlightData>(FName(TEXT("0")), ContextString);
        const FFlightData* Row1 = FlightDataTable->FindRow<FFlightData>(FName(TEXT("1")), ContextString);
        const FFlightData* LastRow = RowCount > 0
            ? FlightDataTable->FindRow<FFlightData>(FName(*FString::FromInt(RowCount - 1)), ContextString)
            : nullptr;

        if (Row0 && Row1 && Row1->Time > Row0->Time)
        {
            DataSampleInterval = Row1->Time - Row0->Time;
        }

        if (LastRow && LastRow->Time > 0.0f)
        {
            ReplayDuration = LastRow->Time;
        }
        else if (RowCount > 1)
        {
            ReplayDuration = (RowCount - 1) * DataSampleInterval;
        }
    }

    bIsInitialized = true;
    ResetReplayState();
    RefreshLabelColor();
    OnTeamColorChanged(EntityTeam);
}

void AMyReplayEntity::ResetReplayState()
{
    for (AActor* ExplosionActor : SpawnedExplosionActors)
    {
        if (IsValid(ExplosionActor))
        {
            ExplosionActor->Destroy();
        }
    }
    SpawnedExplosionActors.Reset();

    TrailPoints.Reset();
    LastReplayTime = -1.0f;
    LastMotionSampleTime = -1.0f;
    ProjectionUpdateAccumulator = ProjectionUpdateInterval;
    TrailDrawAccumulator = TrailDrawInterval;
    CurrentSpeedMetersPerSecond = 0.0;
    bCurrentlyActive = false;
    bHasExploded = false;
    LastDisplayedAltitudeMeters = TNumericLimits<int32>::Lowest();
    LastExplodedFrameIndex = -1;
    bVisualScaleInitialized = false;
    SetActorHiddenInGame(true);
    SetActorEnableCollision(false);
    EntityLabel->SetVisibility(false, true);
}

void AMyReplayEntity::SetSelected(bool bInSelected)
{
    bIsSelected = bInSelected;
    RefreshLabelColor();
}

void AMyReplayEntity::RefreshLabelColor()
{
    EntityLabel->SetTextRenderColor(bIsSelected ? FColor::Yellow : BaseLabelColor);
}

float AMyReplayEntity::GetUnscaledVisualBoundsRadius()
{
    if (CachedUnscaledVisualBoundsRadius > KINDA_SMALL_NUMBER)
    {
        return CachedUnscaledVisualBoundsRadius;
    }

    FBox CombinedBounds(ForceInit);
    TInlineComponentArray<UMeshComponent*> MeshComponents(this);
    for (const UMeshComponent* MeshComponent : MeshComponents)
    {
        if (IsValid(MeshComponent) && MeshComponent->IsRegistered())
        {
            CombinedBounds += MeshComponent->Bounds.GetBox();
        }
    }

    if (!CombinedBounds.IsValid)
    {
        return 0.0f;
    }

    const float CurrentUniformScale = FMath::Max(
        GetActorScale3D().GetAbsMax(),
        KINDA_SMALL_NUMBER
    );
    CachedUnscaledVisualBoundsRadius =
        CombinedBounds.GetExtent().Size() / CurrentUniformScale;
    return CachedUnscaledVisualBoundsRadius;
}

void AMyReplayEntity::SetLabelLayoutOffset(
    const FVector2D& InLayoutOffset,
    const FVector& CameraLocation,
    const FRotator& CameraRotation
)
{
    LabelLayoutOffset = InLayoutOffset;
    RefreshLabelTransform(GetActorLocation(), CameraLocation, CameraRotation, true);
}

void AMyReplayEntity::RefreshLabelTransform(
    const FVector& EntityLocation,
    const FVector& CameraLocation,
    const FRotator& CameraRotation,
    bool bHasCameraLocation
)
{
    if (!EntityLabel || !bShowEntityLabel)
    {
        return;
    }

    if (bHasCameraLocation)
    {
        const float CameraDistance = FVector::Dist(EntityLocation, CameraLocation);
        const float EffectiveLabelReferenceDistance = FMath::Max(LabelReferenceDistance, 8000.0f);
        const float LabelScale = FMath::Clamp(
            CameraDistance / EffectiveLabelReferenceDistance,
            LabelMinScale,
            FMath::Max(LabelMaxScale, LabelMinScale)
        );
        const FVector CameraUp = CameraRotation.RotateVector(FVector::UpVector).GetSafeNormal(
            SMALL_NUMBER,
            FVector::UpVector
        );
        const FVector CameraRight = CameraRotation.RotateVector(FVector::RightVector).GetSafeNormal(
            SMALL_NUMBER,
            FVector::RightVector
        );
        const FVector LabelLocation = EntityLocation
            + CameraUp * (LabelScreenOffset + LabelLayoutOffset.Y) * LabelScale
            + CameraRight * LabelLayoutOffset.X * LabelScale;
        const FVector ToCamera = (CameraLocation - LabelLocation).GetSafeNormal(
            SMALL_NUMBER,
            FVector::ForwardVector
        );
        const FRotator LabelRotation = FRotationMatrix::MakeFromXZ(ToCamera, CameraUp).Rotator();

        EntityLabel->SetWorldLocation(LabelLocation);
        EntityLabel->SetWorldRotation(LabelRotation);
        EntityLabel->SetWorldScale3D(FVector(LabelScale));
    }
    else
    {
        EntityLabel->SetWorldLocation(EntityLocation + FVector::UpVector * LabelScreenOffset);
        EntityLabel->SetWorldScale3D(FVector::OneVector);
    }
}

void AMyReplayEntity::DrawTrailOverlay(
    UCanvas* Canvas,
    APlayerController* PlayerController,
    float UIScale
) const
{
    if (!Canvas || !PlayerController || !bEnableTrail || !bCurrentlyActive || TrailPoints.Num() < 2)
    {
        return;
    }

    const bool bRedTeam = EntityTeam.Equals(TEXT("Red"), ESearchCase::IgnoreCase);
    const FLinearColor TeamColor = bRedTeam
        ? FLinearColor(1.0f, 0.035f, 0.02f, 1.0f)
        : FLinearColor(0.02f, 0.42f, 1.0f, 1.0f);
    const float EffectiveDuration = FMath::Max(TrailDuration, 0.1f);
    const float OuterThickness = FMath::Clamp(TrailThickness * 0.75f * UIScale, 1.6f, 3.5f);
    const float CoreThickness = FMath::Max(OuterThickness * 0.45f, 0.9f);
    const int32 LastDrawablePoint = TrailPoints.Num() - 1;
    for (int32 PointIndex = 1; PointIndex <= LastDrawablePoint; ++PointIndex)
    {
        FVector2D ScreenStart;
        FVector2D ScreenEnd;
        if (!PlayerController->ProjectWorldLocationToScreen(
                TrailPoints[PointIndex - 1].Location,
                ScreenStart,
                true
            ) ||
            !PlayerController->ProjectWorldLocationToScreen(
                TrailPoints[PointIndex].Location,
                ScreenEnd,
                true
            ))
        {
            continue;
        }

        const float Age = LastReplayTime - TrailPoints[PointIndex].Time;
        const float Freshness = 1.0f - FMath::Clamp(Age / EffectiveDuration, 0.0f, 1.0f);
        FLinearColor OuterColor = TeamColor;
        OuterColor.A = FMath::Lerp(0.22f, 0.72f, Freshness);
        FLinearColor CoreColor = FLinearColor::LerpUsingHSV(
            TeamColor,
            FLinearColor::White,
            0.18f
        );
        CoreColor.A = FMath::Lerp(0.38f, 1.0f, Freshness);

        Canvas->K2_DrawLine(ScreenStart, ScreenEnd, OuterThickness, OuterColor);
        Canvas->K2_DrawLine(ScreenStart, ScreenEnd, CoreThickness, CoreColor);
    }
}

void AMyReplayEntity::UpdateTrail(
    float ReplayTime,
    float DeltaTime,
    bool bAddCurrentPoint,
    const FVector& CurrentLocation
)
{
    if (!bEnableTrail)
    {
        TrailPoints.Reset();
        return;
    }

    const float EffectiveDuration = FMath::Max(TrailDuration, 0.1f);
    const float EffectiveSampleInterval = FMath::Max(TrailSampleInterval, 0.05f);
    const float EffectiveDrawInterval = FMath::Max(TrailDrawInterval, 0.016f);

    if (bAddCurrentPoint &&
        (TrailPoints.IsEmpty() || ReplayTime - TrailPoints.Last().Time >= EffectiveSampleInterval))
    {
        const float VisualScale = FMath::Max(GetActorScale3D().GetMax(), 1.0f);
        const FVector TailDirection = GetActorForwardVector().GetSafeNormal(
            SMALL_NUMBER,
            FVector::ForwardVector
        );
        const FVector TailLocation = CurrentLocation - TailDirection * TrailTailOffset * VisualScale;
        TrailPoints.Add({ReplayTime, TailLocation});
    }

    int32 RemoveCount = 0;
    const float OldestAllowedTime = ReplayTime - EffectiveDuration;
    while (RemoveCount < TrailPoints.Num() && TrailPoints[RemoveCount].Time < OldestAllowedTime)
    {
        ++RemoveCount;
    }
    if (RemoveCount > 0)
    {
        TrailPoints.RemoveAt(0, RemoveCount, EAllowShrinking::No);
    }

    TrailDrawAccumulator += DeltaTime;
    if (TrailDrawAccumulator < EffectiveDrawInterval || TrailPoints.Num() < 2)
    {
        return;
    }
    TrailDrawAccumulator = FMath::Fmod(TrailDrawAccumulator, EffectiveDrawInterval);

    const float DebugLifeTime = FMath::Max(EffectiveDrawInterval * 2.5f, 0.1f);
    const float DrawThickness = TrailThickness * (bIsSelected ? 1.6f : 1.0f);

    for (int32 PointIndex = 1; PointIndex < TrailPoints.Num(); ++PointIndex)
    {
        const float Age = ReplayTime - TrailPoints[PointIndex].Time;
        const float Freshness = 1.0f - FMath::Clamp(Age / EffectiveDuration, 0.0f, 1.0f);
        FColor SegmentColor = BaseTrailColor;
        SegmentColor.A = static_cast<uint8>(FMath::Lerp(176.0f, 255.0f, Freshness));

        DrawDebugLine(
            GetWorld(),
            TrailPoints[PointIndex - 1].Location,
            TrailPoints[PointIndex].Location,
            FColor(8, 12, 18, SegmentColor.A),
            false,
            DebugLifeTime,
            1,
            DrawThickness + 1.0f
        );

        DrawDebugLine(
            GetWorld(),
            TrailPoints[PointIndex - 1].Location,
            TrailPoints[PointIndex].Location,
            SegmentColor,
            false,
            DebugLifeTime,
            1,
            DrawThickness
        );
    }
}

void AMyReplayEntity::UpdateReplay(float InReplayTime, float DeltaTime)
{
    if (!bIsInitialized || !FlightDataTable || !GeoRef)
    {
        return;
    }

    const float ReplayTime = FMath::Clamp(InReplayTime, 0.0f, ReplayDuration);
    if (LastReplayTime >= 0.0f && ReplayTime + KINDA_SMALL_NUMBER < LastReplayTime)
    {
        ResetReplayState();
    }

    const float ExactFrame = ReplayTime / FMath::Max(DataSampleInterval, 0.001f);
    const int32 LastAvailableIndex = FMath::Max(TrackRowCount - 1, 0);
    const int32 Index0 = FMath::Clamp(FMath::FloorToInt(ExactFrame), 0, LastAvailableIndex);
    const int32 Index1 = FMath::Min(Index0 + 1, LastAvailableIndex);
    const float Alpha = Index1 > Index0
        ? FMath::Clamp(ExactFrame - static_cast<float>(Index0), 0.0f, 1.0f)
        : 0.0f;

    const FName RowName0(*FString::FromInt(Index0));
    const FName RowName1(*FString::FromInt(Index1));
    static const FString ContextString(TEXT("ReplayLookup"));

    FFlightData* TrackData0 = FlightDataTable->FindRow<FFlightData>(RowName0, ContextString);
    FFlightData* TrackData1 = FlightDataTable->FindRow<FFlightData>(RowName1, ContextString);

    if (!TrackData0)
    {
        bCurrentlyActive = false;
        SetActorHiddenInGame(true);
        SetActorEnableCollision(false);
        EntityLabel->SetVisibility(false, true);
        UpdateTrail(ReplayTime, DeltaTime, false, GetActorLocation());
        LastReplayTime = ReplayTime;
        return;
    }

    bCurrentlyActive = TrackData0->Active != 0;
    SetActorHiddenInGame(!bCurrentlyActive);
    SetActorEnableCollision(bCurrentlyActive);

    if (ExplosionDataTable)
    {
        FExplosionData* ExplosionData = ExplosionDataTable->FindRow<FExplosionData>(RowName0, ContextString);
        if (ExplosionData && ExplosionData->Explosion == 1 && LastExplodedFrameIndex != Index0)
        {
            const FVector ExplosionLLH(ExplosionData->Lon, ExplosionData->Lat, ExplosionData->Alt);
            const FVector ExplosionLocation = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(ExplosionLLH);

            if (ExplosionActorClass)
            {
                FActorSpawnParameters SpawnParameters;
                SpawnParameters.Owner = this;
                SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                AActor* ExplosionActor = GetWorld()->SpawnActor<AActor>(
                    ExplosionActorClass,
                    ExplosionLocation,
                    FRotator::ZeroRotator,
                    SpawnParameters
                );
                if (ExplosionActor)
                {
                    if (ExplosionLifeSpan > 0.0f)
                    {
                        ExplosionActor->SetLifeSpan(ExplosionLifeSpan);
                    }
                    SpawnedExplosionActors.Add(ExplosionActor);
                }
            }

            bHasExploded = true;
            LastExplodedFrameIndex = Index0;
        }
    }

    if (!bCurrentlyActive)
    {
        CurrentSpeedMetersPerSecond = 0.0;
        LastMotionSampleTime = -1.0f;
        EntityLabel->SetVisibility(false, true);
        UpdateTrail(ReplayTime, DeltaTime, false, GetActorLocation());
        LastReplayTime = ReplayTime;
        return;
    }

    double TargetLon = TrackData0->Lon;
    double TargetLat = TrackData0->Lat;
    double TargetAlt = TrackData0->Alt;
    FRotator FinalRotation(TrackData0->Pitch, TrackData0->Yaw, TrackData0->Roll);

    if (TrackData1)
    {
        TargetLon = FMath::Lerp(TrackData0->Lon, TrackData1->Lon, Alpha);
        TargetLat = FMath::Lerp(TrackData0->Lat, TrackData1->Lat, Alpha);
        TargetAlt = FMath::Lerp(TrackData0->Alt, TrackData1->Alt, Alpha);

        const FQuat Rotation0 = FRotator(
            TrackData0->Pitch,
            TrackData0->Yaw,
            TrackData0->Roll
        ).Quaternion();
        const FQuat Rotation1 = FRotator(
            TrackData1->Pitch,
            TrackData1->Yaw,
            TrackData1->Roll
        ).Quaternion();
        FinalRotation = FQuat::Slerp(Rotation0, Rotation1, Alpha).GetNormalized().Rotator();
    }

    const FVector FinalLocation = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(TargetLon, TargetLat, TargetAlt)
    );
    SetActorLocation(FinalLocation);
    SetActorRotation(FinalRotation);

    if (LastMotionSampleTime >= 0.0f && ReplayTime > LastMotionSampleTime + KINDA_SMALL_NUMBER)
    {
        const float MotionDeltaTime = ReplayTime - LastMotionSampleTime;
        CurrentSpeedMetersPerSecond = FVector::Dist(FinalLocation, LastMotionLocation) / 100.0 / MotionDeltaTime;
    }
    else
    {
        CurrentSpeedMetersPerSecond = 0.0;
    }
    LastMotionLocation = FinalLocation;
    LastMotionSampleTime = ReplayTime;
    CurrentAltitudeMeters = TargetAlt;
    CurrentReplayRotation = FinalRotation;

    FVector CameraLocation = FVector::ZeroVector;
    FRotator CameraRotation = FRotator::ZeroRotator;
    bool bHasCameraLocation = false;

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController)
    {
        if (PlayerController->PlayerCameraManager)
        {
            CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
            CameraRotation = PlayerController->PlayerCameraManager->GetCameraRotation();
            bHasCameraLocation = true;
        }
        else if (const APawn* Pawn = PlayerController->GetPawn())
        {
            CameraLocation = Pawn->GetActorLocation();
            CameraRotation = Pawn->GetActorRotation();
            bHasCameraLocation = true;
        }
    }

    const float CameraDistance = bHasCameraLocation
        ? FVector::Dist(FinalLocation, CameraLocation)
        : 0.0f;
    const float CurrentVisualScale = FMath::Max(
        GetActorScale3D().GetAbsMax(),
        KINDA_SMALL_NUMBER
    );
    float DesiredVisualScale = CurrentVisualScale;

    if (bEnableDynamicScale && bHasCameraLocation)
    {
        bool bCalculatedScreenSpaceScale = false;
        if (bUseScreenSpaceScale && PlayerController && PlayerController->PlayerCameraManager)
        {
            int32 ViewportWidth = 0;
            int32 ViewportHeight = 0;
            PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);

            const float UnscaledBoundsRadius = GetUnscaledVisualBoundsRadius();
            if (ViewportWidth > 0 &&
                ViewportHeight > 0 &&
                UnscaledBoundsRadius > KINDA_SMALL_NUMBER)
            {
                const float AspectRatio =
                    static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight);
                const float HorizontalFovRadians = FMath::DegreesToRadians(
                    FMath::Clamp(PlayerController->PlayerCameraManager->GetFOVAngle(), 20.0f, 120.0f)
                );
                const float TanHalfVerticalFov =
                    FMath::Tan(HorizontalFovRadians * 0.5f) /
                    FMath::Max(AspectRatio, 0.1f);
                const float DesiredWorldRadius =
                    FMath::Max(TargetScreenSizePixels, 1.0f) *
                    CameraDistance *
                    TanHalfVerticalFov /
                    static_cast<float>(ViewportHeight);

                DesiredVisualScale = DesiredWorldRadius / UnscaledBoundsRadius;
                bCalculatedScreenSpaceScale = true;
            }
        }

        if (!bCalculatedScreenSpaceScale)
        {
            DesiredVisualScale = FMath::GetMappedRangeValueClamped(
                FVector2D(0.0f, FMath::Max(ScaleDistanceThreshold, 1.0f)),
                FVector2D(MinScale, MaxScale),
                CameraDistance
            );
        }
    }
    if (bIsSelected)
    {
        DesiredVisualScale *= SelectedScaleMultiplier;
    }

    DesiredVisualScale = FMath::Clamp(
        DesiredVisualScale,
        FMath::Max(MinScale, 0.01f),
        FMath::Max(MaxScale, MinScale)
    );

    float AppliedVisualScale = DesiredVisualScale;
    if (bVisualScaleInitialized && DeltaTime > 0.0f)
    {
        const float ScaleBlendAlpha =
            1.0f - FMath::Exp(-FMath::Max(ScreenSpaceScaleInterpSpeed, 0.1f) * DeltaTime);
        AppliedVisualScale = FMath::Lerp(
            CurrentVisualScale,
            DesiredVisualScale,
            ScaleBlendAlpha
        );
    }
    SetActorScale3D(FVector(AppliedVisualScale));
    bVisualScaleInitialized = true;

    EntityLabel->SetVisibility(bShowEntityLabel && bCurrentlyActive, true);
    if (bShowEntityLabel && bCurrentlyActive)
    {
        const int32 AltitudeMeters = FMath::RoundToInt(TargetAlt);
        if (AltitudeMeters != LastDisplayedAltitudeMeters)
        {
            EntityLabel->SetText(FText::FromString(FString::Printf(
                TEXT("%s | %d m"),
                *EntityId,
                AltitudeMeters
            )));
            LastDisplayedAltitudeMeters = AltitudeMeters;
        }
        EntityLabel->SetWorldSize(LabelWorldSize);

        RefreshLabelTransform(FinalLocation, CameraLocation, CameraRotation, bHasCameraLocation);
    }

    UpdateTrail(ReplayTime, DeltaTime, true, FinalLocation);

    ProjectionUpdateAccumulator += DeltaTime;
    const float EffectiveProjectionInterval = FMath::Max(ProjectionUpdateInterval, 0.016f);
    if (bEnableProjectionLine && ProjectionUpdateAccumulator >= EffectiveProjectionInterval)
    {
        ProjectionUpdateAccumulator = FMath::Fmod(ProjectionUpdateAccumulator, EffectiveProjectionInterval);

        const FVector ProjectionEnd = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(
            FVector(TargetLon, TargetLat, ProjectionTraceEndHeight)
        );
        FHitResult HitResult;
        FCollisionQueryParams TraceParameters(SCENE_QUERY_STAT(ReplayGroundProjection), false, this);
        const bool bHitGround = GetWorld()->LineTraceSingleByChannel(
            HitResult,
            FinalLocation,
            ProjectionEnd,
            ECC_Visibility,
            TraceParameters
        );

        const FVector GroundLocation = bHitGround ? HitResult.ImpactPoint : ProjectionEnd;
        const FVector LocalUp = (FinalLocation - ProjectionEnd).GetSafeNormal(
            SMALL_NUMBER,
            FVector::UpVector
        );
        const float DebugLifeTime = EffectiveProjectionInterval * 1.25f;
        const float PillarRadius = 20.0f * AppliedVisualScale;
        const FColor ProjectionColor = bIsSelected ? FColor::Yellow : BaseTrailColor;

        DrawDebugCylinder(
            GetWorld(),
            FinalLocation,
            GroundLocation,
            PillarRadius,
            12,
            ProjectionColor,
            false,
            DebugLifeTime,
            0,
            ProjectionLineThickness
        );

        if (bHitGround)
        {
            DrawDebugCylinder(
                GetWorld(),
                GroundLocation,
                GroundLocation + LocalUp * 10.0f,
                ProjectionCircleRadius * AppliedVisualScale,
                32,
                ProjectionColor,
                false,
                DebugLifeTime,
                0,
                ProjectionLineThickness
            );
        }
    }

    LastReplayTime = ReplayTime;
}
