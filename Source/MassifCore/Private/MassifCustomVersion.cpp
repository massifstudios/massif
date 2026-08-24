#include "MassifCustomVersion.h"

#include "Serialization/CustomVersion.h"

const FGuid FMassifCustomVersion::GUID(0x76BAB9FA, 0x253A48C7, 0x833259B8, 0xDD9DD4FA);

FCustomVersionRegistration GRegisterMassifCustomVersion(
	FMassifCustomVersion::GUID, FMassifCustomVersion::LatestVersion, TEXT("MassifVersion"));
