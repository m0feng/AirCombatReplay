#include "ReplayCaptureHUD.h"

#include "EngineUtils.h"
#include "MyGameManager.h"

void AReplayCaptureHUD::BeginPlay()
{
    Super::BeginPlay();

    for (TActorIterator<AMyGameManager> It(GetWorld()); It; ++It)
    {
        Manager = *It;
        break;
    }
}

void AReplayCaptureHUD::DrawHUD()
{
    Super::DrawHUD();

    if (Manager)
    {
        Manager->DrawCaptureOverlay(Canvas, PlayerOwner);
    }
}
