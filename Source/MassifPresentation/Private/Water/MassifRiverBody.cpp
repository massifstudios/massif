#include "MassifRiverBody.h"

AMassifRiverBody::AMassifRiverBody(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Read by InitializeBody, which runs from PostInitProperties after this.
	WaterBodyRiverComponentClass = UMassifRiverBodyComponent::StaticClass();
}
