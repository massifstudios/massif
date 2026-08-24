#include "SMassifMainMenu.h"

#include "Style/MassifDesignTokens.h"
#include "Style/MassifWidgetStyles.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** Wide enough for the longest entry, narrow enough to read as a column rather than a bar. */
	constexpr float MenuColumnWidth = 460.0f;

	/** Past this the list scrolls rather than pushing the title off the screen. */
	constexpr float SaveListMaxHeight = 420.0f;

	enum class EMenuFace : int32
	{
		Entries = 0,
		Saves = 1,
	};

	FText EntryNote(const TCHAR* Note)
	{
		return FText::FromString(Note);
	}

	/** An entry without a note keeps its label centred rather than sitting high in an empty box. */
	EVisibility NoteVisibility(const FText& Note)
	{
		if (Note.IsEmpty())
		{
			return EVisibility::Collapsed;
		}
		return EVisibility::HitTestInvisible;
	}

	FText SaveNote(const FMassifSaveListItem& Save)
	{
		if (!Save.bLoadable)
		{
			return FText::FromString(Save.Problem);
		}
		return FText::FromString(Save.Detail);
	}
}

void SMassifMainMenu::Construct(const FArguments& InArgs)
{
	OnNewGame = InArgs._OnNewGame;
	OnSettings = InArgs._OnSettings;
	OnQuit = InArgs._OnQuit;
	OnLoadSave = InArgs._OnLoadSave;
	Saves = InArgs._Saves;
	ElevationModelName = InArgs._ElevationModelName;

	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Center)
		.Padding(MassifDesignTokens::Spacing::Page * 2.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(MenuColumnWidth)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Page)
				[
					BuildTitle()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SAssignNew(Faces, SWidgetSwitcher)
					+ SWidgetSwitcher::Slot()[BuildEntries()]
					+ SWidgetSwitcher::Slot()[BuildSaveList()]
				]
			]
		]
	];
}

TSharedRef<SWidget> SMassifMainMenu::BuildTitle() const
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.TextStyle(&MassifWidgetStyles::Display())
			.Text(NSLOCTEXT("Massif", "MenuTitle", "MASSIF"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, MassifDesignTokens::Spacing::Snug, 0.0f, MassifDesignTokens::Spacing::Snug)
		[
			SNew(SBox)
			.HeightOverride(MassifDesignTokens::HairlineThickness)
			[
				SNew(SImage).Image(MassifWidgetStyles::SeparatorBrush())
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.TextStyle(&MassifWidgetStyles::Label())
			.Text(ElevationModelName)
		];
}

TSharedRef<SWidget> SMassifMainMenu::BuildEntries()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Hair)
		[
			BuildEntry(
				NSLOCTEXT("Massif", "MenuNewGame", "NEW GAME"),
				EntryNote(TEXT("Solve a world from a seed")), OnNewGame, true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Hair)
		[
			BuildEntry(
				NSLOCTEXT("Massif", "MenuLoadGame", "LOAD GAME"), GetLoadGameNote(),
				FSimpleDelegate::CreateSP(this, &SMassifMainMenu::ShowSaveListEntry), !Saves.IsEmpty())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Hair)
		[
			BuildEntry(
				NSLOCTEXT("Massif", "MenuSettings", "SETTINGS"),
				EntryNote(TEXT("Environment simulation fidelity")), OnSettings, true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, MassifDesignTokens::Spacing::Loose, 0.0f, 0.0f)
		[
			BuildEntry(
				NSLOCTEXT("Massif", "MenuQuit", "QUIT"), FText::GetEmpty(), OnQuit, true)
		];
}

TSharedRef<SWidget> SMassifMainMenu::BuildSaveList()
{
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	for (const FMassifSaveListItem& Save : Saves)
	{
		const FString SaveName = Save.Name;
		Column->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, MassifDesignTokens::Spacing::Hair)
			[
				BuildEntry(
					FText::FromString(SaveName), SaveNote(Save),
					FSimpleDelegate::CreateSP(this, &SMassifMainMenu::LoadSaveEntry, SaveName),
					Save.bLoadable)
			];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.MaxHeight(SaveListMaxHeight)
		[
			SNew(SScrollBox) + SScrollBox::Slot()[Column]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, MassifDesignTokens::Spacing::Loose, 0.0f, 0.0f)
		[
			BuildEntry(
				NSLOCTEXT("Massif", "MenuBack", "BACK"), FText::GetEmpty(),
				FSimpleDelegate::CreateSP(this, &SMassifMainMenu::ShowEntriesEntry), true)
		];
}

TSharedRef<SWidget> SMassifMainMenu::BuildEntry(
	FText Label, FText Note, FSimpleDelegate Action, bool bEnabled) const
{
	return SNew(SButton)
		.ButtonStyle(&MassifWidgetStyles::MenuEntry())
		.HAlign(HAlign_Fill)
		.IsEnabled(bEnabled)
		.OnClicked_Lambda([Action]()
		{
			Action.ExecuteIfBound();
			return FReply::Handled();
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.TextStyle(&MassifWidgetStyles::Title())
				.Text(MoveTemp(Label))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.TextStyle(&MassifWidgetStyles::Muted())
				.Visibility(NoteVisibility(Note))
				.Text(MoveTemp(Note))
			]
		];
}

void SMassifMainMenu::ShowSaveListEntry()
{
	Faces->SetActiveWidgetIndex(static_cast<int32>(EMenuFace::Saves));
}

void SMassifMainMenu::ShowEntriesEntry()
{
	Faces->SetActiveWidgetIndex(static_cast<int32>(EMenuFace::Entries));
}

void SMassifMainMenu::LoadSaveEntry(FString SaveName)
{
	OnLoadSave.ExecuteIfBound(MoveTemp(SaveName));
}

FText SMassifMainMenu::GetLoadGameNote() const
{
	if (Saves.IsEmpty())
	{
		return NSLOCTEXT("Massif", "MenuNoSaves", "No saved worlds");
	}
	if (Saves.Num() == 1)
	{
		return NSLOCTEXT("Massif", "MenuOneSave", "1 saved world");
	}
	return FText::FromString(FString::Printf(TEXT("%d saved worlds"), Saves.Num()));
}
