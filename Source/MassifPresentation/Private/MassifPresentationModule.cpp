#include "Modules/ModuleManager.h"
#include "UI/Style/MassifStyle.h"

// Shaders/ is reachable from material custom nodes as /Project without any
// mapping of its own: the engine maps it for every project that has the folder.
// Shaders/MassifTerrainSurface.ush is included that way.

class FMassifPresentationModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		MassifStyle::Register();
	}

	// The style set owns its brushes, so leaving it registered across a reload would
	// leak them and leave the registry pointing at a set that no longer exists.
	virtual void ShutdownModule() override
	{
		MassifStyle::Unregister();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMassifPresentationModule, MassifPresentation, "MassifPresentation");
