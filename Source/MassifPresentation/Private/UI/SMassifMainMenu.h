#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SWidgetSwitcher;

/** One save as the menu lists it, already reduced to what is drawn. */
struct FMassifSaveListItem
{
	FString Name;

	/** Seed, elevation model and when it was written, on one line. */
	FString Detail;

	/** False where the save cannot be loaded by this build; Problem says why. */
	bool bLoadable = true;
	FString Problem;
};

DECLARE_DELEGATE_OneParam(FMassifOnLoadSave, FString);

/**
 * The first screen: new game, load game, settings, quit, and the save list behind
 * the load entry.
 *
 * Presentation only. It knows what the entries are called and what they look like,
 * and nothing about what starting or loading a game involves — every entry is a
 * delegate the screen that opened it filled in, and the saves arrive as a list of
 * strings someone else read off disk.
 */
class SMassifMainMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMassifMainMenu) {}
		SLATE_EVENT(FSimpleDelegate, OnNewGame)
		SLATE_EVENT(FSimpleDelegate, OnSettings)
		SLATE_EVENT(FSimpleDelegate, OnQuit)
		SLATE_EVENT(FMassifOnLoadSave, OnLoadSave)

		SLATE_ARGUMENT(TArray<FMassifSaveListItem>, Saves)

		/** Named under the title, because which model the world is cut from is half of what it is. */
		SLATE_ATTRIBUTE(FText, ElevationModelName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> BuildTitle() const;
	TSharedRef<SWidget> BuildEntries();
	TSharedRef<SWidget> BuildSaveList();
	TSharedRef<SWidget> BuildEntry(FText Label, FText Note, FSimpleDelegate Action, bool bEnabled) const;

	FText GetLoadGameNote() const;

	void ShowSaveListEntry();
	void ShowEntriesEntry();
	void LoadSaveEntry(FString SaveName);

	FSimpleDelegate OnNewGame;
	FSimpleDelegate OnSettings;
	FSimpleDelegate OnQuit;
	FMassifOnLoadSave OnLoadSave;

	TArray<FMassifSaveListItem> Saves;

	/** Entries and the save list are two faces of one column, not two screens. */
	TSharedPtr<SWidgetSwitcher> Faces;

	TAttribute<FText> ElevationModelName;
};
