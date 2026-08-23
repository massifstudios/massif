#include "MassifEnvironmentProcess.h"

#include "MassifEnvironmentState.h"
#include "MassifSeed.h"

const TCHAR* MassifFidelityName(EMassifFidelity Fidelity)
{
	switch (Fidelity)
	{
		case EMassifFidelity::Off: return TEXT("Off");
		case EMassifFidelity::Cheap: return TEXT("Cheap");
		case EMassifFidelity::Standard: return TEXT("Standard");
		case EMassifFidelity::Reference: return TEXT("Reference");
		default: return TEXT("Unknown");
	}
}

bool FMassifProcessDeclaration::SupportsFidelity(EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		return true;
	}
	return SupportedFidelities.Contains(Fidelity);
}

bool IMassifEnvironmentProcess::ExecuteOnGpu(FMassifEnvironmentContext&, EMassifFidelity) const
{
	return false;
}

FMassifEnvironmentContext::FMassifEnvironmentContext(
	FMassifEnvironmentState& InState,
	const IMassifElevationSource& InElevation,
	const FMassifGeoreference& InGeoreference,
	uint32 InMasterSeed,
	const FMassifProcessDeclaration& InDeclaration)
	: State(InState)
	, Elevation(InElevation)
	, Georeference(InGeoreference)
	, Declaration(InDeclaration)
	, MasterSeed(InMasterSeed)
{
}

FMassifChannelHandle FMassifEnvironmentContext::GetInput(FName ChannelName) const
{
	checkf(
		Declaration.InputChannels.Contains(ChannelName),
		TEXT("Process %s read channel %s without declaring it as an input"),
		*Declaration.Name.ToString(), *ChannelName.ToString());

	const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
	checkf(
		Channel.IsValid(), TEXT("Process %s declared input channel %s, which no process produces"),
		*Declaration.Name.ToString(), *ChannelName.ToString());
	return Channel;
}

FMassifChannelHandle FMassifEnvironmentContext::FindOptionalInput(FName ChannelName) const
{
	checkf(
		Declaration.OptionalInputChannels.Contains(ChannelName),
		TEXT("Process %s read channel %s without declaring it as an optional input"),
		*Declaration.Name.ToString(), *ChannelName.ToString());
	return State.FindChannel(ChannelName);
}

FMassifChannelHandle FMassifEnvironmentContext::GetOutput(FName ChannelName) const
{
	const bool bDeclared = Declaration.OutputChannels.ContainsByPredicate(
		[ChannelName](const FMassifChannelDescriptor& Descriptor)
		{
			return Descriptor.Name == ChannelName;
		});

	checkf(
		bDeclared, TEXT("Process %s wrote channel %s without declaring it as an output"),
		*Declaration.Name.ToString(), *ChannelName.ToString());

	const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
	checkf(
		Channel.IsValid(), TEXT("Output channel %s of process %s was never registered"),
		*ChannelName.ToString(), *Declaration.Name.ToString());
	return Channel;
}

TUniquePtr<FMassifChannelWriteScope> FMassifEnvironmentContext::OpenOutput(FName ChannelName, int32 TimeSlice) const
{
	const FMassifChannelHandle Channel = GetOutput(ChannelName);
	return MakeUnique<FMassifChannelWriteScope>(State, Channel, TimeSlice);
}

FString FMassifEnvironmentContext::MakeStreamName(const TCHAR* Purpose) const
{
	return FString::Printf(TEXT("%s.%s"), *Declaration.Name.ToString(), Purpose);
}

FRandomStream FMassifEnvironmentContext::MakeStream(const TCHAR* Purpose) const
{
	return FMassifSeed(MasterSeed).MakeChannelStream(*MakeStreamName(Purpose));
}

uint32 FMassifEnvironmentContext::MakeSeed(const TCHAR* Purpose) const
{
	return FMassifSeed(MasterSeed).MakeChannelSeed(*MakeStreamName(Purpose));
}

void MassifFillChannelSlice(
	const FMassifEnvironmentContext& Context, FName ChannelName, int32 TimeSlice, double Value)
{
	const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
	TArrayView<double> Values = Scope->GetComponent(0);
	for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
	{
		Values[CellIndex] = Value;
	}
	Scope->Commit();
}
