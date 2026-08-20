#include "Modules/ModuleManager.h"

// Shaders/ is reachable from material custom nodes as /Project without any
// mapping of its own: the engine maps it for every project that has the folder.
// Shaders/MassifTerrainSurface.ush is included that way.

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, MassifPresentation, "MassifPresentation");
