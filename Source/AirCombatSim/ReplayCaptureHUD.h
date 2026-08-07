#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ReplayCaptureHUD.generated.h"

class AMyGameManager;

UCLASS()
class AIRCOMBATSIM_API AReplayCaptureHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

protected:
    virtual void BeginPlay() override;

private:
    UPROPERTY()
    AMyGameManager* Manager = nullptr;
};
