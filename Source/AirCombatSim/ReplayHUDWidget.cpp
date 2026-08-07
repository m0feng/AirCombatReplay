#include "ReplayHUDWidget.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "MyGameManager.h"
#include "Styling/CoreStyle.h"

namespace ReplayHUD
{
    const FLinearColor PanelColor(0.015f, 0.025f, 0.045f, 0.82f);
    const FLinearColor MutedText(0.72f, 0.78f, 0.86f, 1.0f);
    const FLinearColor Cyan(0.20f, 0.86f, 1.0f, 1.0f);
    const FLinearColor Red(1.0f, 0.20f, 0.20f, 1.0f);
    const FLinearColor Blue(0.20f, 0.58f, 1.0f, 1.0f);
}

void UReplayHUDWidget::InitializeWithManager(AMyGameManager* InManager)
{
    Manager = InManager;
}

TSharedRef<SWidget> UReplayHUDWidget::RebuildWidget()
{
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        BuildWidgetTree();
    }
    return Super::RebuildWidget();
}

UTextBlock* UReplayHUDWidget::MakeText(const FString& Text, int32 FontSize, const FLinearColor& Color)
{
    UTextBlock* TextBlock = WidgetTree->ConstructWidget<UTextBlock>();
    TextBlock->SetText(FText::FromString(Text));
    TextBlock->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), FontSize));
    TextBlock->SetColorAndOpacity(FSlateColor(Color));
    TextBlock->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
    TextBlock->SetShadowOffset(FVector2D(1.0f, 1.0f));
    return TextBlock;
}

void UReplayHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();
}

void UReplayHUDWidget::BuildWidgetTree()
{
    UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ReplayHUDRoot"));
    WidgetTree->RootWidget = RootCanvas;

    // Transparent full-screen hit surface makes entity selection work away from HUD panels.
    UBorder* ClickSurface = WidgetTree->ConstructWidget<UBorder>();
    ClickSurface->SetBrushColor(FLinearColor::Transparent);
    UCanvasPanelSlot* ClickSurfaceSlot = RootCanvas->AddChildToCanvas(ClickSurface);
    ClickSurfaceSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
    ClickSurfaceSlot->SetOffsets(FMargin(0.0f));
    ClickSurfaceSlot->SetZOrder(-100);

    // Replay information and controls.
    UBorder* ReplayPanel = WidgetTree->ConstructWidget<UBorder>();
    ReplayPanel->SetBrushColor(ReplayHUD::PanelColor);
    ReplayPanel->SetPadding(FMargin(14.0f, 10.0f));
    UCanvasPanelSlot* ReplayPanelSlot = RootCanvas->AddChildToCanvas(ReplayPanel);
    ReplayPanelSlot->SetAnchors(FAnchors(0.0f, 0.0f));
    ReplayPanelSlot->SetPosition(FVector2D(20.0f, 20.0f));
    ReplayPanelSlot->SetSize(FVector2D(500.0f, 154.0f));

    UVerticalBox* ReplayBox = WidgetTree->ConstructWidget<UVerticalBox>();
    ReplayPanel->SetContent(ReplayBox);

    UTextBlock* ReplayTitle = MakeText(TEXT("TACTICAL REPLAY"), 17, ReplayHUD::Cyan);
    ReplayBox->AddChildToVerticalBox(ReplayTitle)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 3.0f));

    StatusText = MakeText(TEXT("TIME 00:00.0 / 00:00.0    RATE 1.0x"), 15, FLinearColor::White);
    ReplayBox->AddChildToVerticalBox(StatusText);

    CountText = MakeText(TEXT("RED 0    BLUE 0"), 14, ReplayHUD::MutedText);
    ReplayBox->AddChildToVerticalBox(CountText)->SetPadding(FMargin(0.0f, 1.0f, 0.0f, 4.0f));

    USizeBox* ProgressSize = WidgetTree->ConstructWidget<USizeBox>();
    ProgressSize->SetHeightOverride(8.0f);
    ProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
    ProgressBar->SetFillColorAndOpacity(ReplayHUD::Cyan);
    ProgressBar->SetPercent(0.0f);
    ProgressSize->SetContent(ProgressBar);
    ReplayBox->AddChildToVerticalBox(ProgressSize)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));

    UHorizontalBox* ControlRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    ReplayBox->AddChildToVerticalBox(ControlRow);

    UButton* SlowerButton = WidgetTree->ConstructWidget<UButton>();
    SlowerButton->SetContent(MakeText(TEXT("  -  "), 15, FLinearColor::White));
    SlowerButton->OnClicked.AddDynamic(this, &UReplayHUDWidget::HandleSlowerClicked);
    ControlRow->AddChildToHorizontalBox(SlowerButton)->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));

    UButton* PauseButton = WidgetTree->ConstructWidget<UButton>();
    PauseButtonText = MakeText(TEXT("  PAUSE / RESUME  "), 15, FLinearColor::White);
    PauseButton->SetContent(PauseButtonText);
    PauseButton->OnClicked.AddDynamic(this, &UReplayHUDWidget::HandlePauseClicked);
    ControlRow->AddChildToHorizontalBox(PauseButton)->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));

    UButton* FasterButton = WidgetTree->ConstructWidget<UButton>();
    FasterButton->SetContent(MakeText(TEXT("  +  "), 15, FLinearColor::White));
    FasterButton->OnClicked.AddDynamic(this, &UReplayHUDWidget::HandleFasterClicked);
    ControlRow->AddChildToHorizontalBox(FasterButton);

    // North, scale and team legend.
    UBorder* MapPanel = WidgetTree->ConstructWidget<UBorder>();
    MapPanel->SetBrushColor(ReplayHUD::PanelColor);
    MapPanel->SetPadding(FMargin(14.0f, 10.0f));
    UCanvasPanelSlot* MapPanelSlot = RootCanvas->AddChildToCanvas(MapPanel);
    MapPanelSlot->SetAnchors(FAnchors(1.0f, 0.0f));
    MapPanelSlot->SetAlignment(FVector2D(1.0f, 0.0f));
    MapPanelSlot->SetPosition(FVector2D(-20.0f, 20.0f));
    MapPanelSlot->SetSize(FVector2D(250.0f, 154.0f));

    UVerticalBox* MapBox = WidgetTree->ConstructWidget<UVerticalBox>();
    MapPanel->SetContent(MapBox);

    UHorizontalBox* NorthRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    MapBox->AddChildToVerticalBox(NorthRow)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));
    NorthRow->AddChildToHorizontalBox(MakeText(TEXT("NORTH  "), 14, ReplayHUD::MutedText));
    NorthArrowText = MakeText(TEXT("^"), 25, FLinearColor::White);
    NorthArrowText->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
    NorthRow->AddChildToHorizontalBox(NorthArrowText);

    ScaleText = MakeText(TEXT("SCALE 10 km"), 14, ReplayHUD::MutedText);
    MapBox->AddChildToVerticalBox(ScaleText);

    ScaleLineBox = WidgetTree->ConstructWidget<USizeBox>();
    ScaleLineBox->SetWidthOverride(120.0f);
    ScaleLineBox->SetHeightOverride(4.0f);
    UBorder* ScaleLine = WidgetTree->ConstructWidget<UBorder>();
    ScaleLine->SetBrushColor(FLinearColor::White);
    ScaleLineBox->SetContent(ScaleLine);
    MapBox->AddChildToVerticalBox(ScaleLineBox)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 10.0f));

    UHorizontalBox* LegendRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    MapBox->AddChildToVerticalBox(LegendRow);

    USizeBox* RedSwatchSize = WidgetTree->ConstructWidget<USizeBox>();
    RedSwatchSize->SetWidthOverride(12.0f);
    RedSwatchSize->SetHeightOverride(12.0f);
    UBorder* RedSwatch = WidgetTree->ConstructWidget<UBorder>();
    RedSwatch->SetBrushColor(ReplayHUD::Red);
    RedSwatchSize->SetContent(RedSwatch);
    LegendRow->AddChildToHorizontalBox(RedSwatchSize)->SetVerticalAlignment(VAlign_Center);
    LegendRow->AddChildToHorizontalBox(MakeText(TEXT(" RED     "), 14, FLinearColor::White));

    USizeBox* BlueSwatchSize = WidgetTree->ConstructWidget<USizeBox>();
    BlueSwatchSize->SetWidthOverride(12.0f);
    BlueSwatchSize->SetHeightOverride(12.0f);
    UBorder* BlueSwatch = WidgetTree->ConstructWidget<UBorder>();
    BlueSwatch->SetBrushColor(ReplayHUD::Blue);
    BlueSwatchSize->SetContent(BlueSwatch);
    LegendRow->AddChildToHorizontalBox(BlueSwatchSize)->SetVerticalAlignment(VAlign_Center);
    LegendRow->AddChildToHorizontalBox(MakeText(TEXT(" BLUE"), 14, FLinearColor::White));

    // Selection details remain compact until an entity is clicked.
    SelectionPanel = WidgetTree->ConstructWidget<UBorder>();
    SelectionPanel->SetBrushColor(ReplayHUD::PanelColor);
    SelectionPanel->SetPadding(FMargin(14.0f, 10.0f));
    UCanvasPanelSlot* SelectionPanelSlot = RootCanvas->AddChildToCanvas(SelectionPanel);
    SelectionPanelSlot->SetAnchors(FAnchors(0.0f, 1.0f));
    SelectionPanelSlot->SetAlignment(FVector2D(0.0f, 1.0f));
    SelectionPanelSlot->SetPosition(FVector2D(20.0f, -20.0f));
    SelectionPanelSlot->SetSize(FVector2D(540.0f, 112.0f));

    UVerticalBox* SelectionBox = WidgetTree->ConstructWidget<UVerticalBox>();
    SelectionPanel->SetContent(SelectionBox);
    SelectionBox->AddChildToVerticalBox(MakeText(TEXT("SELECTED ENTITY"), 15, ReplayHUD::Cyan));
    SelectionText = MakeText(TEXT("CLICK AN ENTITY FOR DETAILS"), 14, FLinearColor::White);
    SelectionText->SetAutoWrapText(true);
    SelectionBox->AddChildToVerticalBox(SelectionText)->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 0.0f));

    // Full-screen black is above every other HUD element and appears at CSV end.
    Blackout = WidgetTree->ConstructWidget<UBorder>();
    Blackout->SetBrushColor(FLinearColor::Black);
    Blackout->SetVisibility(ESlateVisibility::Collapsed);
    UCanvasPanelSlot* BlackoutSlot = RootCanvas->AddChildToCanvas(Blackout);
    BlackoutSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
    BlackoutSlot->SetOffsets(FMargin(0.0f));
    BlackoutSlot->SetZOrder(1000);
}

void UReplayHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);

    if (!Manager)
    {
        return;
    }

    if (StatusText)
    {
        StatusText->SetText(FText::FromString(FString::Printf(
            TEXT("TIME %s / %s    RATE %.2gx"),
            *FormatReplayTime(Manager->GetReplayTime()),
            *FormatReplayTime(Manager->GetReplayDuration()),
            Manager->GetPlaybackRate()
        )));
    }

    if (CountText)
    {
        int32 RedCount = 0;
        int32 BlueCount = 0;
        Manager->GetAliveCounts(RedCount, BlueCount);
        CountText->SetText(FText::FromString(FString::Printf(
            TEXT("RED  %d ALIVE       BLUE  %d ALIVE"),
            RedCount,
            BlueCount
        )));
    }

    if (ProgressBar)
    {
        ProgressBar->SetPercent(Manager->GetReplayProgress());
    }

    if (PauseButtonText)
    {
        const TCHAR* ButtonLabel = Manager->IsReplayFinished()
            ? TEXT("  REPLAY  ")
            : (Manager->IsReplayPaused() ? TEXT("  RESUME  ") : TEXT("  PAUSE  "));
        PauseButtonText->SetText(FText::FromString(ButtonLabel));
    }

    if (SelectionText)
    {
        SelectionText->SetText(FText::FromString(Manager->GetSelectedEntityDetails()));
    }
    if (SelectionPanel)
    {
        SelectionPanel->SetVisibility(
            Manager->HasSelectedEntity() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed
        );
    }

    float NorthAngle = 0.0f;
    float ScaleMeters = 10000.0f;
    float ScalePixels = 120.0f;
    if (Manager->GetMapOverlayData(NorthAngle, ScaleMeters, ScalePixels))
    {
        if (NorthArrowText)
        {
            NorthArrowText->SetRenderTransformAngle(NorthAngle);
        }
        if (ScaleLineBox)
        {
            ScaleLineBox->SetWidthOverride(ScalePixels);
        }
        if (ScaleText)
        {
            const FString ScaleLabel = ScaleMeters >= 1000.0f
                ? FString::Printf(TEXT("SCALE %.3g km"), ScaleMeters / 1000.0f)
                : FString::Printf(TEXT("SCALE %.0f m"), ScaleMeters);
            ScaleText->SetText(FText::FromString(ScaleLabel));
        }
    }

    if (Blackout)
    {
        const bool bShowBlack = Manager->bBlackScreenAtEnd && Manager->IsReplayFinished();
        Blackout->SetVisibility(bShowBlack ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

FReply UReplayHUDWidget::NativeOnMouseButtonDown(
    const FGeometry& InGeometry,
    const FPointerEvent& InMouseEvent
)
{
    if (Manager && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
    {
        const FVector2D WidgetPosition = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
        if (Manager->SelectEntityAtWidgetPosition(WidgetPosition))
        {
            return FReply::Handled();
        }
    }
    return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UReplayHUDWidget::HandleSlowerClicked()
{
    if (Manager)
    {
        Manager->AdjustPlaybackRate(-1);
    }
}

void UReplayHUDWidget::HandlePauseClicked()
{
    if (Manager)
    {
        Manager->TogglePause();
    }
}

void UReplayHUDWidget::HandleFasterClicked()
{
    if (Manager)
    {
        Manager->AdjustPlaybackRate(1);
    }
}

FString UReplayHUDWidget::FormatReplayTime(float Seconds)
{
    const float SafeSeconds = FMath::Max(Seconds, 0.0f);
    const int32 WholeMinutes = FMath::FloorToInt(SafeSeconds / 60.0f);
    const float RemainingSeconds = SafeSeconds - static_cast<float>(WholeMinutes * 60);
    return FString::Printf(TEXT("%02d:%04.1f"), WholeMinutes, RemainingSeconds);
}
