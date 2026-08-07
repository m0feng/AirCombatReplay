#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "MyReplayEntity.h"
#include "SimTypes.h"
#include "MyGameManager.generated.h"

class UReplayHUDWidget;
class UCanvas;
class APlayerController;
class ACameraActor;
class ADirectionalLight;

UCLASS()
class AIRCOMBATSIM_API AMyGameManager : public AActor
{
    GENERATED_BODY()

public:
    AMyGameManager();

    virtual void Tick(float DeltaSeconds) override;

    float GetReplayTime() const { return ReplayTime; }
    float GetReplayDuration() const { return ReplayDuration; }
    float GetPlaybackRate() const { return PlaybackRate; }
    float GetReplayProgress() const;
    bool IsReplayPaused() const { return bReplayPaused; }
    bool IsReplayFinished() const { return bReplayFinished; }
    bool HasSelectedEntity() const { return IsValid(SelectedEntity); }

    void GetAliveCounts(int32& OutRedCount, int32& OutBlueCount) const;
    FString GetSelectedEntityDetails() const;
    bool GetMapOverlayData(float& OutNorthAngleDegrees, float& OutScaleMeters, float& OutScalePixels) const;
    bool SelectEntityAtWidgetPosition(const FVector2D& WidgetPosition);
    void DrawCaptureOverlay(UCanvas* Canvas, APlayerController* PlayerController);

    UFUNCTION(BlueprintCallable, Category = "Replay")
    void TogglePause();

    UFUNCTION(BlueprintCallable, Category = "Replay")
    void AdjustPlaybackRate(int32 Direction);

    UFUNCTION(BlueprintCallable, Category = "Replay")
    void SeekNormalized(float NormalizedTime);

    UFUNCTION(BlueprintCallable, Category = "Replay")
    void RestartReplay();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
    UDataTable* ManifestTable = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data")
    ACesiumGeoreference* CesiumRef = nullptr;

    // Key may be the manifest Type (F16/AIM-9L) or Category (Plane/Missile).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
    TMap<FString, TSubclassOf<AMyReplayEntity>> EntityClassMap;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "0.25", ClampMax = "4.0"))
    float InitialPlaybackRate = 0.5f;

    // Tactical overview camera follows the active aircraft/UAV formation, excluding missiles.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera")
    bool bEnableDynamicOverviewCamera = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "30.0", ClampMax = "100.0", Units = "deg"))
    float OverviewFieldOfViewDegrees = 60.0f;

    // 70,000 ft reference formation width = 21,336 m.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "1000.0", Units = "m"))
    float MinimumFramedWidthMeters = 21336.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "1.0", ClampMax = "3.0"))
    float OverviewFrameMargin = 1.18f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "1000.0", Units = "m"))
    float MinimumCameraDistanceMeters = 12000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "1000.0", Units = "m"))
    float MaximumCameraDistanceMeters = 130000.0f;

    // Treat MaximumCameraDistanceMeters as a preferred ceiling. If a wider
    // formation would otherwise leave the frame, allow the camera to exceed it.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera")
    bool bAlwaysKeepAircraftInFrame = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overview Camera", meta = (ClampMin = "0.1"))
    float OverviewCameraInterpSpeed = 2.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay UI")
    bool bCreateReplayHUD = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay UI", meta = (ClampMin = "4.0"))
    float SelectionRadiusPixels = 36.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay End")
    bool bBlackScreenAtEnd = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay End", meta = (ClampMin = "0.0", Units = "s"))
    float CaptureBlackScreenDuration = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video Capture", meta = (ClampMin = "0.0", Units = "s"))
    float CaptureWarmupDuration = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video Capture", meta = (ClampMin = "10.0", Units = "s"))
    float CaptureMaxWarmupDuration = 90.0f;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void InitializeCaptureFromCommandLine();
    bool InitializeReplayCsvFromCommandLine(FString& OutError);
    UDataTable* LoadRuntimeCsvTable(
        const FString& CsvFilename,
        UScriptStruct* RowStruct,
        const TCHAR* TableDescription
    );
    void ConfigureHighQualityCapture();
    void UpdateAllEntities(float DeltaSeconds);
    void RequestCaptureFrame();
    void OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Colors);
    bool StartDirectVideoEncoder(int32 Width, int32 Height);
    bool FinishDirectVideoEncoder();
    void CloseDirectVideoEncoder();
    bool AreCesiumTilesReady(float& OutLoadProgress) const;
    void InitializeOverviewCamera();
    void UpdateOverviewCamera(float DeltaSeconds, bool bSnap);
    void UpdateEntityLabelLayout();
    void SetSelectedEntity(AMyReplayEntity* NewSelection);

    UPROPERTY()
    TArray<AMyReplayEntity*> SpawnedEntities;

    // Keep command-line CSV tables alive for the whole replay. This allows a
    // newly converted ACMI round to play without importing assets in the editor.
    UPROPERTY()
    TArray<UDataTable*> RuntimeCsvTables;

    UPROPERTY()
    AMyReplayEntity* SelectedEntity = nullptr;

    UPROPERTY()
    UReplayHUDWidget* ReplayHUD = nullptr;

    UPROPERTY()
    ACameraActor* OverviewCamera = nullptr;

    UPROPERTY()
    ADirectionalLight* CaptureFillLight = nullptr;

    float ReplayTime = 0.0f;
    float ReplayDuration = 0.0f;
    float PlaybackRate = 1.0f;
    float FinishedElapsedTime = 0.0f;
    bool bReplayPaused = false;
    bool bReplayFinished = false;

    bool bCaptureRound = false;
    bool bCaptureStarted = false;
    bool bCaptureExitPending = false;
    bool bCaptureFailed = false;
    bool bDirectVideoEncode = false;
    bool bEncoderFinalizeStarted = false;
    int32 CaptureFrameRate = 30;
    int32 CaptureFrameNumber = 0;
    int32 CapturePrewarmStage = 0;
    float CaptureReplayLimit = 0.0f;
    float CaptureWarmupElapsed = 0.0f;
    float CaptureTilesReadyElapsed = 0.0f;
    float CaptureWarmupLogElapsed = 0.0f;
    float LastCesiumLoadProgress = 0.0f;
    FString CaptureFailureReason;
    FString ReplayCsvDirectory;
    FString CaptureOutputDirectory;
    FString CaptureFfmpegExecutable;
    FString CaptureVideoFilename;
    FDelegateHandle ScreenshotCapturedHandle;
    FProcHandle EncoderProcessHandle;
    void* EncoderInputReadPipe = nullptr;
    void* EncoderInputWritePipe = nullptr;
};
