#include "MyGameManager.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Cesium3DTileset.h"
#include "CesiumRasterOverlay.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/Canvas.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ReplayHUDWidget.h"
#include "ReplayCaptureHUD.h"
#include "UnrealClient.h"

AMyGameManager::AMyGameManager()
{
    PrimaryActorTick.bCanEverTick = true;
    SetIsSpatiallyLoaded(false);
}

void AMyGameManager::BeginPlay()
{
    Super::BeginPlay();

    PlaybackRate = FMath::Clamp(InitialPlaybackRate, 0.25f, 4.0f);
    InitializeCaptureFromCommandLine();

    FString ReplayCsvError;
    if (!InitializeReplayCsvFromCommandLine(ReplayCsvError))
    {
        UE_LOG(LogTemp, Error, TEXT("GameManager: %s"), *ReplayCsvError);
        if (bCaptureRound)
        {
            bCaptureFailed = true;
            bCaptureExitPending = true;
            CaptureFailureReason = ReplayCsvError;
        }
        return;
    }

    // The latest scenario can begin with opposing formations more than 33 km
    // apart. Enforce the presentation settings at runtime so older Blueprint
    // defaults cannot retain the previous 80 km camera ceiling.
    OverviewFrameMargin = 1.18f;
    MaximumCameraDistanceMeters = 130000.0f;
    bAlwaysKeepAircraftInFrame = true;
    OverviewCameraInterpSpeed = 2.2f;

    if (!ManifestTable || !CesiumRef)
    {
        UE_LOG(LogTemp, Error, TEXT("GameManager: ManifestTable or CesiumRef is missing."));
        return;
    }

    if (bCaptureRound)
    {
        ConfigureHighQualityCapture();
    }

    const TArray<FName> RowNames = ManifestTable->GetRowNames();
    for (const FName& RowName : RowNames)
    {
        const FManifestData* Item = ManifestTable->FindRow<FManifestData>(RowName, TEXT("ReplayManifest"));
        if (!Item)
        {
            continue;
        }

        FString TrackSource;
        UDataTable* TrackTable = nullptr;
        if (!ReplayCsvDirectory.IsEmpty())
        {
            TrackSource = FPaths::Combine(ReplayCsvDirectory, FPaths::GetCleanFilename(Item->TrackFile));
            TrackTable = LoadRuntimeCsvTable(Item->TrackFile, FFlightData::StaticStruct(), TEXT("track"));
        }
        else
        {
            const FString TrackPureName = Item->TrackFile.Replace(TEXT(".csv"), TEXT(""));
            TrackSource = FString::Printf(TEXT("/Game/Data/%s.%s"), *TrackPureName, *TrackPureName);
            TrackTable = LoadObject<UDataTable>(nullptr, *TrackSource);
        }

        UDataTable* ExplosionTable = nullptr;
        if (!Item->ExplosionFile.IsEmpty())
        {
            if (!ReplayCsvDirectory.IsEmpty())
            {
                ExplosionTable = LoadRuntimeCsvTable(
                    Item->ExplosionFile,
                    FExplosionData::StaticStruct(),
                    TEXT("explosion")
                );
            }
            else
            {
                const FString ExplosionPureName = Item->ExplosionFile.Replace(TEXT(".csv"), TEXT(""));
                const FString ExplosionAssetPath = FString::Printf(
                    TEXT("/Game/Data/%s.%s"),
                    *ExplosionPureName,
                    *ExplosionPureName
                );
                ExplosionTable = LoadObject<UDataTable>(nullptr, *ExplosionAssetPath);
            }
        }

        if (!TrackTable)
        {
            UE_LOG(LogTemp, Warning, TEXT("Could not load track table: %s"), *TrackSource);
            continue;
        }

        TSubclassOf<AMyReplayEntity> ClassToSpawn;
        if (const TSubclassOf<AMyReplayEntity>* MappedClass = EntityClassMap.Find(Item->Type))
        {
            ClassToSpawn = *MappedClass;
        }
        if (!ClassToSpawn)
        {
            if (const TSubclassOf<AMyReplayEntity>* MappedClass = EntityClassMap.Find(Item->Category))
            {
                ClassToSpawn = *MappedClass;
            }
        }

        // Tacview normally reports semantic types such as Air+FixedWing and
        // Weapon+Missile, while older imported rounds used model-specific map
        // keys. Keep those mappings, but provide project-native category
        // fallbacks so arbitrary ACMI files do not require Blueprint editing.
        if (!ClassToSpawn && Item->Category.Equals(TEXT("Plane"), ESearchCase::IgnoreCase))
        {
            ClassToSpawn = LoadClass<AMyReplayEntity>(
                nullptr,
                TEXT("/Game/BP_F16.BP_F16_C")
            );
        }
        if (!ClassToSpawn && Item->Category.Equals(TEXT("Missile"), ESearchCase::IgnoreCase))
        {
            ClassToSpawn = LoadClass<AMyReplayEntity>(
                nullptr,
                TEXT("/Game/BP_Missile.BP_Missile_C")
            );
        }
        if (!ClassToSpawn)
        {
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("No class mapped for Type: %s or Category: %s"),
                *Item->Type,
                *Item->Category
            );
            continue;
        }

        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMyReplayEntity* NewEntity = GetWorld()->SpawnActor<AMyReplayEntity>(
            ClassToSpawn,
            FVector::ZeroVector,
            FRotator::ZeroRotator,
            SpawnParameters
        );

        if (!NewEntity)
        {
            continue;
        }

#if WITH_EDITOR
        NewEntity->SetActorLabel(RowName.ToString());
#endif
        NewEntity->InitializeEntity(
            TrackTable,
            ExplosionTable,
            CesiumRef,
            RowName.ToString(),
            Item->Type,
            Item->Category,
            Item->Team
        );
        // Trails are intentionally disabled for the clean presentation view.
        // Set this after spawning so older Blueprint defaults cannot re-enable them.
        NewEntity->bEnableTrail = false;
        const bool bMissileLike =
            Item->Category.Equals(TEXT("Missile"), ESearchCase::IgnoreCase) ||
            Item->Category.Equals(TEXT("Munition"), ESearchCase::IgnoreCase) ||
            Item->Category.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase);
        NewEntity->bEnableDynamicScale = true;
        NewEntity->bUseScreenSpaceScale = true;
        NewEntity->TargetScreenSizePixels = bMissileLike ? 32.0f : 56.0f;
        NewEntity->ScreenSpaceScaleInterpSpeed = 6.0f;
        NewEntity->MinScale = 1.0f;
        NewEntity->MaxScale = bMissileLike ? 1200.0f : 600.0f;
        // Recorded labels are drawn in screen space at a fixed pixel size.
        NewEntity->bShowEntityLabel = !bCaptureRound;
        SpawnedEntities.Add(NewEntity);
        ReplayDuration = FMath::Max(ReplayDuration, NewEntity->GetReplayDuration());
    }

    if (bCaptureRound && CaptureReplayLimit > 0.0f)
    {
        ReplayDuration = FMath::Min(ReplayDuration, CaptureReplayLimit);
    }

    if (SpawnedEntities.IsEmpty() || ReplayDuration <= 0.0f)
    {
        const FString Error = FString::Printf(
            TEXT("Replay data produced no playable entities (manifest rows: %d, spawned: %d, duration: %.3f)."),
            ManifestTable->GetRowMap().Num(),
            SpawnedEntities.Num(),
            ReplayDuration
        );
        UE_LOG(LogTemp, Error, TEXT("GameManager: %s"), *Error);
        if (bCaptureRound)
        {
            bCaptureFailed = true;
            bCaptureExitPending = true;
            CaptureFailureReason = Error;
        }
        return;
    }

    UpdateAllEntities(0.0f);
    InitializeOverviewCamera();
    UpdateOverviewCamera(0.0f, true);
    UpdateEntityLabelLayout();

    if (bCaptureRound)
    {
        if (APlayerController* PlayerController = GetWorld()->GetFirstPlayerController())
        {
            PlayerController->ClientSetHUD(AReplayCaptureHUD::StaticClass());
        }
    }
    else if (bCreateReplayHUD)
    {
        if (APlayerController* PlayerController = GetWorld()->GetFirstPlayerController())
        {
            ReplayHUD = CreateWidget<UReplayHUDWidget>(PlayerController, UReplayHUDWidget::StaticClass());
            if (ReplayHUD)
            {
                ReplayHUD->InitializeWithManager(this);
                ReplayHUD->AddToViewport(100);
            }

            PlayerController->bShowMouseCursor = true;
            FInputModeGameAndUI InputMode;
            InputMode.SetHideCursorDuringCapture(false);
            InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
            PlayerController->SetInputMode(InputMode);
        }
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Replay initialized: %d entities, duration %.2f seconds."),
        SpawnedEntities.Num(),
        ReplayDuration
    );
}

void AMyGameManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FApp::SetUseFixedTimeStep(false);
    CloseDirectVideoEncoder();
    Super::EndPlay(EndPlayReason);
}

void AMyGameManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (bCaptureExitPending)
    {
        if (bDirectVideoEncode && !FinishDirectVideoEncoder())
        {
            return;
        }

        const FString MarkerName = bCaptureFailed
            ? TEXT("CaptureFailed.txt")
            : TEXT("CaptureComplete.txt");
        const FString MarkerContents = bCaptureFailed
            ? CaptureFailureReason
            : FString::Printf(TEXT("%d\n"), CaptureFrameNumber);
        FFileHelper::SaveStringToFile(
            MarkerContents,
            *FPaths::Combine(CaptureOutputDirectory, MarkerName)
        );
        FApp::SetUseFixedTimeStep(false);
        FPlatformMisc::RequestExit(false);
        return;
    }

    if (bCaptureRound && !bCaptureStarted)
    {
        // Visit representative camera positions before recording. This lets
        // Cesium cache the high-resolution terrain and raster tiles along the
        // whole engagement instead of revealing rectangular LOD transitions
        // while the final video is already running. The last stage returns to
        // replay time zero and waits for that opening view to settle again.
        constexpr float PrewarmFractions[] = {
            0.0f,
            0.20f,
            0.40f,
            0.60f,
            0.80f,
            0.95f,
            0.0f
        };
        constexpr int32 PrewarmStageCount = UE_ARRAY_COUNT(PrewarmFractions);
        CapturePrewarmStage = FMath::Clamp(CapturePrewarmStage, 0, PrewarmStageCount - 1);
        const float PrewarmReplayTime =
            ReplayDuration * PrewarmFractions[CapturePrewarmStage];

        if (!FMath::IsNearlyEqual(ReplayTime, PrewarmReplayTime, 0.001f))
        {
            ReplayTime = PrewarmReplayTime;
            UpdateAllEntities(0.0f);
            UpdateOverviewCamera(0.0f, true);
            UpdateEntityLabelLayout();
            CaptureTilesReadyElapsed = 0.0f;
        }

        UpdateAllEntities(DeltaSeconds);
        UpdateOverviewCamera(DeltaSeconds, false);
        UpdateEntityLabelLayout();
        CaptureWarmupElapsed += DeltaSeconds;
        CaptureWarmupLogElapsed += DeltaSeconds;

        const bool bTilesReady = AreCesiumTilesReady(LastCesiumLoadProgress);
        CaptureTilesReadyElapsed = bTilesReady
            ? CaptureTilesReadyElapsed + DeltaSeconds
            : 0.0f;

        if (CaptureWarmupLogElapsed >= 2.0f)
        {
            UE_LOG(
                LogTemp,
                Display,
                TEXT("Prewarming Cesium view %d/%d: %.0f%%, elapsed %.1f / %.1f seconds."),
                CapturePrewarmStage + 1,
                PrewarmStageCount,
                LastCesiumLoadProgress,
                CaptureWarmupElapsed,
                CaptureMaxWarmupDuration
            );
            CaptureWarmupLogElapsed = 0.0f;
        }

        const bool bMinimumWarmupComplete = CaptureWarmupElapsed >= CaptureWarmupDuration;
        const bool bTilesStable = CaptureTilesReadyElapsed >= 2.0f;
        if (bTilesStable && CapturePrewarmStage < PrewarmStageCount - 1)
        {
            ++CapturePrewarmStage;
            CaptureTilesReadyElapsed = 0.0f;
            UE_LOG(
                LogTemp,
                Display,
                TEXT("Cesium prewarm view cached; advancing to view %d/%d."),
                CapturePrewarmStage + 1,
                PrewarmStageCount
            );
        }
        else if (bMinimumWarmupComplete && bTilesStable)
        {
            FApp::SetFixedDeltaTime(1.0 / static_cast<double>(CaptureFrameRate));
            FApp::SetUseFixedTimeStep(true);
            bCaptureStarted = true;
            UE_LOG(
                LogTemp,
                Display,
                TEXT("Cesium route prewarm complete. Starting deterministic frame capture.")
            );
        }
        else if (CaptureWarmupElapsed >= CaptureMaxWarmupDuration)
        {
            bCaptureFailed = true;
            CaptureFailureReason = FString::Printf(
                TEXT("Cesium map did not become ready within %.0f seconds (load progress %.0f%%). Check network/proxy access to api.cesium.com."),
                CaptureMaxWarmupDuration,
                LastCesiumLoadProgress
            );
            UE_LOG(LogTemp, Error, TEXT("%s"), *CaptureFailureReason);
            bCaptureExitPending = true;
        }
        return;
    }

    const float ReplayDeltaSeconds = bCaptureRound
        ? 1.0f / static_cast<float>(CaptureFrameRate)
        : DeltaSeconds;

    if (!bReplayPaused && !bReplayFinished && ReplayDuration > 0.0f)
    {
        // Capture frame zero before advancing, so the video starts at CSV time 0.
        if (!bCaptureRound || CaptureFrameNumber > 0)
        {
            ReplayTime += ReplayDeltaSeconds * PlaybackRate;
        }

        if (ReplayTime >= ReplayDuration)
        {
            ReplayTime = ReplayDuration;
            bReplayFinished = true;
            FinishedElapsedTime = 0.0f;
            SetSelectedEntity(nullptr);
        }
    }

    UpdateAllEntities(ReplayDeltaSeconds);
    UpdateOverviewCamera(ReplayDeltaSeconds, false);
    UpdateEntityLabelLayout();

    if (bReplayFinished)
    {
        FinishedElapsedTime += ReplayDeltaSeconds;
    }

    if (bCaptureRound)
    {
        RequestCaptureFrame();
        if (bReplayFinished && FinishedElapsedTime >= FMath::Max(CaptureBlackScreenDuration, 0.0f))
        {
            // Exit on the following tick so the last requested black frame is rendered and written.
            bCaptureExitPending = true;
        }
    }
}

float AMyGameManager::GetReplayProgress() const
{
    return ReplayDuration > KINDA_SMALL_NUMBER
        ? FMath::Clamp(ReplayTime / ReplayDuration, 0.0f, 1.0f)
        : 0.0f;
}

void AMyGameManager::GetAliveCounts(int32& OutRedCount, int32& OutBlueCount) const
{
    OutRedCount = 0;
    OutBlueCount = 0;

    for (const AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (!IsValid(Entity) || !Entity->IsCurrentlyActive())
        {
            continue;
        }
        if (Entity->GetEntityCategory().Equals(TEXT("Missile"), ESearchCase::IgnoreCase) ||
            Entity->GetEntityCategory().Equals(TEXT("Munition"), ESearchCase::IgnoreCase) ||
            Entity->GetEntityCategory().Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (Entity->GetTeam().Equals(TEXT("Red"), ESearchCase::IgnoreCase))
        {
            ++OutRedCount;
        }
        else if (Entity->GetTeam().Equals(TEXT("Blue"), ESearchCase::IgnoreCase))
        {
            ++OutBlueCount;
        }
    }
}

FString AMyGameManager::GetSelectedEntityDetails() const
{
    if (!IsValid(SelectedEntity))
    {
        return TEXT("CLICK AN ENTITY FOR DETAILS");
    }

    const FRotator Rotation = SelectedEntity->GetCurrentReplayRotation();
    const float Heading = FMath::Fmod(Rotation.Yaw + 360.0f, 360.0f);
    const float Pitch = FRotator::NormalizeAxis(Rotation.Pitch);
    const TCHAR* Status = SelectedEntity->IsCurrentlyActive()
        ? TEXT("ACTIVE")
        : (SelectedEntity->HasExploded() ? TEXT("DESTROYED") : TEXT("INACTIVE"));

    return FString::Printf(
        TEXT("%s  %s\nTEAM  %s    STATUS  %s\nALT  %.0f m    SPEED  %.0f m/s\nHEADING  %03.0f deg    PITCH  %+.1f deg"),
        *SelectedEntity->GetEntityId(),
        *SelectedEntity->GetEntityType(),
        *SelectedEntity->GetTeam().ToUpper(),
        Status,
        SelectedEntity->GetCurrentAltitudeMeters(),
        SelectedEntity->GetCurrentSpeedMetersPerSecond(),
        Heading,
        Pitch
    );
}

bool AMyGameManager::GetMapOverlayData(
    float& OutNorthAngleDegrees,
    float& OutScaleMeters,
    float& OutScalePixels
) const
{
    OutNorthAngleDegrees = 0.0f;
    OutScaleMeters = 10000.0f;
    OutScalePixels = 120.0f;

    if (!CesiumRef || !GetWorld())
    {
        return false;
    }

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (!PlayerController || !PlayerController->PlayerCameraManager)
    {
        return false;
    }

    const FVector CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
    const FVector CameraLLH = CesiumRef->TransformUnrealPositionToLongitudeLatitudeHeight(CameraLocation);
    const double LatitudeRadians = FMath::DegreesToRadians(CameraLLH.Y);
    constexpr double CoordinateStepDegrees = 0.01;

    const FVector OriginWorld = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(CameraLLH.X, CameraLLH.Y, 0.0)
    );
    const FVector NorthWorld = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(CameraLLH.X, CameraLLH.Y + CoordinateStepDegrees, 0.0)
    );
    const FVector EastWorld = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(CameraLLH.X + CoordinateStepDegrees, CameraLLH.Y, 0.0)
    );

    FVector2D OriginScreen;
    FVector2D NorthScreen;
    FVector2D EastScreen;
    if (!UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PlayerController, OriginWorld, OriginScreen, true) ||
        !UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PlayerController, NorthWorld, NorthScreen, true) ||
        !UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PlayerController, EastWorld, EastScreen, true))
    {
        return false;
    }

    const FVector2D NorthDirection = (NorthScreen - OriginScreen).GetSafeNormal();
    OutNorthAngleDegrees = FMath::RadiansToDegrees(FMath::Atan2(NorthDirection.X, -NorthDirection.Y));

    const double EastMeters = 111320.0 * FMath::Cos(LatitudeRadians) * CoordinateStepDegrees;
    const double EastPixels = FVector2D::Distance(OriginScreen, EastScreen);
    if (EastMeters <= KINDA_SMALL_NUMBER || EastPixels <= KINDA_SMALL_NUMBER)
    {
        return true;
    }

    const double MetersPerPixel = EastMeters / EastPixels;
    const double TargetMeters = MetersPerPixel * 120.0;
    const double Magnitude = FMath::Pow(10.0, FMath::FloorToDouble(FMath::LogX(10.0, TargetMeters)));
    const double Normalized = TargetMeters / Magnitude;
    const double NiceMultiplier = Normalized >= 5.0 ? 5.0 : (Normalized >= 2.0 ? 2.0 : 1.0);

    OutScaleMeters = static_cast<float>(NiceMultiplier * Magnitude);
    OutScalePixels = FMath::Clamp(static_cast<float>(OutScaleMeters / MetersPerPixel), 48.0f, 160.0f);
    return true;
}

bool AMyGameManager::SelectEntityAtWidgetPosition(const FVector2D& WidgetPosition)
{
    if (bReplayFinished || !GetWorld())
    {
        return false;
    }

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (!PlayerController)
    {
        return false;
    }

    AMyReplayEntity* ClosestEntity = nullptr;
    float ClosestDistanceSquared = FMath::Square(SelectionRadiusPixels);

    for (AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (!IsValid(Entity) || !Entity->IsCurrentlyActive())
        {
            continue;
        }

        FVector2D EntityWidgetPosition;
        if (!UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
                PlayerController,
                Entity->GetActorLocation(),
                EntityWidgetPosition,
                true))
        {
            continue;
        }

        const float DistanceSquared = FVector2D::DistSquared(WidgetPosition, EntityWidgetPosition);
        if (DistanceSquared <= ClosestDistanceSquared)
        {
            ClosestDistanceSquared = DistanceSquared;
            ClosestEntity = Entity;
        }
    }

    SetSelectedEntity(ClosestEntity);
    return ClosestEntity != nullptr;
}

void AMyGameManager::TogglePause()
{
    if (bReplayFinished)
    {
        RestartReplay();
        return;
    }
    bReplayPaused = !bReplayPaused;
}

void AMyGameManager::AdjustPlaybackRate(int32 Direction)
{
    if (Direction == 0)
    {
        return;
    }
    const float Multiplier = Direction > 0 ? 2.0f : 0.5f;
    PlaybackRate = FMath::Clamp(PlaybackRate * Multiplier, 0.25f, 4.0f);
}

void AMyGameManager::SeekNormalized(float NormalizedTime)
{
    ReplayTime = FMath::Clamp(NormalizedTime, 0.0f, 1.0f) * ReplayDuration;
    bReplayFinished = false;
    FinishedElapsedTime = 0.0f;
    UpdateAllEntities(0.0f);
    UpdateOverviewCamera(0.0f, true);
    UpdateEntityLabelLayout();
}

void AMyGameManager::RestartReplay()
{
    ReplayTime = 0.0f;
    FinishedElapsedTime = 0.0f;
    bReplayFinished = false;
    bReplayPaused = false;
    SetSelectedEntity(nullptr);

    for (AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (IsValid(Entity))
        {
            Entity->ResetReplayState();
        }
    }
    UpdateAllEntities(0.0f);
    UpdateOverviewCamera(0.0f, true);
    UpdateEntityLabelLayout();
}

void AMyGameManager::UpdateAllEntities(float DeltaSeconds)
{
    for (AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (IsValid(Entity))
        {
            Entity->UpdateReplay(ReplayTime, DeltaSeconds);
        }
    }
}

void AMyGameManager::SetSelectedEntity(AMyReplayEntity* NewSelection)
{
    if (SelectedEntity == NewSelection)
    {
        return;
    }
    if (IsValid(SelectedEntity))
    {
        SelectedEntity->SetSelected(false);
    }
    SelectedEntity = NewSelection;
    if (IsValid(SelectedEntity))
    {
        SelectedEntity->SetSelected(true);
    }
}

void AMyGameManager::InitializeCaptureFromCommandLine()
{
    bCaptureRound = FParse::Param(FCommandLine::Get(), TEXT("AutoRecordRound"));
    if (!bCaptureRound)
    {
        return;
    }

    FParse::Value(FCommandLine::Get(), TEXT("RecordFPS="), CaptureFrameRate);
    CaptureFrameRate = FMath::Clamp(CaptureFrameRate, 1, 120);
    FParse::Value(FCommandLine::Get(), TEXT("ReplayRate="), PlaybackRate);
    PlaybackRate = FMath::Clamp(PlaybackRate, 0.25f, 4.0f);
    FParse::Value(FCommandLine::Get(), TEXT("RecordReplayLimit="), CaptureReplayLimit);
    CaptureReplayLimit = FMath::Max(CaptureReplayLimit, 0.0f);
    FParse::Value(FCommandLine::Get(), TEXT("RecordWarmup="), CaptureWarmupDuration);
    CaptureWarmupDuration = FMath::Max(CaptureWarmupDuration, 0.0f);
    FParse::Value(FCommandLine::Get(), TEXT("RecordMaxWarmup="), CaptureMaxWarmupDuration);
    CaptureMaxWarmupDuration = FMath::Max(CaptureMaxWarmupDuration, CaptureWarmupDuration + 5.0f);
    FParse::Value(FCommandLine::Get(), TEXT("RecordOutputDir="), CaptureOutputDirectory);
    CaptureOutputDirectory.TrimQuotesInline();
    FParse::Value(FCommandLine::Get(), TEXT("RecordFfmpeg="), CaptureFfmpegExecutable);
    CaptureFfmpegExecutable.TrimQuotesInline();
    FParse::Value(FCommandLine::Get(), TEXT("RecordVideoFile="), CaptureVideoFilename);
    CaptureVideoFilename.TrimQuotesInline();

    if (CaptureOutputDirectory.IsEmpty())
    {
        CaptureOutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RoundRenders/Frames"));
    }
    CaptureOutputDirectory = FPaths::ConvertRelativePathToFull(CaptureOutputDirectory);
    IFileManager::Get().MakeDirectory(*CaptureOutputDirectory, true);

    bDirectVideoEncode = !CaptureFfmpegExecutable.IsEmpty() && !CaptureVideoFilename.IsEmpty();
    if (bDirectVideoEncode)
    {
        CaptureFfmpegExecutable = FPaths::ConvertRelativePathToFull(CaptureFfmpegExecutable);
        CaptureVideoFilename = FPaths::ConvertRelativePathToFull(CaptureVideoFilename);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(CaptureVideoFilename), true);

        if (!FPaths::FileExists(CaptureFfmpegExecutable))
        {
            bCaptureFailed = true;
            bCaptureExitPending = true;
            CaptureFailureReason = FString::Printf(
                TEXT("ffmpeg executable not found: %s"),
                *CaptureFfmpegExecutable
            );
        }
        else
        {
            if (IConsoleVariable* ScreenshotDelegateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenshotDelegate")))
            {
                ScreenshotDelegateCVar->Set(1, ECVF_SetByCode);
            }
            ScreenshotCapturedHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(
                this,
                &AMyGameManager::OnScreenshotCaptured
            );
        }
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Automatic round capture enabled: %d FPS, minimum %.1f second / maximum %.1f second map warmup, output %s%s"),
        CaptureFrameRate,
        CaptureWarmupDuration,
        CaptureMaxWarmupDuration,
        *CaptureOutputDirectory,
        bDirectVideoEncode ? TEXT(" (direct ffmpeg encoding)") : TEXT("")
    );
}

bool AMyGameManager::InitializeReplayCsvFromCommandLine(FString& OutError)
{
    FParse::Value(FCommandLine::Get(), TEXT("ReplayCsvDir="), ReplayCsvDirectory);
    ReplayCsvDirectory.TrimQuotesInline();
    if (ReplayCsvDirectory.IsEmpty())
    {
        return true;
    }

    ReplayCsvDirectory = FPaths::ConvertRelativePathToFull(ReplayCsvDirectory);
    FPaths::NormalizeDirectoryName(ReplayCsvDirectory);
    if (!FPaths::DirectoryExists(ReplayCsvDirectory))
    {
        OutError = FString::Printf(TEXT("Replay CSV directory not found: %s"), *ReplayCsvDirectory);
        return false;
    }

    UDataTable* RuntimeManifest = LoadRuntimeCsvTable(
        TEXT("Match_Manifest.csv"),
        FManifestData::StaticStruct(),
        TEXT("manifest")
    );
    if (!RuntimeManifest)
    {
        OutError = FString::Printf(
            TEXT("Could not load a valid Match_Manifest.csv from %s"),
            *ReplayCsvDirectory
        );
        return false;
    }

    ManifestTable = RuntimeManifest;
    UE_LOG(
        LogTemp,
        Display,
        TEXT("Using runtime replay CSV directory: %s (%d manifest rows)."),
        *ReplayCsvDirectory,
        ManifestTable->GetRowMap().Num()
    );
    return true;
}

UDataTable* AMyGameManager::LoadRuntimeCsvTable(
    const FString& CsvFilename,
    UScriptStruct* RowStruct,
    const TCHAR* TableDescription
)
{
    if (ReplayCsvDirectory.IsEmpty() || CsvFilename.IsEmpty() || !RowStruct)
    {
        return nullptr;
    }

    // The converter writes a flat directory. Ignoring any path component also
    // prevents a manifest entry from reading outside the selected replay folder.
    const FString CleanFilename = FPaths::GetCleanFilename(CsvFilename);
    if (!CleanFilename.Equals(CsvFilename, ESearchCase::CaseSensitive))
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Replay CSV %s filename contained a path; using %s."),
            TableDescription,
            *CleanFilename
        );
    }

    const FString CsvPath = FPaths::Combine(ReplayCsvDirectory, CleanFilename);
    FString CsvContents;
    if (!FFileHelper::LoadFileToString(CsvContents, *CsvPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not read replay %s CSV: %s"), TableDescription, *CsvPath);
        return nullptr;
    }

    UDataTable* Table = NewObject<UDataTable>(this);
    Table->RowStruct = RowStruct;
    const TArray<FString> ImportProblems = Table->CreateTableFromCSVString(CsvContents);
    for (const FString& Problem : ImportProblems)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Replay %s CSV import warning (%s): %s"),
            TableDescription,
            *CleanFilename,
            *Problem
        );
    }

    if (Table->GetRowMap().IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("Replay %s CSV has no usable rows: %s"), TableDescription, *CsvPath);
        return nullptr;
    }

    RuntimeCsvTables.Add(Table);
    return Table;
}

void AMyGameManager::ConfigureHighQualityCapture()
{
    auto SetIntegerCVar = [](const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            Variable->Set(Value, ECVF_SetByCode);
        }
    };
    auto SetFloatCVar = [](const TCHAR* Name, float Value)
    {
        if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            Variable->Set(Value, ECVF_SetByCode);
        }
    };

    SetIntegerCVar(TEXT("r.AntiAliasingMethod"), 4);
    SetIntegerCVar(TEXT("r.PostProcessAAQuality"), 6);
    SetFloatCVar(TEXT("r.TSR.History.ScreenPercentage"), 200.0f);
    SetIntegerCVar(TEXT("r.MotionBlurQuality"), 0);
    SetIntegerCVar(TEXT("r.DefaultFeature.MotionBlur"), 0);
    SetIntegerCVar(TEXT("r.DefaultFeature.AutoExposure"), 0);
    SetIntegerCVar(TEXT("r.EyeAdaptationQuality"), 0);
    SetFloatCVar(TEXT("r.Tonemapper.Sharpen"), 0.35f);
    SetIntegerCVar(TEXT("r.MaxAnisotropy"), 16);

    for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
    {
        ACesium3DTileset* Tileset = *It;
        if (!IsValid(Tileset))
        {
            continue;
        }

        Tileset->SetMaximumScreenSpaceError(4.0);
        Tileset->MaximumSimultaneousTileLoads = FMath::Max(
            Tileset->MaximumSimultaneousTileLoads,
            40
        );
        Tileset->MaximumCachedBytes = FMath::Max<int64>(
            Tileset->MaximumCachedBytes,
            1024LL * 1024LL * 1024LL
        );
        Tileset->ForbidHoles = true;

        TArray<UCesiumRasterOverlay*> RasterOverlays;
        Tileset->GetComponents<UCesiumRasterOverlay>(RasterOverlays);
        for (UCesiumRasterOverlay* RasterOverlay : RasterOverlays)
        {
            if (!IsValid(RasterOverlay))
            {
                continue;
            }

            // Native 1440p already requests roughly twice the raster detail of
            // the old 1080p capture. SSE 1 exposed strongly color-mismatched
            // source imagery tiles in this area, so use 4 pixels here: it
            // preserves the former geographic LOD while the 2K output keeps
            // labels and model silhouettes substantially sharper.
            RasterOverlay->SetMaximumScreenSpaceError(4.0);
            RasterOverlay->SetMaximumTextureSize(4096);
            RasterOverlay->SetMaximumSimultaneousTileLoads(40);
            RasterOverlay->Refresh();
        }
    }

    if (GetWorld())
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.Owner = this;
        SpawnParameters.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        CaptureFillLight = GetWorld()->SpawnActor<ADirectionalLight>(
            ADirectionalLight::StaticClass(),
            FVector::ZeroVector,
            FRotator(-90.0f, 0.0f, 0.0f),
            SpawnParameters
        );
        if (IsValid(CaptureFillLight))
        {
            UDirectionalLightComponent* LightComponent =
                Cast<UDirectionalLightComponent>(CaptureFillLight->GetLightComponent());
            if (LightComponent)
            {
                LightComponent->SetIntensity(1.2f);
                LightComponent->SetLightColor(FLinearColor(0.86f, 0.91f, 1.0f));
                LightComponent->SetCastShadows(false);
                LightComponent->SetVolumetricScatteringIntensity(0.0f);
            }
        }
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("High-quality capture configured: TSR 200%% history, fixed exposure, no motion blur, Cesium SSE 4 / raster SSE 4 at native 2K.")
    );
}

void AMyGameManager::InitializeOverviewCamera()
{
    if (!bEnableDynamicOverviewCamera || !GetWorld())
    {
        return;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Owner = this;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    OverviewCamera = GetWorld()->SpawnActor<ACameraActor>(
        ACameraActor::StaticClass(),
        FVector::ZeroVector,
        FRotator::ZeroRotator,
        SpawnParameters
    );
    if (!OverviewCamera)
    {
        UE_LOG(LogTemp, Warning, TEXT("Could not create the dynamic overview camera."));
        return;
    }

#if WITH_EDITOR
    OverviewCamera->SetActorLabel(TEXT("Runtime_TacticalOverviewCamera"));
#endif
    OverviewCamera->GetCameraComponent()->SetFieldOfView(OverviewFieldOfViewDegrees);

    if (APlayerController* PlayerController = GetWorld()->GetFirstPlayerController())
    {
        PlayerController->SetViewTarget(OverviewCamera);
    }
}

void AMyGameManager::UpdateOverviewCamera(float DeltaSeconds, bool bSnap)
{
    if (!bEnableDynamicOverviewCamera || !IsValid(OverviewCamera) || !CesiumRef || !GetWorld())
    {
        return;
    }

    TArray<FVector> PlatformLocations;
    PlatformLocations.Reserve(SpawnedEntities.Num());
    for (const AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (!IsValid(Entity) || !Entity->IsCurrentlyActive())
        {
            continue;
        }
        if (Entity->GetEntityCategory().Equals(TEXT("Missile"), ESearchCase::IgnoreCase) ||
            Entity->GetEntityCategory().Equals(TEXT("Munition"), ESearchCase::IgnoreCase) ||
            Entity->GetEntityCategory().Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
        {
            continue;
        }
        PlatformLocations.Add(Entity->GetActorLocation());
    }

    if (PlatformLocations.IsEmpty())
    {
        return;
    }

    FVector FocusWorld = FVector::ZeroVector;
    for (const FVector& Location : PlatformLocations)
    {
        FocusWorld += Location;
    }
    FocusWorld /= static_cast<double>(PlatformLocations.Num());

    const FVector FocusLLH = CesiumRef->TransformUnrealPositionToLongitudeLatitudeHeight(FocusWorld);
    const FVector GeographicOrigin = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(FocusLLH);
    const FVector NorthWorld = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(FocusLLH.X, FocusLLH.Y + 0.01, FocusLLH.Z)
    );
    const FVector EastWorld = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(FocusLLH.X + 0.01, FocusLLH.Y, FocusLLH.Z)
    );
    const FVector NorthDirection = (NorthWorld - GeographicOrigin).GetSafeNormal();
    const FVector EastDirection = (EastWorld - GeographicOrigin).GetSafeNormal();

    float HalfEastMeters = FMath::Max(MinimumFramedWidthMeters * 0.5f, 1.0f);
    float HalfNorthMeters = 1.0f;
    for (const FVector& Location : PlatformLocations)
    {
        const FVector Offset = Location - FocusWorld;
        HalfEastMeters = FMath::Max(HalfEastMeters, FMath::Abs(FVector::DotProduct(Offset, EastDirection)) / 100.0f);
        HalfNorthMeters = FMath::Max(HalfNorthMeters, FMath::Abs(FVector::DotProduct(Offset, NorthDirection)) / 100.0f);
    }

    int32 ViewportWidth = 1920;
    int32 ViewportHeight = 1080;
    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController)
    {
        PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
    }
    const float AspectRatio = ViewportHeight > 0
        ? static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight)
        : 16.0f / 9.0f;
    HalfNorthMeters = FMath::Max(HalfNorthMeters, HalfEastMeters / FMath::Max(AspectRatio, 0.1f));

    const float HorizontalFovRadians = FMath::DegreesToRadians(
        FMath::Clamp(OverviewFieldOfViewDegrees, 30.0f, 100.0f)
    );
    const float VerticalFovRadians = 2.0f * FMath::Atan(
        FMath::Tan(HorizontalFovRadians * 0.5f) / FMath::Max(AspectRatio, 0.1f)
    );
    const float Margin = FMath::Max(OverviewFrameMargin, 1.0f);
    const float RequiredDistanceForWidth = HalfEastMeters * Margin / FMath::Tan(HorizontalFovRadians * 0.5f);
    const float RequiredDistanceForHeight = HalfNorthMeters * Margin / FMath::Tan(VerticalFovRadians * 0.5f);
    const float RequiredCameraDistanceMeters = FMath::Max(
        RequiredDistanceForWidth,
        RequiredDistanceForHeight
    );
    float CameraDistanceMeters = FMath::Clamp(
        RequiredCameraDistanceMeters,
        MinimumCameraDistanceMeters,
        FMath::Max(MaximumCameraDistanceMeters, MinimumCameraDistanceMeters)
    );
    if (bAlwaysKeepAircraftInFrame &&
        RequiredCameraDistanceMeters > MaximumCameraDistanceMeters)
    {
        CameraDistanceMeters = RequiredCameraDistanceMeters;
    }

    const FVector DesiredCameraLocation = CesiumRef->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector(FocusLLH.X, FocusLLH.Y, FocusLLH.Z + CameraDistanceMeters)
    );
    const FVector ViewDirection = (FocusWorld - DesiredCameraLocation).GetSafeNormal(
        SMALL_NUMBER,
        -FVector::UpVector
    );
    const FRotator DesiredCameraRotation = FRotationMatrix::MakeFromXZ(
        ViewDirection,
        NorthDirection
    ).Rotator();

    if (bSnap || DeltaSeconds <= 0.0f)
    {
        OverviewCamera->SetActorLocationAndRotation(DesiredCameraLocation, DesiredCameraRotation);
    }
    else
    {
        const float BlendAlpha = 1.0f - FMath::Exp(-FMath::Max(OverviewCameraInterpSpeed, 0.1f) * DeltaSeconds);
        const FVector SmoothedLocation = FMath::Lerp(
            OverviewCamera->GetActorLocation(),
            DesiredCameraLocation,
            BlendAlpha
        );
        const FQuat SmoothedRotation = FQuat::Slerp(
            OverviewCamera->GetActorQuat(),
            DesiredCameraRotation.Quaternion(),
            BlendAlpha
        ).GetNormalized();
        OverviewCamera->SetActorLocationAndRotation(SmoothedLocation, SmoothedRotation);
    }

    OverviewCamera->GetCameraComponent()->SetFieldOfView(OverviewFieldOfViewDegrees);
    if (PlayerController && PlayerController->GetViewTarget() != OverviewCamera)
    {
        PlayerController->SetViewTarget(OverviewCamera);
    }
}

void AMyGameManager::UpdateEntityLabelLayout()
{
    if (!GetWorld())
    {
        return;
    }

    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (!PlayerController)
    {
        return;
    }

    FVector LabelCameraLocation = FVector::ZeroVector;
    FRotator LabelCameraRotation = FRotator::ZeroRotator;
    if (IsValid(OverviewCamera))
    {
        LabelCameraLocation = OverviewCamera->GetActorLocation();
        LabelCameraRotation = OverviewCamera->GetActorRotation();
    }
    else if (PlayerController->PlayerCameraManager)
    {
        LabelCameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
        LabelCameraRotation = PlayerController->PlayerCameraManager->GetCameraRotation();
    }
    else
    {
        return;
    }

    // Keep every label rigidly anchored to its own entity. Dynamic collision
    // avoidance caused labels to jump between candidate positions when nearby
    // entities crossed, which could make a label appear to point into empty air.
    for (AMyReplayEntity* Entity : SpawnedEntities)
    {
        if (IsValid(Entity) && Entity->IsCurrentlyActive() && Entity->bShowEntityLabel)
        {
            Entity->SetLabelLayoutOffset(
                FVector2D::ZeroVector,
                LabelCameraLocation,
                LabelCameraRotation
            );
        }
    }
}

bool AMyGameManager::AreCesiumTilesReady(float& OutLoadProgress) const
{
    OutLoadProgress = 100.0f;
    int32 TilesetCount = 0;

    for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
    {
        if (!IsValid(*It) || It->IsHidden())
        {
            continue;
        }
        ++TilesetCount;
        OutLoadProgress = FMath::Min(OutLoadProgress, It->GetLoadProgress());
    }

    // A map without a Cesium tileset should not be blocked by the capture safeguard.
    return TilesetCount == 0 || OutLoadProgress >= 99.0f;
}

void AMyGameManager::RequestCaptureFrame()
{
    const FString Filename = FPaths::Combine(
        CaptureOutputDirectory,
        FString::Printf(TEXT("Frame_%06d.png"), CaptureFrameNumber)
    );
    // Capture HUD is drawn into the viewport canvas, so Slate UI capture is not needed.
    FScreenshotRequest::RequestScreenshot(Filename, false, false);
    ++CaptureFrameNumber;
}

void AMyGameManager::OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Colors)
{
    if (!bDirectVideoEncode || bCaptureFailed || Width <= 0 || Height <= 0 || Colors.IsEmpty())
    {
        return;
    }

    if (!EncoderProcessHandle.IsValid() && !StartDirectVideoEncoder(Width, Height))
    {
        bCaptureFailed = true;
        bCaptureExitPending = true;
        return;
    }

    const uint8* PixelBytes = reinterpret_cast<const uint8*>(Colors.GetData());
    const int32 TotalBytes = Colors.Num() * sizeof(FColor);
    int32 TotalWritten = 0;
    while (TotalWritten < TotalBytes)
    {
        int32 BytesWritten = 0;
        const bool bWriteSucceeded = FPlatformProcess::WritePipe(
            EncoderInputWritePipe,
            PixelBytes + TotalWritten,
            TotalBytes - TotalWritten,
            &BytesWritten
        );
        if (!bWriteSucceeded || BytesWritten <= 0)
        {
            bCaptureFailed = true;
            bCaptureExitPending = true;
            CaptureFailureReason = FString::Printf(
                TEXT("Failed to stream capture frame %d to ffmpeg."),
                CaptureFrameNumber
            );
            UE_LOG(LogTemp, Error, TEXT("%s"), *CaptureFailureReason);
            return;
        }
        TotalWritten += BytesWritten;
    }
}

bool AMyGameManager::StartDirectVideoEncoder(int32 Width, int32 Height)
{
    if (!FPlatformProcess::CreatePipe(EncoderInputReadPipe, EncoderInputWritePipe, true))
    {
        CaptureFailureReason = TEXT("Could not create the ffmpeg input pipe.");
        UE_LOG(LogTemp, Error, TEXT("%s"), *CaptureFailureReason);
        return false;
    }

    const FString EncoderArguments = FString::Printf(
        TEXT("-y -hide_banner -loglevel warning -f rawvideo -pixel_format bgra -video_size %dx%d -framerate %d -i pipe:0 -an -c:v libx264 -preset slow -crf 15 -profile:v high -pix_fmt yuv420p -movflags +faststart \"%s\""),
        Width,
        Height,
        CaptureFrameRate,
        *CaptureVideoFilename
    );
    EncoderProcessHandle = FPlatformProcess::CreateProc(
        *CaptureFfmpegExecutable,
        *EncoderArguments,
        false,
        true,
        true,
        nullptr,
        0,
        *FPaths::GetPath(CaptureVideoFilename),
        nullptr,
        EncoderInputReadPipe
    );

    // The child owns its inherited read handle after CreateProc succeeds.
    FPlatformProcess::ClosePipe(EncoderInputReadPipe, nullptr);
    EncoderInputReadPipe = nullptr;

    if (!EncoderProcessHandle.IsValid())
    {
        FPlatformProcess::ClosePipe(nullptr, EncoderInputWritePipe);
        EncoderInputWritePipe = nullptr;
        CaptureFailureReason = FString::Printf(
            TEXT("Could not start ffmpeg: %s"),
            *CaptureFfmpegExecutable
        );
        UE_LOG(LogTemp, Error, TEXT("%s"), *CaptureFailureReason);
        return false;
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Streaming %dx%d BGRA frames directly to ffmpeg: %s"),
        Width,
        Height,
        *CaptureVideoFilename
    );
    return true;
}

bool AMyGameManager::FinishDirectVideoEncoder()
{
    if (!bEncoderFinalizeStarted)
    {
        if (ScreenshotCapturedHandle.IsValid())
        {
            UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotCapturedHandle);
            ScreenshotCapturedHandle.Reset();
        }
        FPlatformProcess::ClosePipe(EncoderInputReadPipe, EncoderInputWritePipe);
        EncoderInputReadPipe = nullptr;
        EncoderInputWritePipe = nullptr;
        bEncoderFinalizeStarted = true;
    }

    if (!EncoderProcessHandle.IsValid())
    {
        return true;
    }
    if (FPlatformProcess::IsProcRunning(EncoderProcessHandle))
    {
        return false;
    }

    int32 EncoderReturnCode = INDEX_NONE;
    FPlatformProcess::GetProcReturnCode(EncoderProcessHandle, &EncoderReturnCode);
    FPlatformProcess::CloseProc(EncoderProcessHandle);
    EncoderProcessHandle.Reset();

    if (EncoderReturnCode != 0 || IFileManager::Get().FileSize(*CaptureVideoFilename) <= 0)
    {
        bCaptureFailed = true;
        CaptureFailureReason = FString::Printf(
            TEXT("ffmpeg did not produce a valid MP4 (exit code %d)."),
            EncoderReturnCode
        );
        UE_LOG(LogTemp, Error, TEXT("%s"), *CaptureFailureReason);
    }
    return true;
}

void AMyGameManager::CloseDirectVideoEncoder()
{
    if (ScreenshotCapturedHandle.IsValid())
    {
        UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotCapturedHandle);
        ScreenshotCapturedHandle.Reset();
    }
    FPlatformProcess::ClosePipe(EncoderInputReadPipe, EncoderInputWritePipe);
    EncoderInputReadPipe = nullptr;
    EncoderInputWritePipe = nullptr;

    if (EncoderProcessHandle.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(EncoderProcessHandle))
        {
            FPlatformProcess::TerminateProc(EncoderProcessHandle, true);
        }
        FPlatformProcess::CloseProc(EncoderProcessHandle);
        EncoderProcessHandle.Reset();
    }
}

void AMyGameManager::DrawCaptureOverlay(UCanvas* Canvas, APlayerController* PlayerController)
{
    if (!Canvas || !GEngine)
    {
        return;
    }

    const float ViewWidth = static_cast<float>(Canvas->SizeX);
    const float ViewHeight = static_cast<float>(Canvas->SizeY);
    const float UIScale = FMath::Clamp(ViewHeight / 1080.0f, 0.75f, 2.0f);
    const UFont* Font = GEngine->GetSmallFont();

    auto DrawSolid = [Canvas](float X, float Y, float Width, float Height, const FLinearColor& Color)
    {
        Canvas->K2_DrawTexture(
            nullptr,
            FVector2D(X, Y),
            FVector2D(Width, Height),
            FVector2D::ZeroVector,
            FVector2D::UnitVector,
            Color,
            BLEND_Translucent
        );
    };
    auto DrawLabel = [Canvas, Font, UIScale](
        const FString& Text,
        float X,
        float Y,
        const FLinearColor& Color,
        float RelativeScale = 1.0f)
    {
        Canvas->K2_DrawText(
            const_cast<UFont*>(Font),
            Text,
            FVector2D(X, Y),
            FVector2D(UIScale * RelativeScale),
            Color,
            0.0f,
            FLinearColor::Black,
            FVector2D(UIScale),
            false,
            false,
            false,
            FLinearColor::Black
        );
    };

    if (bBlackScreenAtEnd && bReplayFinished)
    {
        DrawSolid(0.0f, 0.0f, ViewWidth, ViewHeight, FLinearColor::Black);
        return;
    }

    if (PlayerController)
    {
        for (const AMyReplayEntity* Entity : SpawnedEntities)
        {
            if (!IsValid(Entity) || !Entity->IsCurrentlyActive())
            {
                continue;
            }

            FVector2D ScreenPosition;
            if (!PlayerController->ProjectWorldLocationToScreen(
                    Entity->GetActorLocation(),
                    ScreenPosition,
                    true
                ) ||
                ScreenPosition.X < 0.0f ||
                ScreenPosition.Y < 0.0f ||
                ScreenPosition.X > ViewWidth ||
                ScreenPosition.Y > ViewHeight)
            {
                continue;
            }

            FLinearColor TeamColor = FLinearColor::White;
            if (Entity->GetTeam().Equals(TEXT("Red"), ESearchCase::IgnoreCase))
            {
                TeamColor = FLinearColor(1.0f, 0.34f, 0.34f, 1.0f);
            }
            else if (Entity->GetTeam().Equals(TEXT("Blue"), ESearchCase::IgnoreCase))
            {
                TeamColor = FLinearColor(0.42f, 0.78f, 1.0f, 1.0f);
            }

            const FString EntityText = FString::Printf(
                TEXT("%s | %d m"),
                *Entity->GetEntityId(),
                FMath::RoundToInt(Entity->GetCurrentAltitudeMeters())
            );
            Canvas->K2_DrawText(
                const_cast<UFont*>(Font),
                EntityText,
                ScreenPosition - FVector2D(0.0f, 27.0f * UIScale),
                FVector2D(0.92f * UIScale),
                TeamColor,
                0.0f,
                FLinearColor(0.0f, 0.0f, 0.0f, 0.9f),
                FVector2D(1.2f * UIScale),
                true,
                false,
                true,
                FLinearColor(0.0f, 0.0f, 0.0f, 0.92f)
            );
        }
    }

    const FLinearColor PanelColor(0.015f, 0.025f, 0.045f, 0.82f);
    const FLinearColor MutedText(0.72f, 0.78f, 0.86f, 1.0f);
    const FLinearColor Cyan(0.20f, 0.86f, 1.0f, 1.0f);
    const float Margin = 20.0f * UIScale;
    const float ReplayPanelWidth = 500.0f * UIScale;
    const float ReplayPanelHeight = 142.0f * UIScale;
    const float Padding = 14.0f * UIScale;

    DrawSolid(Margin, Margin, ReplayPanelWidth, ReplayPanelHeight, PanelColor);
    DrawLabel(TEXT("TACTICAL REPLAY"), Margin + Padding, Margin + 9.0f * UIScale, Cyan, 1.15f);
    if (bCaptureRound && !bCaptureStarted)
    {
        DrawLabel(
            FString::Printf(TEXT("LOADING CESIUM MAP  %.0f%%"), LastCesiumLoadProgress),
            Margin + 205.0f * UIScale,
            Margin + 9.0f * UIScale,
            FLinearColor(1.0f, 0.82f, 0.25f, 1.0f)
        );
    }
    DrawLabel(
        FString::Printf(
            TEXT("TIME %s / %s    RATE %.2gx"),
            *UReplayHUDWidget::FormatReplayTime(ReplayTime),
            *UReplayHUDWidget::FormatReplayTime(ReplayDuration),
            PlaybackRate
        ),
        Margin + Padding,
        Margin + 32.0f * UIScale,
        FLinearColor::White
    );

    int32 RedCount = 0;
    int32 BlueCount = 0;
    GetAliveCounts(RedCount, BlueCount);
    DrawLabel(
        FString::Printf(TEXT("RED  %d ALIVE       BLUE  %d ALIVE"), RedCount, BlueCount),
        Margin + Padding,
        Margin + 52.0f * UIScale,
        MutedText
    );

    const float ProgressX = Margin + Padding;
    const float ProgressY = Margin + 77.0f * UIScale;
    const float ProgressWidth = ReplayPanelWidth - 2.0f * Padding;
    DrawSolid(ProgressX, ProgressY, ProgressWidth, 6.0f * UIScale, FLinearColor(0.55f, 0.58f, 0.62f, 1.0f));
    DrawSolid(ProgressX, ProgressY, ProgressWidth * GetReplayProgress(), 6.0f * UIScale, Cyan);

    DrawLabel(TEXT("-     PAUSE / RESUME     +"), ProgressX, Margin + 99.0f * UIScale, FLinearColor::White);

    const float MapPanelWidth = 250.0f * UIScale;
    const float MapPanelHeight = 142.0f * UIScale;
    const float MapX = ViewWidth - Margin - MapPanelWidth;
    DrawSolid(MapX, Margin, MapPanelWidth, MapPanelHeight, PanelColor);
    DrawLabel(TEXT("NORTH"), MapX + Padding, Margin + 9.0f * UIScale, MutedText);

    float NorthAngle = 0.0f;
    float ScaleMeters = 10000.0f;
    float ScalePixels = 120.0f;
    GetMapOverlayData(NorthAngle, ScaleMeters, ScalePixels);
    const float NorthRadians = FMath::DegreesToRadians(NorthAngle);
    const FVector2D ArrowCenter(MapX + 112.0f * UIScale, Margin + 23.0f * UIScale);
    const FVector2D ArrowDirection(FMath::Sin(NorthRadians), -FMath::Cos(NorthRadians));
    const FVector2D ArrowRight(-ArrowDirection.Y, ArrowDirection.X);
    const FVector2D ArrowTip = ArrowCenter + ArrowDirection * 16.0f * UIScale;
    Canvas->K2_DrawLine(ArrowCenter - ArrowDirection * 9.0f * UIScale, ArrowTip, 2.0f * UIScale, FLinearColor::White);
    Canvas->K2_DrawLine(ArrowTip, ArrowTip - ArrowDirection * 6.0f * UIScale + ArrowRight * 4.0f * UIScale, 2.0f * UIScale, FLinearColor::White);
    Canvas->K2_DrawLine(ArrowTip, ArrowTip - ArrowDirection * 6.0f * UIScale - ArrowRight * 4.0f * UIScale, 2.0f * UIScale, FLinearColor::White);

    const FString ScaleLabel = ScaleMeters >= 1000.0f
        ? FString::Printf(TEXT("SCALE %.3g km"), ScaleMeters / 1000.0f)
        : FString::Printf(TEXT("SCALE %.0f m"), ScaleMeters);
    DrawLabel(ScaleLabel, MapX + Padding, Margin + 51.0f * UIScale, MutedText);
    Canvas->K2_DrawLine(
        FVector2D(MapX + Padding, Margin + 77.0f * UIScale),
        FVector2D(MapX + Padding + ScalePixels * UIScale, Margin + 77.0f * UIScale),
        3.0f * UIScale,
        FLinearColor::White
    );

    DrawSolid(MapX + Padding, Margin + 98.0f * UIScale, 11.0f * UIScale, 11.0f * UIScale, FLinearColor(1.0f, 0.2f, 0.2f));
    DrawLabel(TEXT("RED"), MapX + 30.0f * UIScale, Margin + 94.0f * UIScale, FLinearColor::White);
    DrawSolid(MapX + 101.0f * UIScale, Margin + 98.0f * UIScale, 11.0f * UIScale, 11.0f * UIScale, FLinearColor(0.2f, 0.58f, 1.0f));
    DrawLabel(TEXT("BLUE"), MapX + 117.0f * UIScale, Margin + 94.0f * UIScale, FLinearColor::White);
}
