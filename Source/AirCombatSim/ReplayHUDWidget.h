#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ReplayHUDWidget.generated.h"

class AMyGameManager;
class UBorder;
class UProgressBar;
class USizeBox;
class UTextBlock;

UCLASS()
class AIRCOMBATSIM_API UReplayHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    void InitializeWithManager(AMyGameManager* InManager);
    static FString FormatReplayTime(float Seconds);

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
    virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
    UFUNCTION()
    void HandleSlowerClicked();

    UFUNCTION()
    void HandlePauseClicked();

    UFUNCTION()
    void HandleFasterClicked();

    UTextBlock* MakeText(const FString& Text, int32 FontSize, const FLinearColor& Color);
    void BuildWidgetTree();
    UPROPERTY()
    AMyGameManager* Manager = nullptr;

    UPROPERTY()
    UTextBlock* StatusText = nullptr;

    UPROPERTY()
    UTextBlock* CountText = nullptr;

    UPROPERTY()
    UTextBlock* PauseButtonText = nullptr;

    UPROPERTY()
    UProgressBar* ProgressBar = nullptr;

    UPROPERTY()
    UTextBlock* NorthArrowText = nullptr;

    UPROPERTY()
    USizeBox* ScaleLineBox = nullptr;

    UPROPERTY()
    UTextBlock* ScaleText = nullptr;

    UPROPERTY()
    UTextBlock* SelectionText = nullptr;

    UPROPERTY()
    UBorder* SelectionPanel = nullptr;

    UPROPERTY()
    UBorder* Blackout = nullptr;
};
